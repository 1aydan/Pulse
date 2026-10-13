// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseMemoryAnalyzer.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/AssetManager.h"
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
			ByPackage.Add(PackageName, FRootCandidate{ PackageName, Kind, Detail });
		}
		else if (static_cast<uint8>(Kind) < static_cast<uint8>(Existing->Kind))
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
	// Cooper, Harvey & Kennedy, "A Simple, Fast Dominance Algorithm". A virtual root (index N) points
	// at every top-level asset, so a package's immediate dominator is the closest node every load path
	// to it passes through. Its retained cost is then its subtree in the dominator tree: what would
	// leave memory if that one package stopped loading.
	const int32 NumNodes = Report.Nodes.Num();
	const int32 VirtualRoot = NumNodes;

	TArray<int32> RootSuccessors;
	for (const FPulseMemoryRoot& Root : Report.Roots)
	{
		RootSuccessors.AddUnique(Root.NodeIndex);
	}

	auto GetSuccessors = [&Report, &RootSuccessors, VirtualRoot](int32 Node) -> const TArray<int32>&
	{
		return Node == VirtualRoot ? RootSuccessors : Report.Nodes[Node].HardDependencies;
	};

	// Iterative DFS for a postorder: a deep reference chain must not be able to overflow the stack.
	TArray<int32> PostOrderNumber;
	PostOrderNumber.Init(INDEX_NONE, NumNodes + 1);
	TArray<int32> PostOrder;
	PostOrder.Reserve(NumNodes + 1);
	{
		TBitArray<> Discovered(false, NumNodes + 1);
		TArray<TPair<int32, int32>> Stack;
		Stack.Emplace(VirtualRoot, 0);
		Discovered[VirtualRoot] = true;
		while (Stack.Num() > 0)
		{
			TPair<int32, int32>& Top = Stack.Last();
			const TArray<int32>& Successors = GetSuccessors(Top.Key);
			if (Top.Value < Successors.Num())
			{
				const int32 Next = Successors[Top.Value++];
				if (!Discovered[Next])
				{
					Discovered[Next] = true;
					Stack.Emplace(Next, 0);
				}
			}
			else
			{
				PostOrderNumber[Top.Key] = PostOrder.Num();
				PostOrder.Add(Top.Key);
				Stack.Pop(EAllowShrinking::No);
			}
		}
	}

	// Predecessors, restricted to reachable nodes.
	TArray<TArray<int32>> Predecessors;
	Predecessors.SetNum(NumNodes + 1);
	for (const int32 Node : PostOrder)
	{
		for (const int32 Successor : GetSuccessors(Node))
		{
			Predecessors[Successor].Add(Node);
		}
	}

	TArray<int32> ImmediateDominator;
	ImmediateDominator.Init(INDEX_NONE, NumNodes + 1);
	ImmediateDominator[VirtualRoot] = VirtualRoot;

	auto Intersect = [&ImmediateDominator, &PostOrderNumber](int32 Left, int32 Right)
	{
		while (Left != Right)
		{
			while (PostOrderNumber[Left] < PostOrderNumber[Right])
			{
				Left = ImmediateDominator[Left];
			}
			while (PostOrderNumber[Right] < PostOrderNumber[Left])
			{
				Right = ImmediateDominator[Right];
			}
		}
		return Left;
	};

	bool bChanged = true;
	while (bChanged)
	{
		bChanged = false;
		// Reverse postorder, skipping the virtual root, which is last in postorder.
		for (int32 Position = PostOrder.Num() - 2; Position >= 0; --Position)
		{
			const int32 Node = PostOrder[Position];
			int32 NewDominator = INDEX_NONE;
			for (const int32 Predecessor : Predecessors[Node])
			{
				if (ImmediateDominator[Predecessor] == INDEX_NONE)
				{
					continue;
				}
				NewDominator = NewDominator == INDEX_NONE ? Predecessor : Intersect(Predecessor, NewDominator);
			}
			if (NewDominator != INDEX_NONE && ImmediateDominator[Node] != NewDominator)
			{
				ImmediateDominator[Node] = NewDominator;
				bChanged = true;
			}
		}
	}

	// A node's dominator is always its DFS ancestor and so later in postorder: walking postorder
	// forward finishes every subtree before its total is pushed up to the parent.
	for (FPulseMemoryNode& Node : Report.Nodes)
	{
		Node.Retained = Node.Self;
	}
	for (const int32 Node : PostOrder)
	{
		if (Node == VirtualRoot)
		{
			continue;
		}
		const int32 Dominator = ImmediateDominator[Node];
		if (Dominator != INDEX_NONE && Dominator != VirtualRoot)
		{
			Report.Nodes[Dominator].Retained += Report.Nodes[Node].Retained;
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
				Node.NumRootsReaching++;
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
		if (Node.NumRootsReaching > 0)
		{
			OutReport.NumReachablePackages++;
		}
	}

	ReportProgress(LOCTEXT("StageDone", "Done"), 1.0f);
	return true;
}

#undef LOCTEXT_NAMESPACE
