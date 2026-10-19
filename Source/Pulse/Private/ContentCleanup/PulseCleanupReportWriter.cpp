// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseCleanupReportWriter.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "PulseCleanupResult.h"
#include "PulseCsvWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

static const TCHAR* GPulseCleanupRunPrefix = TEXT("ContentCleanup_");

static FString PulseCleanupBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

static FString PulseCleanupInt(int64 Value)
{
	return FString::Printf(TEXT("%lld"), Value);
}

FPulseCleanupReportWriter::FPulseCleanupReportWriter(const FString& InReportDir, int32 InMaxHistoricalRuns)
	: ReportDir(InReportDir)
	, MaxHistoricalRuns(InMaxHistoricalRuns)
{
}

bool FPulseCleanupReportWriter::Write(const FPulseCleanupReport& Report, FString& OutError)
{
	RunDir = ReportDir / (GPulseCleanupRunPrefix + Report.GeneratedAtUtc);
	IFileManager::Get().MakeDirectory(*RunDir, /*Tree*/ true);
	if (!IFileManager::Get().DirectoryExists(*RunDir))
	{
		OutError = FString::Printf(TEXT("Failed to create report directory %s"), *RunDir);
		return false;
	}

	if (!WriteUnusedCsv(Report, OutError) || !WriteDuplicatesCsv(Report, OutError) || !WriteRunJson(Report, OutError))
	{
		return false;
	}
	PruneHistoricalRuns();
	return true;
}

bool FPulseCleanupReportWriter::WriteUnusedCsv(const FPulseCleanupReport& Report, FString& OutError) const
{
	FPulseCsvWriter Csv;
	Csv.WriteRow({
		TEXT("Asset"), TEXT("AssetPath"), TEXT("Class"), TEXT("Status"), TEXT("DiskBytes"),
		TEXT("RAMBytes"), TEXT("VRAMBytes"), TEXT("StreamingBytes"),
		TEXT("Referencers"), TEXT("ReferencedBy"), TEXT("MentionedIn")
	});

	for (const FPulseCleanupAsset& Asset : Report.Unused)
	{
		Csv.WriteRow({
			Asset.AssetName,
			Asset.ObjectPath,
			Asset.ClassName,
			PulseCleanupStatusToString(Asset.Status),
			PulseCleanupInt(Asset.DiskBytes),
			PulseCleanupInt(Asset.Memory.RAMBytes),
			PulseCleanupInt(Asset.Memory.VRAMBytes),
			PulseCleanupInt(Asset.Memory.StreamingBytes),
			FString::FromInt(Asset.NumReferencers),
			Asset.ReferencedBy,
			Asset.MentionedIn
		});
	}
	return Csv.SaveToFile(RunDir / TEXT("Unused.csv"), OutError);
}

bool FPulseCleanupReportWriter::WriteDuplicatesCsv(const FPulseCleanupReport& Report, FString& OutError) const
{
	FPulseCsvWriter Csv;
	Csv.WriteRow({
		TEXT("Group"), TEXT("SourceFile"), TEXT("SourceMD5"), TEXT("Class"),
		TEXT("Asset"), TEXT("AssetPath"), TEXT("DiskBytes"), TEXT("RAMBytes"), TEXT("VRAMBytes"),
		TEXT("Used"), TEXT("Referencers"), TEXT("GroupWastedDiskBytes"), TEXT("GroupWastedResidentBytes")
	});

	for (int32 GroupIndex = 0; GroupIndex < Report.Duplicates.Num(); ++GroupIndex)
	{
		const FPulseDuplicateGroup& Group = Report.Duplicates[GroupIndex];
		for (const FPulseDuplicateMember& Member : Group.Members)
		{
			Csv.WriteRow({
				FString::FromInt(GroupIndex + 1),
				Group.SourceFile,
				Group.SourceHash,
				Group.ClassName,
				Member.AssetName,
				Member.ObjectPath,
				PulseCleanupInt(Member.DiskBytes),
				PulseCleanupInt(Member.Memory.RAMBytes),
				PulseCleanupInt(Member.Memory.VRAMBytes),
				PulseCleanupBool(Member.bUsed),
				FString::FromInt(Member.NumReferencers),
				PulseCleanupInt(Group.WastedDiskBytes),
				PulseCleanupInt(Group.WastedMemory.GetResidentBytes())
			});
		}
	}
	return Csv.SaveToFile(RunDir / TEXT("Duplicates.csv"), OutError);
}

