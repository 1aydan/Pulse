// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseMemoryReportWriter.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PulseCsvWriter.h"
#include "PulseMemoryResult.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/** Prefix for run folders. Also the prune filter, so nothing else in ReportDir is ever touched. */
static const TCHAR* GPulseMemoryRunPrefix = TEXT("MemoryAudit_");

static FString PulseFormatBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

static FString PulseFormatInt(int64 Value)
{
	return FString::Printf(TEXT("%lld"), Value);
}

/** Most resident first, then by object path. Case-sensitive so the order is total and reproducible. */
static bool PulseNodeLess(const FPulseMemoryReport& Report, int32 Left, int32 Right)
{
	const FPulseMemoryNode& A = Report.Nodes[Left];
	const FPulseMemoryNode& B = Report.Nodes[Right];
	if (A.Self.GetResidentBytes() != B.Self.GetResidentBytes())
	{
		return A.Self.GetResidentBytes() > B.Self.GetResidentBytes();
	}
	if (A.Self.StreamingBytes != B.Self.StreamingBytes)
	{
		return A.Self.StreamingBytes > B.Self.StreamingBytes;
	}
	return A.PackageName.ToString().Compare(B.PackageName.ToString(), ESearchCase::CaseSensitive) < 0;
}

TArray<int32> PulseSortRootsByResidentCost(const FPulseMemoryReport& Report)
{
	TArray<int32> Order;
	Order.Reserve(Report.Roots.Num());
	for (int32 Index = 0; Index < Report.Roots.Num(); ++Index)
	{
		Order.Add(Index);
	}
	Order.Sort([&Report](int32 Left, int32 Right)
	{
		const FPulseMemoryRoot& A = Report.Roots[Left];
		const FPulseMemoryRoot& B = Report.Roots[Right];
		if (A.Total.GetResidentBytes() != B.Total.GetResidentBytes())
		{
			return A.Total.GetResidentBytes() > B.Total.GetResidentBytes();
		}
		return Report.Nodes[A.NodeIndex].PackageName.ToString().Compare(
			Report.Nodes[B.NodeIndex].PackageName.ToString(), ESearchCase::CaseSensitive) < 0;
	});
	return Order;
}

FPulseMemoryReportWriter::FPulseMemoryReportWriter(const FString& InReportDir, int32 InMaxDependenciesPerRoot, int32 InMaxHistoricalRuns)
	: ReportDir(InReportDir)
	, MaxDependenciesPerRoot(FMath::Max(InMaxDependenciesPerRoot, 1))
	, MaxHistoricalRuns(InMaxHistoricalRuns)
{
}

bool FPulseMemoryReportWriter::Write(const FPulseMemoryReport& Report, FString& OutError)
{
	RunDir = ReportDir / (GPulseMemoryRunPrefix + Report.GeneratedAtUtc);

	// MakeDirectory can report failure for a directory that already exists, so existence is the
	// condition that actually matters here, not the return value.
	IFileManager::Get().MakeDirectory(*RunDir, /*Tree*/ true);
	if (!IFileManager::Get().DirectoryExists(*RunDir))
	{
		OutError = FString::Printf(TEXT("Failed to create report directory %s"), *RunDir);
		return false;
	}

	if (!WriteRootsCsv(Report, OutError)
		|| !WriteDependenciesCsv(Report, OutError)
		|| !WriteAssetsCsv(Report, OutError)
		|| !WriteRunJson(Report, OutError))
	{
		return false;
	}

	PruneHistoricalRuns();
	return true;
}

bool FPulseMemoryReportWriter::WriteRootsCsv(const FPulseMemoryReport& Report, FString& OutError) const
{
	FPulseCsvWriter Csv;
	Csv.WriteRow({
		TEXT("Asset"), TEXT("AssetPath"), TEXT("Class"), TEXT("Kind"), TEXT("KindDetail"), TEXT("WorldPartition"),
		TEXT("TotalRAMBytes"), TEXT("TotalVRAMBytes"), TEXT("TotalStreamingBytes"),
		TEXT("UniqueRAMBytes"), TEXT("UniqueVRAMBytes"), TEXT("UniqueStreamingBytes"),
		TEXT("MayLoadRAMBytes"), TEXT("MayLoadVRAMBytes"), TEXT("MayLoadStreamingBytes"),
		TEXT("HardPackages"), TEXT("SoftOnlyPackages"), TEXT("ResidentBytesFromDiskSize"), TEXT("UnknownSizePackages"),
		TEXT("OverBudget")
	});

	for (const int32 RootIndex : PulseSortRootsByResidentCost(Report))
	{
		const FPulseMemoryRoot& Root = Report.Roots[RootIndex];
		const FPulseMemoryNode& Node = Report.Nodes[Root.NodeIndex];
		Csv.WriteRow({
			Node.AssetName,
			Node.ObjectPath,
			Node.ClassName,
			PulseMemoryRootKindToString(Root.Kind),
			Root.KindDetail,
			PulseFormatBool(Root.bWorldPartition),
			PulseFormatInt(Root.Total.RAMBytes),
			PulseFormatInt(Root.Total.VRAMBytes),
			PulseFormatInt(Root.Total.StreamingBytes),
			PulseFormatInt(Root.Unique.RAMBytes),
			PulseFormatInt(Root.Unique.VRAMBytes),
			PulseFormatInt(Root.Unique.StreamingBytes),
			PulseFormatInt(Root.MayLoad.RAMBytes),
			PulseFormatInt(Root.MayLoad.VRAMBytes),
			PulseFormatInt(Root.MayLoad.StreamingBytes),
			FString::FromInt(Root.NumHardPackages),
			FString::FromInt(Root.NumSoftOnlyPackages),
			PulseFormatInt(Root.ResidentBytesFromDiskSize),
			FString::FromInt(Root.NumUnknownSize),
			PulseFormatBool(Report.IsRootOverBudget(Root))
		});
	}

	return Csv.SaveToFile(RunDir / TEXT("Roots.csv"), OutError);
}

