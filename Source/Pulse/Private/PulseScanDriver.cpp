// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseScanDriver.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"
#include "Pulse.h"
#include "PulseAssetEnumerator.h"
#include "PulseCollector.h"
#include "PulseCollectorRegistry.h"
#include "PulseScoring.h"
#include "PulseSettings.h"
#include "PulseShaderStatsBatch.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"
#include "WorldPartition/WorldPartitionHelpers.h"

FPulseScanDriver::FPulseScanDriver(const FPulseRunConfig& InConfig, const UPulseSettings& InSettings)
	: Config(InConfig)
	, Settings(&InSettings)
{
}

// Out-of-line so TUniquePtr<FPulseShaderStatsBatch> can destroy a forward-declared type.
FPulseScanDriver::~FPulseScanDriver() = default;

bool FPulseScanDriver::Initialize(FString& OutError)
{
	if (bInitialized)
	{
		return true;
	}

	TArray<IPulseCollector*> Collectors = FPulseCollectorRegistry::GetAllSorted();
	if (!Config.CategoryFilter.IsEmpty())
	{
		Collectors.RemoveAll([this](const IPulseCollector* Collector)
		{
			return !Config.CategoryFilter.ContainsByPredicate([Collector](const FString& Category)
			{
				return Category.Equals(Collector->GetCategory().ToString(), ESearchCase::IgnoreCase);
			});
		});
	}

	if (Collectors.IsEmpty())
	{
		OutError = Config.CategoryFilter.IsEmpty()
			? FString(TEXT("No collectors are registered."))
			: FString::Printf(TEXT("No collectors match -category=%s."), *FString::Join(Config.CategoryFilter, TEXT(",")));
		return false;
	}

	// Categories switched off in project settings drop out here, after the run's own filter, so a
	// disabled category is never enumerated, scored, or written — not even as an empty section.
	// Removal is announced: a silently absent category reads as a category with nothing wrong.
	TArray<FString> SkippedCategories;
	Collectors.RemoveAll([this, &SkippedCategories](const IPulseCollector* Collector)
	{
		const FName Category = Collector->GetCategory();
		if (Settings->IsCategoryEnabled(Category))
		{
			return false;
		}
		SkippedCategories.AddUnique(Category.ToString());
		return true;
	});

	if (!SkippedCategories.IsEmpty())
	{
		SkippedCategories.Sort([](const FString& A, const FString& B)
		{
			return A.Compare(B, ESearchCase::CaseSensitive) < 0;
		});
		UE_LOG(LogPulse, Display, TEXT("Pulse: skipping %s — disabled in Pulse settings (DisabledCategories)."),
			*FString::Join(SkippedCategories, TEXT(", ")));
	}

	if (Collectors.IsEmpty())
	{
		// Naming the disabled categories matters most here: someone who passed -category=Level and
		// got nothing needs to know the setting overrode them, not that the scan found no levels.
		OutError = FString::Printf(
			TEXT("Every matching category is disabled in Pulse settings (DisabledCategories): %s. Remove it there, or target a different category."),
			*FString::Join(SkippedCategories, TEXT(", ")));
		return false;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& Registry = AssetRegistryModule.Get();
	Registry.SearchAllAssets(/*bSynchronousSearch*/ true);
	AssetRegistry = &Registry;

	// Drain async compilation ONCE before any tag is read. UStaticMesh::GetAssetRegistryTags
	// early-outs while IsCompiling(), so without this a freshly-synced project silently reports
	// zero triangles for hundreds of meshes — a stale-tag lie, not an error.
	FAssetCompilingManager::Get().FinishAllCompilation();

	WorkQueue.Reserve(Collectors.Num());
	for (IPulseCollector* Collector : Collectors)
	{
		FPulseCollectorWork& Work = WorkQueue.AddDefaulted_GetRef();
		Work.Collector = Collector;
		Work.EffectiveTier = static_cast<EPulseTier>(FMath::Min(
			static_cast<uint8>(Config.RunTier),
			static_cast<uint8>(Collector->GetMaxUsefulTier())));

		bool bCollectorTruncated = false;
		FPulseAssetEnumerator::EnumerateForCollector(Registry, *Collector, Config, *Settings, Work.Assets, bCollectorTruncated);
		bTruncated |= bCollectorTruncated;
		AssetsTotal += Work.Assets.Num();

		TArray<FPulseRuleDesc> Rules;
		Collector->GetRuleSchema(Rules);
		for (const FPulseRuleDesc& Rule : Rules)
		{
			Work.RuleById.Add(Rule.RuleId, Rule);
		}

		// Seed the category now so schema and unevaluated rules are present even when the
		// collector matched zero assets — the CSV header must not depend on observed data.
		FPulseCategoryResult& Category = FindOrAddCategory(Collector->GetCategory());
		TArray<FPulseMetricDesc> Metrics;
		Collector->GetMetricSchema(Metrics);
		Category.MetricSchema.Append(Metrics);
		for (const FPulseRuleDesc& Rule : Rules)
		{
			if (Rule.MinTier > Work.EffectiveTier)
			{
				Category.UnevaluatedRuleIds.Add(Rule.RuleId);
			}
		}

		UE_LOG(LogPulse, Display, TEXT("Pulse: %s claims %d assets at tier %s%s."),
			*Collector->GetCollectorName().ToString(),
			Work.Assets.Num(),
			PulseTierToString(Work.EffectiveTier),
			bCollectorTruncated ? TEXT(" (truncated by -maxassets)") : TEXT(""));
	}

	// One batch object serves every ShaderStats work item; slice counts are summed up front so
	// the per-slice progress log can say "batch i/N" across collectors.
	int32 TotalShaderSlices = 0;
	for (const FPulseCollectorWork& Work : WorkQueue)
	{
		if (Work.EffectiveTier == EPulseTier::ShaderStats)
		{
			TotalShaderSlices += FMath::DivideAndRoundUp(Work.Assets.Num(), FMath::Max(1, Settings->Scan.ShaderStatsBatchSize));
		}
	}
	if (TotalShaderSlices > 0)
	{
		ShaderStatsBatch = MakeUnique<FPulseShaderStatsBatch>(Settings->Scan.ShaderStatsBatchSize, TotalShaderSlices);
	}

	bInitialized = true;
	return true;
}

bool FPulseScanDriver::Step(double TimeBudgetSeconds)
{
	if (!bInitialized || bCancelled)
	{
		return false;
	}

	const double Deadline = TimeBudgetSeconds > 0.0 ? FPlatformTime::Seconds() + TimeBudgetSeconds : 0.0;

	while (!bCancelled && CurrentWorkIndex < WorkQueue.Num())
	{
		FPulseCollectorWork& Work = WorkQueue[CurrentWorkIndex];
		if (Work.NextAssetIndex >= Work.Assets.Num())
		{
			++CurrentWorkIndex;
			continue;
		}

		ProcessOneAsset(Work);

		if (Deadline > 0.0 && FPlatformTime::Seconds() >= Deadline)
		{
			break;
		}
	}

	if (IsComplete())
	{
		// Final sweep so a Deep run does not park its last GC interval's packages in memory for
		// the rest of the editor session. No-op when nothing was loaded.
		MaybeCollectGarbage(/*bForce*/ true);
		return false;
	}
	return true;
}

void FPulseScanDriver::Cancel()
{
	bCancelled = true;
	MaybeCollectGarbage(/*bForce*/ true);
}

bool FPulseScanDriver::IsComplete() const
{
	return bInitialized && (bCancelled || AssetsProcessed >= AssetsTotal);
}

FPulseScanProgress FPulseScanDriver::GetProgress() const
{
	FPulseScanProgress Progress;
	Progress.AssetsProcessed = AssetsProcessed;
	Progress.AssetsTotal = AssetsTotal;
	for (int32 WorkIndex = CurrentWorkIndex; WorkIndex < WorkQueue.Num(); ++WorkIndex)
	{
		const FPulseCollectorWork& Work = WorkQueue[WorkIndex];
		if (Work.NextAssetIndex < Work.Assets.Num())
		{
			Progress.CurrentCategory = Work.Collector->GetCategory();
			Progress.CurrentAssetPath = Work.Assets[Work.NextAssetIndex].GetSoftObjectPath().ToString();
			break;
		}
	}
	return Progress;
}

void FPulseScanDriver::ProcessOneAsset(FPulseCollectorWork& Work)
{
	const FAssetData& AssetData = Work.Assets[Work.NextAssetIndex];

	// Pre-warm the upcoming slice's shader maps before touching its first asset, so every
	// material in the slice compiles across the worker pool at once and stays loaded through its
	// own Collect calls below.
	if (Work.EffectiveTier == EPulseTier::ShaderStats && ShaderStatsBatch.IsValid())
	{
		const int32 BatchSize = ShaderStatsBatch->GetBatchSize();
		if ((Work.NextAssetIndex % BatchSize) == 0)
		{
			ShaderStatsBatch->PrewarmSlice(TArrayView<const FAssetData>(Work.Assets).Mid(Work.NextAssetIndex, BatchSize));
		}
	}

	EPulseTier Achieved = EPulseTier::Fast;
	UObject* Loaded = nullptr;
	if (Work.EffectiveTier >= EPulseTier::Deep)
	{
		if (Work.Collector->GetDeepLoadMode() == EPulseLoadMode::Driver)
		{
			Loaded = AssetData.GetAsset();
			if (Loaded != nullptr)
			{
				Achieved = Work.EffectiveTier;
				++PackagesTouchedSinceGC;
			}
			else
			{
				UE_LOG(LogPulse, Warning, TEXT("Failed to load %s; auditing it at Fast tier only."), *AssetData.GetSoftObjectPath().ToString());
			}
		}
		else
		{
			// SelfManaged: the collector performs its own scoped load inside Collect, but the
			// driver still owns the GC cadence, so the load counts against the interval here.
			Achieved = Work.EffectiveTier;
			++PackagesTouchedSinceGC;
		}
	}

	FPulseCollectContext Context{ AssetData, Loaded, *Settings, AssetRegistry, Achieved, Config.RunTier };

	FPulseAssetResult Result;
	Result.AssetPath = AssetData.GetSoftObjectPath();
	Result.ClassPath = AssetData.AssetClassPath;
	Result.Category = Work.Collector->GetCategory();
	Result.AchievedTier = Achieved;

	Work.Collector->CollectFast(Context, Result);
	if (Achieved >= EPulseTier::Deep)
	{
		Work.Collector->CollectDeep(Context, Result);
	}
	if (Achieved == EPulseTier::ShaderStats)
	{
		Work.Collector->CollectShaderStats(Context, Result);
	}
	Work.Collector->Evaluate(Context, Result);

	// Post-process: settings-driven disables and severity overrides, recommendation text from the
	// rule schema (authored once, never by the collector), and the achieved-tier stamp.
	for (int32 IssueIndex = Result.Issues.Num() - 1; IssueIndex >= 0; --IssueIndex)
	{
		FPulseIssue& Issue = Result.Issues[IssueIndex];
		if (Settings->IsRuleDisabled(Issue.RuleId))
		{
			Result.Issues.RemoveAt(IssueIndex);
			continue;
		}
		if (const EPulseSeverity* SeverityOverride = Settings->RuleSeverityOverrides.Find(Issue.RuleId))
		{
			Issue.Severity = *SeverityOverride;
		}
		EPulseTier RuleMinTier = EPulseTier::Fast;
		if (const FPulseRuleDesc* Rule = Work.RuleById.Find(Issue.RuleId))
		{
			if (Rule->Recommendation != nullptr)
			{
				Issue.Recommendation = Rule->Recommendation;
			}
			RuleMinTier = Rule->MinTier;
		}

		// Collectors that know better (the WP census fires at Fast even inside a Deep run) set the
		// tier themselves; this only raises a defaulted Fast to the rule's declared floor and caps
		// everything at what the asset actually reached.
		Issue.DetectedAtTier = FMath::Min(Achieved, FMath::Max(Issue.DetectedAtTier, RuleMinTier));
	}

	FindOrAddCategory(Result.Category).Assets.Add(MoveTemp(Result));

	// Map packages are enormous and force a GC regardless of the interval. The Level category is
	// included because a SelfManaged world load touches the map package without PKG_ContainsMap
	// ever crossing the driver's hands.
	bLastPackageWasMap = ((AssetData.PackageFlags & PKG_ContainsMap) != 0)
		|| Work.Collector->GetCategory() == FName(TEXT("Level"));

	++Work.NextAssetIndex;
	++AssetsProcessed;

	MaybeCollectGarbage(/*bForce*/ false);
}

void FPulseScanDriver::MaybeCollectGarbage(bool bForce)
{
	// Nothing loaded since the last sweep means nothing to reclaim: a Fast run never pays a GC.
	if (PackagesTouchedSinceGC <= 0)
	{
		bLastPackageWasMap = false;
		return;
	}

	// Three triggers, cheapest test first: the map package just processed, the package interval,
	// then the memory watchdog.
	const bool bShouldCollect = bForce
		|| bLastPackageWasMap
		|| PackagesTouchedSinceGC >= Config.GCPackageInterval
		|| FWorldPartitionHelpers::HasExceededMaxMemory();
	if (!bShouldCollect)
	{
		return;
	}

	// A still-compiling mesh holds its package alive, so GC without this drain reclaims far less
	// than expected and the run's memory floor ratchets upward.
	FAssetCompilingManager::Get().FinishAllCompilation();
	CollectGarbage(RF_NoFlags);

	PackagesTouchedSinceGC = 0;
	bLastPackageWasMap = false;
}

FPulseCategoryResult& FPulseScanDriver::FindOrAddCategory(FName Category)
{
	for (FPulseCategoryResult& Existing : Report.Categories)
	{
		if (Existing.Category == Category)
		{
			return Existing;
		}
	}
	FPulseCategoryResult& Added = Report.Categories.AddDefaulted_GetRef();
	Added.Category = Category;
	return Added;
}

FPulseReport& FPulseScanDriver::FinalizeReport()
{
	if (bFinalized)
	{
		return Report;
	}

	// Case-SENSITIVE ToString compare, never FName operator< — that is name-table index order and
	// varies per process. Scoring sorts each category's contents itself, then everything below the
	// category level accumulates in a fixed order and the scores are bit-identical between runs.
	Report.Categories.Sort([](const FPulseCategoryResult& A, const FPulseCategoryResult& B)
	{
		return A.Category.ToString().Compare(B.Category.ToString(), ESearchCase::CaseSensitive) < 0;
	});

	FPulseScoring::Finalize(Report, Settings->Scoring);
	BuildRunHeader();

	bFinalized = true;
	return Report;
}

void FPulseScanDriver::BuildRunHeader()
{
	FPulseRunHeader& Header = Report.Header;

	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Pulse")))
	{
		Header.PulseVersion = Plugin->GetDescriptor().VersionName;
	}
	Header.EngineVersion = FEngineVersion::Current().ToString();
	Header.ProjectName = FApp::GetProjectName();

	Header.RunTier = Config.RunTier;
	Header.bDeepRequested = Config.bDeepRequested;
	Header.bShaderStatsRequested = Config.bShaderStatsRequested;
	Header.bTruncated = bTruncated;
	Header.MinSeverityFilter = PulseSeverityToString(Config.MinSeverity);
	Header.CategoryFilter = Config.CategoryFilter;
	Header.PathFilter = Config.PathFilter;
	Header.SettingsHash = Settings->ComputeSettingsHash();

	if (ShaderStatsBatch.IsValid() && !ShaderStatsBatch->IsAvailable())
	{
		Header.ShaderStatsUnavailableReason = ShaderStatsBatch->GetUnavailableReason();
	}

	// The ONLY timestamp in report content, so unchanged content diffs to exactly this line.
	Header.GeneratedAtUtc = FDateTime::UtcNow().ToIso8601();
}