bool FPulseCleanupReportWriter::WriteRunJson(const FPulseCleanupReport& Report, FString& OutError) const
{
	int32 NumByStatus[3] = {};
	int32 NumMentioned = 0;
	for (const FPulseCleanupAsset& Asset : Report.Unused)
	{
		NumByStatus[static_cast<uint8>(Asset.Status)]++;
		NumMentioned += Asset.IsMentioned() ? 1 : 0;
	}

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("engineVersion"), Report.EngineVersion);
	Writer->WriteArrayStart(TEXT("scanPaths"));
	for (const FString& Path : Report.ScanPaths)
	{
		Writer->WriteValue(Path);
	}
	Writer->WriteArrayEnd();

	Writer->WriteObjectStart(TEXT("roots"));
	Writer->WriteValue(TEXT("usedMapsToCook"), Report.bUsedMapsToCook);
	Writer->WriteValue(TEXT("mapsToCook"), Report.NumMapsToCook);
	Writer->WriteValue(TEXT("alwaysCookDirectories"), Report.NumAlwaysCookDirectories);
	Writer->WriteValue(TEXT("packagesUsed"), Report.NumUsedPackages);
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("search"));
	Writer->WriteValue(TEXT("configAndSourceSearched"), Report.bSearchedConfigAndSource);
	Writer->WriteValue(TEXT("filesSearched"), Report.NumFilesSearched);
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("totals"));
	Writer->WriteValue(TEXT("assetsScanned"), Report.NumAssetsScanned);
	Writer->WriteValue(TEXT("orphans"), NumByStatus[static_cast<uint8>(EPulseCleanupStatus::Orphan)]);
	Writer->WriteValue(TEXT("unreachable"), NumByStatus[static_cast<uint8>(EPulseCleanupStatus::Unreachable)]);
	Writer->WriteValue(TEXT("redirectors"), NumByStatus[static_cast<uint8>(EPulseCleanupStatus::Redirector)]);
	Writer->WriteValue(TEXT("mentionedInConfigOrSource"), NumMentioned);
	Writer->WriteValue(TEXT("unusedDiskBytes"), Report.GetUnusedDiskBytes(/*bIncludeMentioned*/ true));
	Writer->WriteValue(TEXT("unusedDiskBytesNotMentioned"), Report.GetUnusedDiskBytes(/*bIncludeMentioned*/ false));
	Writer->WriteValue(TEXT("duplicateGroups"), Report.Duplicates.Num());
	Writer->WriteValue(TEXT("duplicateWastedDiskBytes"), Report.GetDuplicateWastedDiskBytes());
	Writer->WriteObjectEnd();

	Writer->WriteValue(TEXT("note"),
		TEXT("Nothing here is safe to delete on this report alone. Unused means no map, primary asset, always-loaded asset or always-cook directory reaches the asset through any hard or soft reference the Asset Registry records. ")
		TEXT("Paths built at runtime from strings, and references from content outside the project, are invisible to it; config and C++ mentions are flagged in MentionedIn. Fix up redirectors before deleting anything."));

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

void FPulseCleanupReportWriter::PruneHistoricalRuns() const
{
	if (MaxHistoricalRuns <= 0)
	{
		return;
	}

	TArray<FString> RunFolders;
	IFileManager::Get().FindFiles(RunFolders, *(ReportDir / (FString(GPulseCleanupRunPrefix) + TEXT("*"))), /*Files*/ false, /*Directories*/ true);
	if (RunFolders.Num() <= MaxHistoricalRuns)
	{
		return;
	}

	// Sortable UTC timestamps in the names: lexical order is chronological order.
	RunFolders.Sort();
	for (int32 Index = 0; Index < RunFolders.Num() - MaxHistoricalRuns; ++Index)
	{
		IFileManager::Get().DeleteDirectory(*(ReportDir / RunFolders[Index]), /*RequireExists*/ false, /*Tree*/ true);
	}
}