bool FPulseMemoryReportWriter::WriteDependenciesCsv(const FPulseMemoryReport& Report, FString& OutError) const
{
	FPulseCsvWriter Csv;
	Csv.WriteRow({
		TEXT("Root"), TEXT("Dependency"), TEXT("AssetPath"), TEXT("Class"), TEXT("Hard"), TEXT("Depth"),
		TEXT("RAMBytes"), TEXT("VRAMBytes"), TEXT("StreamingBytes"),
		TEXT("RetainedRAMBytes"), TEXT("RetainedVRAMBytes"), TEXT("RootsReaching"),
		TEXT("SizeSource"), TEXT("SizeNote"), TEXT("Via"), TEXT("Referencers"), TEXT("ReferencedBy")
	});

	FPulseMemoryReach Reach;
	TArray<TArray<int32>> ReferencerSlots;
	for (const int32 RootIndex : PulseSortRootsByResidentCost(Report))
	{
		const FPulseMemoryRoot& Root = Report.Roots[RootIndex];
		Report.ComputeReach(Root.NodeIndex, Report.Estimation.bIncludeSoftReferences, Reach);
		Report.CollectReferencers(Reach, ReferencerSlots);

		// Hard and soft are capped separately: a root with a large soft fan-out must not push its
		// own resident dependencies out of the file.
		auto WriteTop = [&](int32 FirstSlot, int32 EndSlot, bool bHard)
		{
			TArray<int32> Slots;
			for (int32 Slot = FMath::Max(FirstSlot, 1); Slot < EndSlot; ++Slot)
			{
				Slots.Add(Slot);
			}
			Slots.Sort([&Report, &Reach](int32 Left, int32 Right)
			{
				return PulseNodeLess(Report, Reach.Nodes[Left], Reach.Nodes[Right]);
			});
			if (Slots.Num() > MaxDependenciesPerRoot)
			{
				Slots.SetNum(MaxDependenciesPerRoot);
			}

			for (const int32 Slot : Slots)
			{
				const FPulseMemoryNode& Node = Report.Nodes[Reach.Nodes[Slot]];
				Csv.WriteRow({
					Report.Nodes[Root.NodeIndex].AssetName,
					Node.AssetName,
					Node.ObjectPath.IsEmpty() ? Node.PackageName.ToString() : Node.ObjectPath,
					Node.ClassName,
					PulseFormatBool(bHard),
					FString::FromInt(Reach.Depths[Slot]),
					PulseFormatInt(Node.Self.RAMBytes),
					PulseFormatInt(Node.Self.VRAMBytes),
					PulseFormatInt(Node.Self.StreamingBytes),
					PulseFormatInt(Node.Retained.RAMBytes),
					PulseFormatInt(Node.Retained.VRAMBytes),
					FString::FromInt(Node.NumRootsReaching),
					PulseMemorySizeSourceToString(Node.SizeSource),
					Node.SizeNote,
					Report.BuildViaPath(Reach, Slot),
					// Every direct referencer within this root, not just the one on the via chain:
					// a dependency only unloads once all of them stop referencing it.
					FString::FromInt(ReferencerSlots[Slot].Num()),
					Report.FormatReferencers(Reach, ReferencerSlots[Slot], /*MaxNames*/ 25)
				});
			}
		};

		WriteTop(0, Reach.NumHard, /*bHard*/ true);
		WriteTop(Reach.NumHard, Reach.Nodes.Num(), /*bHard*/ false);
	}

	return Csv.SaveToFile(RunDir / TEXT("Dependencies.csv"), OutError);
}

