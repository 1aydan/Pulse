// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseMemoryAnalyzer.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/AssetManager.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameMapsSettings.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Pulse.h"
#include "PulseTagUtils.h"

#define LOCTEXT_NAMESPACE "PulseMemoryAnalyzer"

/** Native packages are code, not content: they cost nothing the registry can describe. */
static bool PulseIsScriptPackage(FName PackageName)
{
	TCHAR Buffer[FName::StringBufferSize];
	PackageName.ToString(Buffer);
	return FCString::Strnicmp(Buffer, TEXT("/Script/"), 8) == 0;
}

/** Deterministic dependency order: dependency lists come back in registry order, which is not stable. */
static void PulseSortPackageNames(TArray<FName>& Names)
{
	Names.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
}

FPulseMemoryAnalyzer::FPulseMemoryAnalyzer(const FPulseMemoryAuditorConfig& InConfig)
	: Config(InConfig)
	, Estimator(InConfig.Estimation)
{
}

bool FPulseMemoryAnalyzer::IsInScanScope(FName PackageName) const
{
	const FString Package = PackageName.ToString();

	bool bUnderScanPath = Config.PathFilter.Num() == 0;
	for (const FString& Path : Config.PathFilter)
	{
		// Directory semantics: /Game/Maps must not match /Game/MapsArchive.
		const FString Prefix = Path.EndsWith(TEXT("/")) ? Path : Path + TEXT("/");
		if (Package.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			bUnderScanPath = true;
			break;
		}
	}
	if (!bUnderScanPath)
	{
		return false;
	}

	for (const FString& Pattern : Config.ExcludePathPatterns)
	{
		if (!Pattern.IsEmpty() && Package.Contains(Pattern, ESearchCase::IgnoreCase))
		{
			return false;
		}
	}
	return true;
}

