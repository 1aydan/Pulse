// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseReportWriter.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PulseCsvWriter.h"
#include "PulseJsonWriter.h"
#include "PulseResult.h"

// Scores always print %.2f — never %g, whose exponent switching renders an unchanged value
// differently depending on magnitude and wrecks report diffs.
static FString PulseScoreField(float Score)
{
	return FString::Printf(TEXT("%.2f"), Score);
}

static bool PulseWriteSummaryCsv(const FPulseReport& Report, const FString& Path, FString& OutError)
{
	const FPulseRunHeader& Header = Report.Header;

	FPulseCsvWriter Csv;
	Csv.WriteRow({ TEXT("PulseVersion"), Header.PulseVersion });
	Csv.WriteRow({ TEXT("EngineVersion"), Header.EngineVersion });
	Csv.WriteRow({ TEXT("Project"), Header.ProjectName });
	Csv.WriteRow({ TEXT("Tier"), PulseTierToString(Header.RunTier) });
	Csv.WriteRow({ TEXT("SettingsHash"), Header.SettingsHash });
	Csv.WriteRow({ TEXT("Truncated"), Header.bTruncated ? TEXT("true") : TEXT("false") });
	Csv.WriteRow({ TEXT("OverallScore"), PulseScoreField(Report.OverallScore) });
	// No timestamp anywhere in this block: GeneratedAtUtc lives only in the snapshot filename.

	Csv.WriteRow({});

	Csv.WriteRow({ TEXT("Category"), TEXT("Score"), TEXT("MeanAssetScore"), TEXT("Assets"), TEXT("AssetsWithIssues"),
		TEXT("Info"), TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Critical"), TEXT("MissingTags") });

	for (const FPulseCategoryResult& Category : Report.Categories)
	{
		Csv.WriteRow({
			Category.Category.ToString(),
			PulseScoreField(Category.Score),
			PulseScoreField(Category.MeanAssetScore),
			FString::Printf(TEXT("%d"), Category.NumAssets),
			FString::Printf(TEXT("%d"), Category.NumAssetsWithIssues),
			FString::Printf(TEXT("%d"), Category.IssueCountBySeverity[static_cast<int32>(EPulseSeverity::Info)]),
			FString::Printf(TEXT("%d"), Category.IssueCountBySeverity[static_cast<int32>(EPulseSeverity::Low)]),
			FString::Printf(TEXT("%d"), Category.IssueCountBySeverity[static_cast<int32>(EPulseSeverity::Medium)]),
			FString::Printf(TEXT("%d"), Category.IssueCountBySeverity[static_cast<int32>(EPulseSeverity::High)]),
			FString::Printf(TEXT("%d"), Category.IssueCountBySeverity[static_cast<int32>(EPulseSeverity::Critical)]),
			FString::Printf(TEXT("%d"), Category.NumMissingExpectedTags)
		});
	}

	return Csv.SaveToFile(Path, OutError);
}

static bool PulseWriteAssetsCsv(const FPulseCategoryResult& Category, const FString& Path, FString& OutError)
{
	FPulseCsvWriter Csv;

	// Columns come from the schema, never from observed data, so the header is identical run to
	// run: Deep-tier columns are present-but-empty in a Fast run and diffs show only real change.
	TArray<FString> HeaderRow;
	HeaderRow.Add(TEXT("Asset"));
	HeaderRow.Add(TEXT("Class"));
	HeaderRow.Add(TEXT("AchievedTier"));
	HeaderRow.Add(TEXT("Score"));
	for (const FPulseMetricDesc& Desc : Category.MetricSchema)
	{
		HeaderRow.Add(Desc.Name.ToString());
	}
	Csv.WriteRow(HeaderRow);

	for (const FPulseAssetResult& Asset : Category.Assets)
	{
		TArray<FString> Row;
		Row.Reserve(HeaderRow.Num());
		Row.Add(Asset.AssetPath.ToString());
		Row.Add(Asset.ClassPath.ToString());
		Row.Add(PulseTierToString(Asset.AchievedTier));
		Row.Add(PulseScoreField(Asset.Score));
		for (const FPulseMetricDesc& Desc : Category.MetricSchema)
		{
			const FPulseMetric* Metric = Asset.FindMetric(Desc.Name);
			Row.Add(Metric ? Metric->ToCsvField() : FString());
		}
		Csv.WriteRow(Row);
	}

	return Csv.SaveToFile(Path, OutError);
}

static bool PulseWriteIssuesCsv(const FPulseReport& Report, EPulseSeverity MinSeverity, const FString& Path, FString& OutError)
{
	FPulseCsvWriter Csv;
	Csv.WriteRow({ TEXT("Category"), TEXT("Asset"), TEXT("RuleId"), TEXT("Severity"), TEXT("Tier"),
		TEXT("Message"), TEXT("Recommendation") });

	// Row order is category > asset path > (severity desc, rule id) — free, because FPulseScoring
	// already sorted the whole report before summation.
	for (const FPulseCategoryResult& Category : Report.Categories)
	{
		for (const FPulseAssetResult& Asset : Category.Assets)
		{
			for (const FPulseIssue& Issue : Asset.Issues)
			{
				if (Issue.Severity < MinSeverity)
				{
					continue;
				}
				Csv.WriteRow({
					Category.Category.ToString(),
					Asset.AssetPath.ToString(),
					Issue.RuleId.ToString(),
					PulseSeverityToString(Issue.Severity),
					PulseTierToString(Issue.DetectedAtTier),
					Issue.Message,
					Issue.Recommendation
				});
			}
		}
	}

	return Csv.SaveToFile(Path, OutError);
}

/**
 * Copies the exact Statistics.json bytes to PulseAudit_<stamp>.json, then prunes the oldest
 * snapshots. The stamp is GeneratedAtUtc with ':' '.' '-' stripped — filesystem-safe on every
 * platform and still lexically sortable, which is what pruning relies on.
 */
static bool PulseWriteSnapshot(const FString& ReportDir, const FString& JsonContent, const FString& GeneratedAtUtc, int32 MaxHistoricalSnapshots, FString& OutError)
{
	FString Stamp = GeneratedAtUtc;
	Stamp.ReplaceInline(TEXT(":"), TEXT(""), ESearchCase::CaseSensitive);
	Stamp.ReplaceInline(TEXT("."), TEXT(""), ESearchCase::CaseSensitive);
	Stamp.ReplaceInline(TEXT("-"), TEXT(""), ESearchCase::CaseSensitive);

	const FString SnapshotPath = FPaths::Combine(ReportDir, FString::Printf(TEXT("PulseAudit_%s.json"), *Stamp));
	if (!FFileHelper::SaveStringToFile(JsonContent, *SnapshotPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write %s"), *SnapshotPath);
		return false;
	}

	// 0 means keep everything.
	if (MaxHistoricalSnapshots <= 0)
	{
		return true;
	}

	IFileManager& FileManager = IFileManager::Get();

	// FindFiles with a wildcard returns bare filenames; the stripped-stamp naming makes a lexical
	// sort a chronological sort, so the oldest snapshots are simply the front of the array.
	TArray<FString> SnapshotNames;
	FileManager.FindFiles(SnapshotNames, *FPaths::Combine(ReportDir, TEXT("PulseAudit_*.json")), /*Files*/ true, /*Directories*/ false);
	SnapshotNames.Sort([](const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive) < 0;
	});

	const int32 NumToDelete = SnapshotNames.Num() - MaxHistoricalSnapshots;
	for (int32 Index = 0; Index < NumToDelete; ++Index)
	{
		const FString DeletePath = FPaths::Combine(ReportDir, SnapshotNames[Index]);
		if (!FileManager.Delete(*DeletePath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true))
		{
			OutError = FString::Printf(TEXT("Failed to prune snapshot %s"), *DeletePath);
			return false;
		}
	}

	return true;
}

FPulseReportWriter::FPulseReportWriter(const FString& InReportDir, bool bInWriteCsv, bool bInWriteJson, int32 InMaxHistoricalSnapshots)
	: ReportDir(InReportDir)
	, bWriteCsv(bInWriteCsv)
	, bWriteJson(bInWriteJson)
	, MaxHistoricalSnapshots(InMaxHistoricalSnapshots)
{
}

bool FPulseReportWriter::Write(const FPulseReport& Report, EPulseSeverity MinSeverity, FString& OutError) const
{
	IFileManager& FileManager = IFileManager::Get();

	// MakeDirectory can report false on a directory that already exists, so double-check before
	// declaring failure.
	if (!FileManager.MakeDirectory(*ReportDir, /*Tree*/ true) && !FileManager.DirectoryExists(*ReportDir))
	{
		OutError = FString::Printf(TEXT("Failed to create report directory %s"), *ReportDir);
		return false;
	}

	if (bWriteCsv)
	{
		if (!PulseWriteSummaryCsv(Report, FPaths::Combine(ReportDir, TEXT("Summary.csv")), OutError))
		{
			return false;
		}

		for (const FPulseCategoryResult& Category : Report.Categories)
		{
			const FString AssetsPath = FPaths::Combine(ReportDir, FString::Printf(TEXT("Assets_%s.csv"), *Category.Category.ToString()));
			if (!PulseWriteAssetsCsv(Category, AssetsPath, OutError))
			{
				return false;
			}
		}

		if (!PulseWriteIssuesCsv(Report, MinSeverity, FPaths::Combine(ReportDir, TEXT("Issues.csv")), OutError))
		{
			return false;
		}
	}

	if (bWriteJson)
	{
		// The snapshot is written from the same string as Statistics.json so the two are
		// byte-identical by construction, not by hoping two serializer passes agree.
		const FString Json = PulseWriteReportJson(Report, MinSeverity);

		const FString StatisticsPath = FPaths::Combine(ReportDir, TEXT("Statistics.json"));
		if (!FFileHelper::SaveStringToFile(Json, *StatisticsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write %s"), *StatisticsPath);
			return false;
		}

		if (!PulseWriteSnapshot(ReportDir, Json, Report.Header.GeneratedAtUtc, MaxHistoricalSnapshots, OutError))
		{
			return false;
		}
	}

	return true;
}