bool FPulseMemoryReportWriter::WriteAssetsCsv(const FPulseMemoryReport& Report, FString& OutError) const
{
	FPulseCsvWriter Csv;
	Csv.WriteRow({
		TEXT("Asset"), TEXT("AssetPath"), TEXT("Class"),
		TEXT("RAMBytes"), TEXT("VRAMBytes"), TEXT("StreamingBytes"),
		TEXT("RetainedRAMBytes"), TEXT("RetainedVRAMBytes"), TEXT("RetainedStreamingBytes"),
		TEXT("RootsReaching"), TEXT("InBaseline"), TEXT("SizeSource"), TEXT("SizeNote")
	});

	// Only what something actually loads. Packages reached purely through soft references are in
	// Dependencies.csv as may-load rows; listing them here would blur "is resident" with "could be".
	TArray<int32> Order;
	for (int32 NodeIndex = 0; NodeIndex < Report.Nodes.Num(); ++NodeIndex)
	{
		if (Report.Nodes[NodeIndex].NumRootsReaching > 0)
		{
			Order.Add(NodeIndex);
		}
	}
	Order.Sort([&Report](int32 Left, int32 Right) { return PulseNodeLess(Report, Left, Right); });

	for (const int32 NodeIndex : Order)
	{
		const FPulseMemoryNode& Node = Report.Nodes[NodeIndex];
		Csv.WriteRow({
			Node.AssetName,
			Node.ObjectPath.IsEmpty() ? Node.PackageName.ToString() : Node.ObjectPath,
			Node.ClassName,
			PulseFormatInt(Node.Self.RAMBytes),
			PulseFormatInt(Node.Self.VRAMBytes),
			PulseFormatInt(Node.Self.StreamingBytes),
			PulseFormatInt(Node.Retained.RAMBytes),
			PulseFormatInt(Node.Retained.VRAMBytes),
			PulseFormatInt(Node.Retained.StreamingBytes),
			FString::FromInt(Node.NumRootsReaching),
			PulseFormatBool(Node.bInBaseline),
			PulseMemorySizeSourceToString(Node.SizeSource),
			Node.SizeNote
		});
	}

	return Csv.SaveToFile(RunDir / TEXT("Assets.csv"), OutError);
}