void FPulseMemoryAnalyzer::GatherRoots(const IAssetRegistry& Registry, TArray<FRootCandidate>& OutRoots) const
{
	// Keyed by package so an asset that qualifies twice — a map that is also a primary asset — is
	// listed once, under the kind that explains it best (lowest enum value wins).
	TMap<FName, FRootCandidate> ByPackage;
	auto AddCandidate = [&ByPackage](FName PackageName, EPulseMemoryRootKind Kind, const FString& Detail)
	{
		if (PackageName.IsNone() || PulseIsScriptPackage(PackageName))
		{
			return;
		}
		FRootCandidate* Existing = ByPackage.Find(PackageName);
		if (Existing == nullptr)
		{
			ByPackage.Add(PackageName, FRootCandidate{ PackageName, Kind, Detail, Kind == EPulseMemoryRootKind::Inspected });
			return;
		}

		// An inspected asset keeps its stronger kind — a dropped map is still a map, and still a load
		// root — but remembers that it was asked for so the panel can pin it.
		Existing->bInspected |= Kind == EPulseMemoryRootKind::Inspected;
		if (static_cast<uint8>(Kind) < static_cast<uint8>(Existing->Kind))
		{
			Existing->Kind = Kind;
			Existing->Detail = Detail;
		}
	};

	auto AddAssetsOfClass = [this, &Registry, &AddCandidate](const FTopLevelAssetPath& ClassPath, EPulseMemoryRootKind Kind, const FString& Detail)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(ClassPath);
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		for (const FString& Path : Config.PathFilter)
		{
			Filter.PackagePaths.Add(FName(*Path));
		}

		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (IsInScanScope(Asset.PackageName))
			{
				AddCandidate(Asset.PackageName, Kind, Detail);
			}
		}
	};

	// Always-loaded assets are global: a GameMode under /Game/Core is resident in a map under
	// /Game/Levels whether or not /Game/Core is a scan path, so these skip the scope check.
	if (Config.Roots.bIncludeGameMapsSettings)
	{
		const FSoftObjectPath GameModePath(UGameMapsSettings::GetGlobalDefaultGameMode());
		AddCandidate(GameModePath.GetLongPackageFName(), EPulseMemoryRootKind::AlwaysLoaded, TEXT("Maps & Modes: Global Default GameMode"));

		const FSoftClassPath& GameInstancePath = GetDefault<UGameMapsSettings>()->GameInstanceClass;
		AddCandidate(GameInstancePath.GetLongPackageFName(), EPulseMemoryRootKind::AlwaysLoaded, TEXT("Maps & Modes: Game Instance Class"));
	}

	for (const FSoftObjectPath& AssetPath : Config.Roots.AlwaysLoadedAssets)
	{
		if (!AssetPath.IsNull())
		{
			AddCandidate(AssetPath.GetLongPackageFName(), EPulseMemoryRootKind::AlwaysLoaded, TEXT("AlwaysLoadedAssets"));
		}
	}

	if (Config.Roots.bIncludeMaps)
	{
		AddAssetsOfClass(UWorld::StaticClass()->GetClassPathName(), EPulseMemoryRootKind::Map, TEXT("Map"));
	}

	for (const FSoftClassPath& ClassPath : Config.Roots.AdditionalRootClasses)
	{
		if (!ClassPath.IsNull())
		{
			AddAssetsOfClass(ClassPath.GetAssetPath(), EPulseMemoryRootKind::Configured, ClassPath.GetAssetName());
		}
	}

	if (Config.Roots.bIncludeBlueprints)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		for (const FString& Path : Config.PathFilter)
		{
			Filter.PackagePaths.Add(FName(*Path));
		}

		TArray<FAssetData> Blueprints;
		Registry.GetAssets(Filter, Blueprints);
		for (const FAssetData& Blueprint : Blueprints)
		{
			// Editor Utility Blueprints and Widgets are tooling and never ship. Matched by name because
			// their classes live in Blutility, which a runtime-facing audit should not depend on.
			const FString ClassName = Blueprint.AssetClassPath.GetAssetName().ToString();
			if (ClassName.StartsWith(TEXT("EditorUtility")) || !IsInScanScope(Blueprint.PackageName))
			{
				continue;
			}
			AddCandidate(Blueprint.PackageName, EPulseMemoryRootKind::Blueprint, ClassName);
		}
	}

	// Inspected assets skip the scope check: naming one is the request, wherever it lives.
	for (const FString& Path : Config.InspectAssets)
	{
		const FName PackageName = Path.Contains(TEXT("."))
			? FSoftObjectPath(Path).GetLongPackageFName()
			: FName(*Path);
		AddCandidate(PackageName, EPulseMemoryRootKind::Inspected, TEXT("Inspected"));
	}

	if (Config.Roots.bIncludePrimaryAssets)
	{
		if (UAssetManager::IsInitialized())
		{
			UAssetManager& AssetManager = UAssetManager::Get();
			TArray<FPrimaryAssetTypeInfo> TypeInfos;
			AssetManager.GetPrimaryAssetTypeInfoList(TypeInfos);
			int32 NumTypes = 0;
			int32 NumIds = 0;
			int32 NumInScope = 0;
			for (const FPrimaryAssetTypeInfo& TypeInfo : TypeInfos)
			{
				// Editor-only types never ship, so what they reference is not a runtime cost.
				if (TypeInfo.bIsEditorOnly)
				{
					continue;
				}
				NumTypes++;

				TArray<FPrimaryAssetId> Ids;
				AssetManager.GetPrimaryAssetIdList(TypeInfo.PrimaryAssetType, Ids);
				NumIds += Ids.Num();
				for (const FPrimaryAssetId& Id : Ids)
				{
					const FName PackageName = AssetManager.GetPrimaryAssetPath(Id).GetLongPackageFName();
					if (IsInScanScope(PackageName))
					{
						AddCandidate(PackageName, EPulseMemoryRootKind::PrimaryAsset, TypeInfo.PrimaryAssetType.ToString());
						NumInScope++;
					}
				}
			}

			// Said out loud because an empty result is ambiguous: no primary assets configured, none
			// under the scan paths, and an Asset Manager that has not scanned yet all look the same.
			UE_LOG(LogPulse, Display, TEXT("Pulse.Memory: %d primary asset type(s), %d primary asset(s), %d under the scan paths."),
				NumTypes, NumIds, NumInScope);
		}
		else
		{
			UE_LOG(LogPulse, Warning, TEXT("Pulse.Memory: the Asset Manager is not initialized, so primary assets are not included as top-level assets."));
		}
	}

	ByPackage.GenerateValueArray(OutRoots);
	OutRoots.Sort([](const FRootCandidate& A, const FRootCandidate& B) { return A.PackageName.LexicalLess(B.PackageName); });
}

