// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseLevelCollector.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorWorldUtils.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Pulse.h"
#include "PulseCollectorRegistry.h"
#include "PulseSettings.h"
#include "PulseTagUtils.h"
#include "UObject/Package.h"
#include "WorldPartition/WorldPartitionActorDescUtils.h"

// Metric, tag, and rule names. Behind a Meyers singleton because FName construction must not run at
// static-init time — the same reason TPulseAutoCollector defers collector construction to
// StartupModule.
struct FPulseLevelNames
{
	FName IsPartitioned = FName(TEXT("IsPartitioned"));
	FName UsesExternalActors = FName(TEXT("UsesExternalActors"));
	FName StreamingDisabled = FName(TEXT("StreamingDisabled"));
	FName ActorCount = FName(TEXT("ActorCount"));
	FName ActorCountSource = FName(TEXT("ActorCountSource"));

	FName TagLevelIsPartitioned = FName(TEXT("LevelIsPartitioned"));
	FName TagLevelIsUsingExternalActors = FName(TEXT("LevelIsUsingExternalActors"));
	FName TagLevelHasStreamingDisabled = FName(TEXT("LevelHasStreamingDisabled"));

	FName RuleExcessiveActors = FName(TEXT("Level.ExcessiveActors"));
	FName RulePartitionExpected = FName(TEXT("Level.PartitionExpected"));
	FName RuleStreamingDisabled = FName(TEXT("Level.StreamingDisabled"));
};

static const FPulseLevelNames& PulseLevelNames()
{
	static const FPulseLevelNames GPulseLevelNamesInstance;
	return GPulseLevelNamesInstance;
}

static FPulseMetricDesc PulseLevelMetricDesc(FName Name, EPulseMetricKind Kind, EPulseMetricUnit Unit, EPulseTier MinTier, const TCHAR* Description)
{
	FPulseMetricDesc Desc;
	Desc.Name = Name;
	Desc.Kind = Kind;
	Desc.Unit = Unit;
	Desc.MinTier = MinTier;
	Desc.Description = Description;
	return Desc;
}

static FPulseRuleDesc PulseLevelRuleDesc(FName RuleId, EPulseSeverity DefaultSeverity, EPulseTier MinTier, const TCHAR* Description, const TCHAR* Recommendation)
{
	FPulseRuleDesc Desc;
	Desc.RuleId = RuleId;
	Desc.DefaultSeverity = DefaultSeverity;
	Desc.MinTier = MinTier;
	Desc.Description = Description;
	Desc.Recommendation = Recommendation;
	return Desc;
}

/** Counts non-null PersistentLevel actors and emits the Deep-tier count + provenance metrics. */
static void PulseEmitPersistentActorCount(const UWorld* World, FPulseAssetResult& InOutResult)
{
	const FPulseLevelNames& Names = PulseLevelNames();

	if (World->PersistentLevel == nullptr)
	{
		InOutResult.NumMissingExpectedTags++;
		UE_LOG(LogPulse, Warning, TEXT("Pulse.Level: world '%s' has no persistent level; emitting no actor count."), *World->GetPathName());
		return;
	}

	// Actors can contain nulls (deleted actors leave holes in the editor); count only live entries.
	int64 ActorCount = 0;
	for (const AActor* Actor : World->PersistentLevel->Actors)
	{
		if (Actor != nullptr)
		{
			ActorCount++;
		}
	}

	InOutResult.Metrics.Add(FPulseMetric::MakeInt(Names.ActorCount, ActorCount, EPulseMetricUnit::Count, EPulseTier::Deep));
	InOutResult.Metrics.Add(FPulseMetric::MakeText(Names.ActorCountSource, TEXT("PersistentLevel"), EPulseTier::Deep));
}

FName FPulseLevelCollector::GetCollectorName() const
{
	return FName(TEXT("Pulse.Level"));
}

FName FPulseLevelCollector::GetCategory() const
{
	return FName(TEXT("Level"));
}

UClass* FPulseLevelCollector::GetSupportedClass() const
{
	return UWorld::StaticClass();
}

bool FPulseLevelCollector::SupportsSubclasses() const
{
	return false;
}