bool FPulseMemoryReportWriter::WriteRunJson(const FPulseMemoryReport& Report, FString& OutError) const
{
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);

	auto WriteCost = [&Writer](const TCHAR* Name, const FPulseMemoryCost& Cost)
	{
		Writer->WriteObjectStart(Name);
		Writer->WriteValue(TEXT("ramBytes"), Cost.RAMBytes);
		Writer->WriteValue(TEXT("vramBytes"), Cost.VRAMBytes);
		Writer->WriteValue(TEXT("streamingBytes"), Cost.StreamingBytes);
		Writer->WriteObjectEnd();
	};

	int32 NumRootsOverBudget = 0;
	int32 NumByKind[4] = { 0, 0, 0, 0 };
	for (const FPulseMemoryRoot& Root : Report.Roots)
	{
		NumRootsOverBudget += Report.IsRootOverBudget(Root) ? 1 : 0;
		NumByKind[static_cast<uint8>(Root.Kind)]++;
	}

	int32 NumBySource[3] = { 0, 0, 0 };
	for (const FPulseMemoryNode& Node : Report.Nodes)
	{
		if (Node.NumRootsReaching > 0)
		{
			NumBySource[static_cast<uint8>(Node.SizeSource)]++;
		}
	}

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("engineVersion"), Report.EngineVersion);

	Writer->WriteArrayStart(TEXT("scanPaths"));
	for (const FString& Path : Report.ScanPaths)
	{
		Writer->WriteValue(Path);
	}
	Writer->WriteArrayEnd();

	Writer->WriteObjectStart(TEXT("baseline"));
	Writer->WriteValue(TEXT("packages"), Report.NumBaselinePackages);
	Writer->WriteValue(TEXT("ramBytes"), Report.Baseline.RAMBytes);
	Writer->WriteValue(TEXT("vramBytes"), Report.Baseline.VRAMBytes);
	Writer->WriteValue(TEXT("streamingBytes"), Report.Baseline.StreamingBytes);
	Writer->WriteValue(TEXT("overBudget"), Report.IsBaselineOverBudget());
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("totals"));
	Writer->WriteValue(TEXT("roots"), Report.Roots.Num());
	Writer->WriteValue(TEXT("alwaysLoadedRoots"), NumByKind[static_cast<uint8>(EPulseMemoryRootKind::AlwaysLoaded)]);
	Writer->WriteValue(TEXT("mapRoots"), NumByKind[static_cast<uint8>(EPulseMemoryRootKind::Map)]);
	Writer->WriteValue(TEXT("configuredRoots"), NumByKind[static_cast<uint8>(EPulseMemoryRootKind::Configured)]);
	Writer->WriteValue(TEXT("primaryAssetRoots"), NumByKind[static_cast<uint8>(EPulseMemoryRootKind::PrimaryAsset)]);
	Writer->WriteValue(TEXT("rootsOverBudget"), NumRootsOverBudget);
	Writer->WriteValue(TEXT("packagesInGraph"), Report.Nodes.Num());
	Writer->WriteValue(TEXT("packagesReachedByHardReference"), Report.NumReachablePackages);
	Writer->WriteValue(TEXT("packagesSizedFromTags"), NumBySource[static_cast<uint8>(EPulseMemorySizeSource::Tags)]);
	Writer->WriteValue(TEXT("packagesSizedFromDiskSize"), NumBySource[static_cast<uint8>(EPulseMemorySizeSource::DiskSize)]);
	Writer->WriteValue(TEXT("packagesWithUnknownSize"), NumBySource[static_cast<uint8>(EPulseMemorySizeSource::Unknown)]);
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("roots"));
	Writer->WriteValue(TEXT("includeMaps"), Report.RootSettings.bIncludeMaps);
	Writer->WriteValue(TEXT("includePrimaryAssets"), Report.RootSettings.bIncludePrimaryAssets);
	Writer->WriteValue(TEXT("includeGameMapsSettings"), Report.RootSettings.bIncludeGameMapsSettings);
	Writer->WriteArrayStart(TEXT("alwaysLoadedAssets"));
	for (const FSoftObjectPath& Path : Report.RootSettings.AlwaysLoadedAssets)
	{
		Writer->WriteValue(Path.ToString());
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("additionalRootClasses"));
	for (const FSoftClassPath& Path : Report.RootSettings.AdditionalRootClasses)
	{
		Writer->WriteValue(Path.ToString());
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("estimation"));
	Writer->WriteValue(TEXT("includeSoftReferences"), Report.Estimation.bIncludeSoftReferences);
	Writer->WriteValue(TEXT("audioCompressionRatio"), Report.Estimation.AudioCompressionRatio);
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("budgetMB"));
	Writer->WriteValue(TEXT("baselineRAM"), Report.Budget.BaselineRAMBudgetMB);
	Writer->WriteValue(TEXT("baselineVRAM"), Report.Budget.BaselineVRAMBudgetMB);
	Writer->WriteValue(TEXT("rootRAM"), Report.Budget.RootRAMBudgetMB);
	Writer->WriteValue(TEXT("rootVRAM"), Report.Budget.RootVRAMBudgetMB);
	Writer->WriteObjectEnd();

	// Recorded as data rather than left to the README: a CSV read on its own must still say what its
	// numbers are, or a ranking estimate gets quoted as a measured budget.
	Writer->WriteValue(TEXT("estimatesOnly"), true);
	Writer->WriteValue(TEXT("estimationNote"),
		TEXT("Sizes are estimated from Asset Registry tags without loading anything. Textures, meshes, animations and sounds are computed from their tags; ")
		TEXT("other packages use their editor size on disk, which includes editor-only data and usually overstates. Device-profile texture LOD settings, ")
		TEXT("platform-specific formats and shader maps are not included. Use these numbers to rank, not to budget to the byte."));
	Writer->WriteValue(TEXT("streamingNote"),
		TEXT("Streaming bytes (texture mips above the resident floor, Nanite pages, compressed audio) load on demand and are never counted as resident."));
	Writer->WriteValue(TEXT("worldPartitionNote"),
		TEXT("World Partition actors stream per cell, so they are counted under may-load; a partitioned map's total is its persistent level only."));

	Writer->WriteObjectEnd();
	Writer->Close();

	const FString Path = RunDir / TEXT("Run.json");
	if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write %s"), *Path);
		return false;
	}
	return true;
}

void FPulseMemoryReportWriter::PruneHistoricalRuns() const
{
	if (MaxHistoricalRuns <= 0)
	{
		return;
	}

	TArray<FString> RunFolders;
	IFileManager::Get().FindFiles(RunFolders, *(ReportDir / (FString(GPulseMemoryRunPrefix) + TEXT("*"))),
		/*Files*/ false, /*Directories*/ true);

	if (RunFolders.Num() <= MaxHistoricalRuns)
	{
		return;
	}

	// Folder names embed a sortable UTC timestamp, so lexical order is chronological order.
	RunFolders.Sort();

	const int32 NumToDelete = RunFolders.Num() - MaxHistoricalRuns;
	for (int32 Index = 0; Index < NumToDelete; ++Index)
	{
		IFileManager::Get().DeleteDirectory(*(ReportDir / RunFolders[Index]), /*RequireExists*/ false, /*Tree*/ true);
	}
}