int32 FPulseMemoryAnalyzer::FindOrAddNode(const IAssetRegistry& Registry, FName PackageName, FPulseMemoryReport& Report, TArray<int32>& Queue) const
{
	if (const int32* Existing = Report.NodeIndexByPackage.Find(PackageName))
	{
		return *Existing;
	}
	if (PulseIsScriptPackage(PackageName))
	{
		return INDEX_NONE;
	}

	TArray<FAssetData> Assets;
	Registry.GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyOnDiskAssets*/ true);
	const TOptional<FAssetPackageData> PackageData = Registry.GetAssetPackageDataCopy(PackageName);

	// A reference to a package the registry has never seen is a broken reference; nothing loads.
	if (Assets.Num() == 0 && !PackageData.IsSet())
	{
		return INDEX_NONE;
	}

	const int32 NodeIndex = Report.Nodes.AddDefaulted();
	FPulseMemoryNode& Node = Report.Nodes[NodeIndex];
	Node.PackageName = PackageName;

	if (const FAssetData* MainAsset = FPulseMemoryEstimator::PickMainAsset(Assets))
	{
		Node.AssetName = MainAsset->AssetName.ToString();
		Node.ObjectPath = MainAsset->GetSoftObjectPath().ToString();
		Node.ClassName = MainAsset->AssetClassPath.GetAssetName().ToString();
	}
	else
	{
		Node.AssetName = FPackageName::GetShortName(PackageName);
	}

	const FPulseMemoryEstimate Estimate = Estimator.EstimatePackage(Assets, PackageData.IsSet() ? &PackageData.GetValue() : nullptr);
	Node.Self = Estimate.Cost;
	Node.SizeSource = Estimate.Source;
	Node.SizeNote = Estimate.Note;

	Report.NodeIndexByPackage.Add(PackageName, NodeIndex);
	Queue.Add(NodeIndex);
	return NodeIndex;
}

void FPulseMemoryAnalyzer::ExpandNode(const IAssetRegistry& Registry, int32 NodeIndex, FPulseMemoryReport& Report, TArray<int32>& Queue) const
{
	const FName PackageName = Report.Nodes[NodeIndex].PackageName;

	// Game dependencies only. Editor-only references (thumbnails, editor utility widgets, an actor's
	// sprite) never load in a cooked build, and counting them would blame a map for its tooling.
	TArray<FName> HardNames;
	Registry.GetDependencies(PackageName, HardNames, UE::AssetRegistry::EDependencyCategory::Package,
		UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard | UE::AssetRegistry::EDependencyQuery::Game));

	TArray<FName> SoftNames;
	if (Config.Estimation.bIncludeSoftReferences)
	{
		Registry.GetDependencies(PackageName, SoftNames, UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Soft | UE::AssetRegistry::EDependencyQuery::Game));
	}

	// One-file-per-actor maps keep their actors in separate packages the map does not list as
	// dependencies. A legacy (non-partitioned) level still loads every one of them with the map, so
	// they are hard; World Partition streams them per cell, so they are only ever "may load".
	if (Report.Nodes[NodeIndex].ClassName == TEXT("World"))
	{
		TArray<FAssetData> MapAssets;
		Registry.GetAssetsByPackageName(PackageName, MapAssets, /*bIncludeOnlyOnDiskAssets*/ true);
		const FAssetData* MapAsset = FPulseMemoryEstimator::PickMainAsset(MapAssets);

		bool bUsesExternalActors = false;
		bool bIsPartitioned = false;
		if (MapAsset != nullptr)
		{
			PulseGetTagBool(*MapAsset, TEXT("LevelIsUsingExternalActors"), bUsesExternalActors);
			PulseGetTagBool(*MapAsset, TEXT("LevelIsPartitioned"), bIsPartitioned);
		}

		if (bUsesExternalActors || bIsPartitioned)
		{
			TArray<FName> ActorPackages;
			for (const FString& ExternalActorsPath : ULevel::GetExternalActorsPaths(PackageName.ToString()))
			{
				TArray<FAssetData> ActorAssets;
				Registry.GetAssetsByPath(FName(*ExternalActorsPath), ActorAssets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ true);
				for (const FAssetData& ActorAsset : ActorAssets)
				{
					ActorPackages.AddUnique(ActorAsset.PackageName);
				}
			}

			if (bIsPartitioned)
			{
				if (Config.Estimation.bIncludeSoftReferences)
				{
					SoftNames.Append(ActorPackages);
				}
			}
			else
			{
				HardNames.Append(ActorPackages);
			}
		}
	}

	PulseSortPackageNames(HardNames);
	PulseSortPackageNames(SoftNames);

	TArray<int32> HardIndices;
	for (const FName& Name : HardNames)
	{
		const int32 DependencyIndex = FindOrAddNode(Registry, Name, Report, Queue);
		if (DependencyIndex != INDEX_NONE && DependencyIndex != NodeIndex)
		{
			HardIndices.AddUnique(DependencyIndex);
		}
	}

	TArray<int32> SoftIndices;
	for (const FName& Name : SoftNames)
	{
		const int32 DependencyIndex = FindOrAddNode(Registry, Name, Report, Queue);
		// A package referenced both ways is loaded by the hard reference; listing it as soft too would
		// count it under "may load" for something that will load regardless.
		if (DependencyIndex != INDEX_NONE && DependencyIndex != NodeIndex && !HardIndices.Contains(DependencyIndex))
		{
			SoftIndices.AddUnique(DependencyIndex);
		}
	}

	// Assigned last: FindOrAddNode grows Report.Nodes, so a reference taken earlier would dangle.
	Report.Nodes[NodeIndex].HardDependencies = MoveTemp(HardIndices);
	Report.Nodes[NodeIndex].SoftDependencies = MoveTemp(SoftIndices);
}