EPulseTier FPulseLevelCollector::GetMaxUsefulTier() const
{
	return EPulseTier::Deep;
}

EPulseLoadMode FPulseLevelCollector::GetDeepLoadMode() const
{
	return EPulseLoadMode::SelfManaged;
}

void FPulseLevelCollector::GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const
{
	const FPulseLevelNames& Names = PulseLevelNames();

	OutMetrics.Add(PulseLevelMetricDesc(Names.IsPartitioned, EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("True when the map is a World Partition world (LevelIsPartitioned tag). The engine omits the tag entirely for legacy maps rather than writing 0, so absence reads as false.")));

	OutMetrics.Add(PulseLevelMetricDesc(Names.UsesExternalActors, EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("True when actors are stored one-file-per-actor outside the map package (LevelIsUsingExternalActors tag; absent means false).")));

	OutMetrics.Add(PulseLevelMetricDesc(Names.StreamingDisabled, EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("True when a World Partition map has streaming disabled (LevelHasStreamingDisabled tag; absent means false). Only partitioned maps ever write this tag.")));

	OutMetrics.Add(PulseLevelMetricDesc(Names.ActorCount, EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Fast,
		TEXT("Total actors in the map. Asymmetric by design: World Partition maps get it at Fast tier by counting external-actor descriptors in the Asset Registry with zero package loads, while non-partitioned maps store actors inside the map package, so their count needs a Deep-tier PersistentLevel walk and the column stays empty for them in a Fast run.")));

	OutMetrics.Add(PulseLevelMetricDesc(Names.ActorCountSource, EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Provenance of ActorCount: 'WorldPartitionDescriptors' (Fast registry census) or 'PersistentLevel' (Deep loaded-world walk), so the two counting methods are never confused.")));
}

void FPulseLevelCollector::GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const
{
	const FPulseLevelNames& Names = PulseLevelNames();

	OutRules.Add(PulseLevelRuleDesc(Names.RuleExcessiveActors, EPulseSeverity::Medium, EPulseTier::Fast,
		TEXT("ActorCount exceeds MaxActors (Medium), escalating to High past CriticalActors. Fires at Fast tier for World Partition maps and at Deep for legacy maps."),
		TEXT("Reduce the actor population: merge decorative actors via the Merge Actors tool or instanced static meshes, delete stale actors, or split the map into streaming sublevels / World Partition cells.")));

	OutRules.Add(PulseLevelRuleDesc(Names.RulePartitionExpected, EPulseSeverity::Low, EPulseTier::Fast,
		TEXT("A non-partitioned map holds more actors than PartitionExpectedAboveActors, so everything loads and stays resident together."),
		TEXT("Convert the map to World Partition (Tools > Convert Level) so actors stream per grid cell instead of all loading with the map.")));

	OutRules.Add(PulseLevelRuleDesc(Names.RuleStreamingDisabled, EPulseSeverity::Low, EPulseTier::Fast,
		TEXT("The World Partition map has streaming disabled, so every cell loads with the map and stays resident."),
		TEXT("Re-enable streaming in World Settings > World Partition Setup ('Enable Streaming') unless this map is genuinely small enough to hold fully resident.")));
}

void FPulseLevelCollector::CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FPulseLevelNames& Names = PulseLevelNames();

	// All three level tags are written as "1" when true and OMITTED entirely otherwise (see
	// UWorldPartition::AppendAssetRegistryTags and ULevel::GetIsLevelPartitionedFromAsset), so an
	// absent tag is a real false, not a stale registry. Read manually, always emit the metric, and
	// never count absence against the missing-tag trust rollup.
	bool bIsPartitioned = false;
	PulseGetTagBool(Context.AssetData, Names.TagLevelIsPartitioned, bIsPartitioned);
	InOutResult.Metrics.Add(FPulseMetric::MakeBool(Names.IsPartitioned, bIsPartitioned, EPulseTier::Fast));

	bool bUsesExternalActors = false;
	PulseGetTagBool(Context.AssetData, Names.TagLevelIsUsingExternalActors, bUsesExternalActors);
	InOutResult.Metrics.Add(FPulseMetric::MakeBool(Names.UsesExternalActors, bUsesExternalActors, EPulseTier::Fast));

	bool bStreamingDisabled = false;
	PulseGetTagBool(Context.AssetData, Names.TagLevelHasStreamingDisabled, bStreamingDisabled);
	InOutResult.Metrics.Add(FPulseMetric::MakeBool(Names.StreamingDisabled, bStreamingDisabled, EPulseTier::Fast));

	if (!bIsPartitioned)
	{
		// Legacy map: actors live inside the map package, invisible to the registry. Deep fills in.
		return;
	}

	if (Context.AssetRegistry == nullptr)
	{
		InOutResult.NumMissingExpectedTags++;
		UE_LOG(LogPulse, Warning, TEXT("Pulse.Level: no asset registry in context for '%s'; skipping the World Partition actor census."), *Context.AssetData.PackageName.ToString());
		return;
	}

	// World Partition census, with zero loads: every external actor is its own package under the
	// map's __ExternalActors__ folders, each carrying ActorMetaData tags in the registry. The
	// plural GetExternalActorsPaths also covers roots that plugins (game features) registered for
	// this map. bIncludeOnlyOnDiskAssets keeps the count stable regardless of what happens to be
	// in memory.
	int64 DescriptorCount = 0;
	const TArray<FString> ExternalActorsPaths = ULevel::GetExternalActorsPaths(Context.AssetData.PackageName.ToString());
	for (const FString& ExternalActorsPath : ExternalActorsPaths)
	{
		TArray<FAssetData> CandidateAssets;
		Context.AssetRegistry->GetAssetsByPath(FName(*ExternalActorsPath), CandidateAssets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ true);
		for (const FAssetData& CandidateAsset : CandidateAssets)
		{
			if (FWorldPartitionActorDescUtils::IsValidActorDescriptorFromAssetData(CandidateAsset))
			{
				DescriptorCount++;
			}
		}
	}

	InOutResult.Metrics.Add(FPulseMetric::MakeInt(Names.ActorCount, DescriptorCount, EPulseMetricUnit::Count, EPulseTier::Fast));
	InOutResult.Metrics.Add(FPulseMetric::MakeText(Names.ActorCountSource, TEXT("WorldPartitionDescriptors"), EPulseTier::Fast));
}

void FPulseLevelCollector::CollectDeep(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FPulseLevelNames& Names = PulseLevelNames();

	// World Partition maps already produced their census at Fast tier from descriptors; loading one
	// here would initialize world partition machinery for no new information. Deep is for the
	// legacy maps whose actors the registry cannot see.
	bool bIsPartitioned = false;
	InOutResult.GetBoolMetric(Names.IsPartitioned, bIsPartitioned);
	if (bIsPartitioned)
	{
		return;
	}

	const FString PackageNameString = Context.AssetData.PackageName.ToString();

	// SelfManaged load: worlds need LoadWorldPackageForEditor (which primes the world type before
	// UWorld::PostLoad runs), never a bare GetAsset(). The driver still counts this asset as a
	// package touch and GCs after every map, so no GC here.
	UPackage* Package = LoadWorldPackageForEditor(PackageNameString);
	if (Package == nullptr)
	{
		InOutResult.NumMissingExpectedTags++;
		UE_LOG(LogPulse, Warning, TEXT("Pulse.Level: failed to load world package '%s'; emitting no actor count."), *PackageNameString);
		return;
	}

	UWorld* World = UWorld::FindWorldInPackage(Package);
	if (World == nullptr)
	{
		InOutResult.NumMissingExpectedTags++;
		UE_LOG(LogPulse, Warning, TEXT("Pulse.Level: no UWorld found in package '%s'; emitting no actor count."), *PackageNameString);
		return;
	}

	if (World->IsInitialized())
	{
		// Already-initialized worlds — the map currently open when the editor panel drives a scan —
		// must not go through FScopedEditorWorld: its Init() checks !bIsWorldInitialized and its
		// destructor would tear the world down under the editor. The actor array is already
		// populated, so count it directly.
		PulseEmitPersistentActorCount(World, InOutResult);
		return;
	}

	// Everything a world can host is disabled: this scope exists only to make PersistentLevel's
	// actor array real. Mirrors UDerivedDataCacheCommandlet::CacheWorldPackages, minus even the
	// physics scene it keeps.
	UWorld::InitializationValues IVS;
	IVS.RequiresHitProxies(false);
	IVS.ShouldSimulatePhysics(false);
	IVS.EnableTraceCollision(false);
	IVS.CreateNavigation(false);
	IVS.CreateAISystem(false);
	IVS.AllowAudioPlayback(false);
	IVS.CreatePhysicsScene(false);

	FScopedEditorWorld ScopedWorld(World, IVS);
	if (ScopedWorld.GetWorld() == nullptr)
	{
		InOutResult.NumMissingExpectedTags++;
		UE_LOG(LogPulse, Warning, TEXT("Pulse.Level: editor world initialization failed for '%s'; emitting no actor count."), *PackageNameString);
		return;
	}

	PulseEmitPersistentActorCount(World, InOutResult);
}

void FPulseLevelCollector::Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FPulseLevelNames& Names = PulseLevelNames();
	const FPulseLevelThresholds& Thresholds = Context.Settings.Level;

	bool bIsPartitioned = false;
	InOutResult.GetBoolMetric(Names.IsPartitioned, bIsPartitioned);

	int64 ActorCount = 0;
	const bool bHasActorCount = InOutResult.GetIntMetric(Names.ActorCount, ActorCount);

	// The tier stamped on ActorCount — Fast for a descriptor census, Deep for a PersistentLevel
	// walk — is the tier the count-driven rules genuinely fired at.
	EPulseTier ActorCountTier = EPulseTier::Fast;
	if (const FPulseMetric* ActorCountMetric = InOutResult.FindMetric(Names.ActorCount))
	{
		ActorCountTier = ActorCountMetric->SourceTier;
	}

	// No ActorCount metric means the count is unknown (Fast run on a legacy map, or a failed load),
	// so the count rules simply do not fire — unknown must never evaluate as zero.
	if (bHasActorCount)
	{
		if (ActorCount > Thresholds.CriticalActors)
		{
			FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
			Issue.RuleId = Names.RuleExcessiveActors;
			Issue.Severity = EPulseSeverity::High;
			Issue.Message = FString::Printf(TEXT("%lld actors exceeds CriticalActors %d."), ActorCount, Thresholds.CriticalActors);
			Issue.DetectedAtTier = ActorCountTier;
		}
		else if (ActorCount > Thresholds.MaxActors)
		{
			FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
			Issue.RuleId = Names.RuleExcessiveActors;
			Issue.Severity = EPulseSeverity::Medium;
			Issue.Message = FString::Printf(TEXT("%lld actors exceeds MaxActors %d."), ActorCount, Thresholds.MaxActors);
			Issue.DetectedAtTier = ActorCountTier;
		}

		if (Thresholds.PartitionExpectedAboveActors > 0 && !bIsPartitioned && ActorCount > Thresholds.PartitionExpectedAboveActors)
		{
			FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
			Issue.RuleId = Names.RulePartitionExpected;
			Issue.Severity = EPulseSeverity::Low;
			Issue.Message = FString::Printf(TEXT("%lld actors in a non-partitioned map exceeds PartitionExpectedAboveActors %d."), ActorCount, Thresholds.PartitionExpectedAboveActors);
			Issue.DetectedAtTier = ActorCountTier;
		}
	}

	bool bStreamingDisabled = false;
	InOutResult.GetBoolMetric(Names.StreamingDisabled, bStreamingDisabled);
	if (Thresholds.bFlagStreamingDisabled && bStreamingDisabled)
	{
		FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
		Issue.RuleId = Names.RuleStreamingDisabled;
		Issue.Severity = EPulseSeverity::Low;
		Issue.Message = TEXT("World Partition streaming is disabled; every cell loads with the map and stays resident.");
		Issue.DetectedAtTier = EPulseTier::Fast;
	}
}

static TPulseAutoCollector<FPulseLevelCollector> GPulseLevelCollectorRegistrar;