void FPulseMemoryAnalyzer::ComputeRetained(FPulseMemoryReport& Report)
{
	// Two trees, because one would let the question change the answer. With every Blueprint as a root,
	// anything a map shares with some Blueprint becomes reachable around the map, stops being dominated
	// by it, and drops out of the map's Unique — the map's cost would depend on how many Blueprints
	// happen to be listed. So load roots are measured alone, and the tree that adds inspection roots is
	// consulted only for packages no load root reaches at all.
	TArray<int32> LoadRoots;
	TArray<int32> AllRoots;
	for (const FPulseMemoryRoot& Root : Report.Roots)
	{
		if (PulseIsLoadRootKind(Root.Kind))
		{
			LoadRoots.AddUnique(Root.NodeIndex);
		}
		AllRoots.AddUnique(Root.NodeIndex);
	}

	TArray<FPulseMemoryCost> LoadRetained;
	TBitArray<> LoadReachable;
	Report.ComputeRetainedFrom(LoadRoots, LoadRetained, LoadReachable);

	TArray<FPulseMemoryCost> AllRetained;
	TBitArray<> AllReachable;
	if (AllRoots.Num() > LoadRoots.Num())
	{
		Report.ComputeRetainedFrom(AllRoots, AllRetained, AllReachable);
	}

	for (int32 NodeIndex = 0; NodeIndex < Report.Nodes.Num(); ++NodeIndex)
	{
		FPulseMemoryNode& Node = Report.Nodes[NodeIndex];
		if (LoadReachable[NodeIndex])
		{
			Node.Retained = LoadRetained[NodeIndex];
		}
		else if (AllReachable.Num() > 0 && AllReachable[NodeIndex])
		{
			Node.Retained = AllRetained[NodeIndex];
		}
		else
		{
			Node.Retained = Node.Self;
		}
	}
}

bool FPulseMemoryAnalyzer::Run(FPulseMemoryReport& OutReport, FString& OutError, const FProgressCallback& Progress)
{
	auto ReportProgress = [&Progress](const FText& Stage, float Fraction)
	{
		return !Progress || Progress(Stage, Fraction);
	};

	OutReport = FPulseMemoryReport();
	OutReport.GeneratedAtUtc = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	OutReport.EngineVersion = FEngineVersion::Current().ToString();
	OutReport.RootSettings = Config.Roots;
	OutReport.Estimation = Config.Estimation;
	OutReport.Budget = Config.Budget;
	OutReport.ScanPaths = Config.PathFilter;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& Registry = AssetRegistryModule.Get();
	Registry.SearchAllAssets(/*bSynchronousSearch*/ true);

	// The same drain the asset audit does before reading tags: UStaticMesh writes none while it is
	// compiling, and a mesh with no tags falls back to its disk size.
	FAssetCompilingManager::Get().FinishAllCompilation();

	if (!ReportProgress(LOCTEXT("StageRoots", "Finding top-level assets"), 0.0f))
	{
		OutError = TEXT("Cancelled.");
		return false;
	}

	TArray<FRootCandidate> Candidates;
	GatherRoots(Registry, Candidates);

	// Graph construction: breadth-first from every root, creating and sizing each package once.
	TArray<int32> Queue;
	for (const FRootCandidate& Candidate : Candidates)
	{
		const int32 NodeIndex = FindOrAddNode(Registry, Candidate.PackageName, OutReport, Queue);
		if (NodeIndex == INDEX_NONE)
		{
			UE_LOG(LogPulse, Warning, TEXT("Pulse.Memory: top-level asset '%s' (%s) is not in the Asset Registry; skipped."),
				*Candidate.PackageName.ToString(), *Candidate.Detail);
			continue;
		}

		FPulseMemoryRoot& Root = OutReport.Roots.AddDefaulted_GetRef();
		Root.NodeIndex = NodeIndex;
		Root.Kind = Candidate.Kind;
		Root.KindDetail = Candidate.Detail;
		Root.bInspected = Candidate.bInspected;
	}

	if (OutReport.Roots.Num() == 0)
	{
		OutError = TEXT("No top-level assets were found. Check the scan paths and the Roots settings.");
		return false;
	}

	for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
	{
		ExpandNode(Registry, Queue[QueueIndex], OutReport, Queue);

		if ((QueueIndex & 255) == 0)
		{
			const FText Stage = FText::Format(LOCTEXT("StageGraph", "Walking dependencies ({0} packages)"), FText::AsNumber(Queue.Num()));
			if (!ReportProgress(Stage, 0.6f * static_cast<float>(QueueIndex) / static_cast<float>(FMath::Max(Queue.Num(), 1))))
			{
				OutError = TEXT("Cancelled.");
				return false;
			}
		}
	}

	// Per-root totals. Each walk is discarded once summed; see FPulseMemoryReach for why.
	FPulseMemoryReach Reach;
	for (int32 RootIndex = 0; RootIndex < OutReport.Roots.Num(); ++RootIndex)
	{
		FPulseMemoryRoot& Root = OutReport.Roots[RootIndex];
		OutReport.ComputeReach(Root.NodeIndex, Config.Estimation.bIncludeSoftReferences, Reach);

		bool bPartitioned = false;
		Root.NumHardPackages = Reach.NumHard;
		Root.NumSoftOnlyPackages = Reach.Nodes.Num() - Reach.NumHard;

		for (int32 Slot = 0; Slot < Reach.Nodes.Num(); ++Slot)
		{
			FPulseMemoryNode& Node = OutReport.Nodes[Reach.Nodes[Slot]];
			if (Slot < Reach.NumHard)
			{
				Root.Total += Node.Self;
				Node.bReachedByHardReference = true;
				// Only load roots count here: "loaded by 40 Blueprints" would say nothing about how
				// widely the game actually loads a package.
				if (PulseIsLoadRootKind(Root.Kind))
				{
					Node.NumRootsReaching++;
				}
				if (Root.Kind == EPulseMemoryRootKind::AlwaysLoaded)
				{
					Node.bInBaseline = true;
				}
				if (Node.SizeSource == EPulseMemorySizeSource::DiskSize)
				{
					Root.ResidentBytesFromDiskSize += Node.Self.GetResidentBytes();
				}
				else if (Node.SizeSource == EPulseMemorySizeSource::Unknown)
				{
					Root.NumUnknownSize++;
				}
			}
			else
			{
				Root.MayLoad += Node.Self;
			}
		}

		const FPulseMemoryNode& RootNode = OutReport.Nodes[Root.NodeIndex];
		if (RootNode.ClassName == TEXT("World"))
		{
			TArray<FAssetData> MapAssets;
			Registry.GetAssetsByPackageName(RootNode.PackageName, MapAssets, /*bIncludeOnlyOnDiskAssets*/ true);
			if (const FAssetData* MapAsset = FPulseMemoryEstimator::PickMainAsset(MapAssets))
			{
				PulseGetTagBool(*MapAsset, TEXT("LevelIsPartitioned"), bPartitioned);
			}
		}
		Root.bWorldPartition = bPartitioned;

		if ((RootIndex & 15) == 0)
		{
			const FText Stage = FText::Format(LOCTEXT("StageTotals", "Totalling {0} of {1} top-level assets"),
				FText::AsNumber(RootIndex + 1), FText::AsNumber(OutReport.Roots.Num()));
			if (!ReportProgress(Stage, 0.6f + 0.3f * static_cast<float>(RootIndex) / static_cast<float>(OutReport.Roots.Num())))
			{
				OutError = TEXT("Cancelled.");
				return false;
			}
		}
	}

	if (!ReportProgress(LOCTEXT("StageRetained", "Computing unique costs"), 0.9f))
	{
		OutError = TEXT("Cancelled.");
		return false;
	}

	ComputeRetained(OutReport);

	for (FPulseMemoryRoot& Root : OutReport.Roots)
	{
		Root.Unique = OutReport.Nodes[Root.NodeIndex].Retained;
	}

	for (const FPulseMemoryNode& Node : OutReport.Nodes)
	{
		if (Node.bInBaseline)
		{
			OutReport.Baseline += Node.Self;
			OutReport.NumBaselinePackages++;
		}
		if (Node.bReachedByHardReference)
		{
			OutReport.NumReachablePackages++;
		}
	}

	ReportProgress(LOCTEXT("StageDone", "Done"), 1.0f);
	return true;
}

#undef LOCTEXT_NAMESPACE
