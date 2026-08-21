// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseNiagaraCostReportWriter.h"

#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PulseCsvWriter.h"
#include "PulseNiagaraAuditorSettings.h"
#include "PulseNiagaraCostResult.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/** Prefix for run folders. Also the prune filter, so nothing else in ReportDir is ever touched. */
static const TCHAR* GPulseNiagaraRunPrefix = TEXT("NiagaraAudit_");

/**
 * Fixed precision so an unchanged run diffs clean rather than churning in the last digits. Four
 * decimals because a per-instance cost is routinely a few thousandths of a millisecond, and three
 * would round the cheap systems to a flat 0.000.
 */
static FString PulseFormatMilliseconds(double Milliseconds)
{
	return FString::Printf(TEXT("%.4f"), Milliseconds);
}

static FString PulseFormatPercent(double Percent)
{
	return FString::Printf(TEXT("%.2f"), Percent);
}

static FString PulseFormatBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

/** Share of one frame, as a percentage. Zero budget yields an empty cell rather than a divide. */
static FString PulseFormatFrameShare(double Milliseconds, float FrameBudgetMs)
{
	if (FrameBudgetMs <= 0.0f)
	{
		return FString();
	}
	return PulseFormatPercent(Milliseconds / FrameBudgetMs * 100.0);
}

/** Turns a quality level display name into something safe to use as a filename or column prefix. */
static FString PulseSanitizeName(const FString& Name)
{
	FString Sanitized;
	Sanitized.Reserve(Name.Len());
	for (const TCHAR Char : Name)
	{
		Sanitized.AppendChar(FChar::IsAlnum(Char) ? Char : TEXT('_'));
	}
	return Sanitized.IsEmpty() ? TEXT("Unnamed") : Sanitized;
}

/** Empty for a distance-less run, so those columns and cells do not carry a meaningless number. */
static FString PulseFormatDistance(float Distance)
{
	return Distance <= GPulseNiagaraNoDistance ? FString() : FString::Printf(TEXT("%.0f"), Distance);
}

/**
 * Percentage drop from Reference to Candidate. Both must be measured and the reference non-zero,
 * otherwise the caller gets nothing rather than a saving that was never tested for.
 */
static bool PulseComputeSaving(const FPulseNiagaraCostSample* Reference, const FPulseNiagaraCostSample* Candidate, double& OutSavingPercent)
{
	if (Reference == nullptr || Candidate == nullptr || !Reference->bMeasured || !Candidate->bMeasured
		|| Reference->TotalAvgMs <= 0.0)
	{
		return false;
	}
	OutSavingPercent = (Reference->TotalAvgMs - Candidate->TotalAvgMs)
		/ Reference->TotalAvgMs * 100.0;
	return true;
}

FPulseNiagaraCostReportWriter::FPulseNiagaraCostReportWriter(const FString& InReportDir, const FPulseNiagaraCostBudget& InBudget, int32 InMaxHistoricalRuns)
	: ReportDir(InReportDir)
	, Budget(InBudget)
	, MaxHistoricalRuns(InMaxHistoricalRuns)
{
}

FString FPulseNiagaraCostReportWriter::QualityName(const FPulseNiagaraCostReport& Report, int32 QualityIndex)
{
	return Report.QualityLevelNames.IsValidIndex(QualityIndex)
		? Report.QualityLevelNames[QualityIndex]
		: FString::FromInt(Report.QualityLevels[QualityIndex]);
}

bool FPulseNiagaraCostReportWriter::Write(const FPulseNiagaraCostReport& Report, FString& OutError)
{
	RunDir = ReportDir / (GPulseNiagaraRunPrefix + Report.GeneratedAtUtc);

	// MakeDirectory can report failure for a directory that already exists, so existence is the
	// condition that actually matters here, not the return value.
	IFileManager::Get().MakeDirectory(*RunDir, /*Tree*/ true);
	if (!IFileManager::Get().DirectoryExists(*RunDir))
	{
		OutError = FString::Printf(TEXT("Failed to create report directory %s"), *RunDir);
		return false;
	}

	for (int32 QualityIndex = 0; QualityIndex < Report.QualityLevels.Num(); ++QualityIndex)
	{
		if (!WriteQualityCsv(Report, QualityIndex, OutError))
		{
			return false;
		}
	}

	if (!WriteComparisonCsv(Report, OutError))
	{
		return false;
	}
	if (!WriteRunJson(Report, OutError))
	{
		return false;
	}

	PruneHistoricalRuns();
	return true;
}

bool FPulseNiagaraCostReportWriter::WriteQualityCsv(const FPulseNiagaraCostReport& Report, int32 QualityIndex, FString& OutError) const
{
	const int32 QualityLevel = Report.QualityLevels[QualityIndex];

	FPulseCsvWriter Csv;
	Csv.WriteRow({
		TEXT("System"), TEXT("AssetPath"), TEXT("EffectType"), TEXT("NumEmitters"), TEXT("HasGPUEmitter"),
		TEXT("DistanceCm"), TEXT("Measured"), TEXT("SkipReason"),
		TEXT("InstancesSpawned"), TEXT("InstancesActiveAtEnd"), TEXT("FramesRecorded"), TEXT("IdleFrames"),
		TEXT("TotalAvgMs"), TEXT("TotalMinMs"), TEXT("TotalMaxMs"),
		TEXT("PerInstanceAvgMs"), TEXT("PerInstanceMinMs"), TEXT("PerInstanceMaxMs"),
		TEXT("TotalPercentOfFrame"), TEXT("PerInstancePercentOfFrame"), TEXT("OverBudget")
	});

	for (const FPulseNiagaraCostRow& Row : Report.Rows)
	{
		for (const float Distance : Report.Distances)
		{
			const FPulseNiagaraCostSample* Sample = Row.FindSample(QualityLevel, Distance);
			if (Sample == nullptr)
			{
				continue;
			}

			// An unmeasured system still gets a row: knowing a system was skipped, and why, is a
			// finding. Its cost cells stay empty rather than zero, because a zero here would read as
			// free and that is the one lie this file must never tell.
			const bool bMeasured = Sample->bMeasured;
			const bool bOverBudget = bMeasured && Report.FrameBudgetMs > 0.0f
				&& Sample->PerInstanceAvgMs / Report.FrameBudgetMs * 100.0 > Budget.MaxFrameBudgetPercentPerInstance;

			Csv.WriteRow({
				Row.AssetName,
				Row.AssetPath,
				Row.EffectType,
				Row.NumEmitters >= 0 ? FString::FromInt(Row.NumEmitters) : FString(),
				PulseFormatBool(Row.bHasGPUEmitter),
				PulseFormatDistance(Sample->Distance),
				PulseFormatBool(bMeasured),
				Sample->SkipReason,
				FString::FromInt(Sample->InstancesSpawned),
				bMeasured ? FString::FromInt(Sample->InstancesActiveAtEnd) : FString(),
				bMeasured ? FString::FromInt(Sample->FramesRecorded) : FString(),
				bMeasured ? FString::FromInt(Sample->IdleFrames) : FString(),
				bMeasured ? PulseFormatMilliseconds(Sample->TotalAvgMs) : FString(),
				bMeasured ? PulseFormatMilliseconds(Sample->TotalMinMs) : FString(),
				bMeasured ? PulseFormatMilliseconds(Sample->TotalMaxMs) : FString(),
				bMeasured ? PulseFormatMilliseconds(Sample->PerInstanceAvgMs) : FString(),
				bMeasured ? PulseFormatMilliseconds(Sample->PerInstanceMinMs) : FString(),
				bMeasured ? PulseFormatMilliseconds(Sample->PerInstanceMaxMs) : FString(),
				bMeasured ? PulseFormatFrameShare(Sample->TotalAvgMs, Report.FrameBudgetMs) : FString(),
				bMeasured ? PulseFormatFrameShare(Sample->PerInstanceAvgMs, Report.FrameBudgetMs) : FString(),
				bMeasured ? PulseFormatBool(bOverBudget) : FString()
			});
		}
	}

	return Csv.SaveToFile(RunDir / (PulseSanitizeName(QualityName(Report, QualityIndex)) + TEXT(".csv")), OutError);
}

bool FPulseNiagaraCostReportWriter::WriteComparisonCsv(const FPulseNiagaraCostReport& Report, FString& OutError) const
{
	// One row per system, every measured axis point side by side. This is the file the run actually
	// exists to produce: a single system's milliseconds are hard to judge, but the same system
	// costing the same at Low as at Epic — or as far away as up close — is an unambiguous defect.
	const bool bHasDistanceAxis = Report.Distances.Num() > 0 && Report.Distances[0] > GPulseNiagaraNoDistance;

	TArray<FString> Header;
	Header.Add(TEXT("System"));
	Header.Add(TEXT("AssetPath"));

	for (int32 QualityIndex = 0; QualityIndex < Report.QualityLevels.Num(); ++QualityIndex)
	{
		const FString Quality = PulseSanitizeName(QualityName(Report, QualityIndex));
		for (const float Distance : Report.Distances)
		{
			const FString Prefix = bHasDistanceAxis
				? FString::Printf(TEXT("%s_%.0fcm"), *Quality, Distance)
				: Quality;

			Header.Add(Prefix + TEXT("_AvgMs"));
			Header.Add(Prefix + TEXT("_MinMs"));
			Header.Add(Prefix + TEXT("_MaxMs"));
		}
	}

	Header.Add(TEXT("QualitySavingPercent"));
	Header.Add(TEXT("FlatScalability"));
	if (bHasDistanceAxis)
	{
		Header.Add(TEXT("DistanceSavingPercent"));
		Header.Add(TEXT("FlatDistanceCulling"));
	}

	FPulseCsvWriter Csv;
	Csv.WriteRow(Header);

	const float NearestDistance = Report.Distances.Num() > 0 ? Report.Distances[0] : GPulseNiagaraNoDistance;
	const float FarthestDistance = Report.Distances.Num() > 0 ? Report.Distances.Last() : GPulseNiagaraNoDistance;

	for (const FPulseNiagaraCostRow& Row : Report.Rows)
	{
		TArray<FString> Fields;
		Fields.Add(Row.AssetName);
		Fields.Add(Row.AssetPath);

		for (const int32 QualityLevel : Report.QualityLevels)
		{
			for (const float Distance : Report.Distances)
			{
				const FPulseNiagaraCostSample* Sample = Row.FindSample(QualityLevel, Distance);
				const bool bMeasured = Sample != nullptr && Sample->bMeasured;

				Fields.Add(bMeasured ? PulseFormatMilliseconds(Sample->TotalAvgMs) : FString());
				Fields.Add(bMeasured ? PulseFormatMilliseconds(Sample->TotalMinMs) : FString());
				Fields.Add(bMeasured ? PulseFormatMilliseconds(Sample->TotalMaxMs) : FString());
			}
		}

		// Quality saving compares the lowest level against the highest at the nearest distance,
		// which is why QualityLevels and Distances both keep their run order. Fewer than two levels
		// means there is nothing to compare and the cells stay empty rather than claiming zero.
		FString QualitySaving;
		FString FlatScalability;
		if (Report.QualityLevels.Num() >= 2)
		{
			double Saving = 0.0;
			const FPulseNiagaraCostSample* HighQuality = Row.FindSample(Report.QualityLevels.Last(), NearestDistance);
			const FPulseNiagaraCostSample* LowQuality = Row.FindSample(Report.QualityLevels[0], NearestDistance);
			if (PulseComputeSaving(HighQuality, LowQuality, Saving))
			{
				QualitySaving = PulseFormatPercent(Saving);
				FlatScalability = PulseFormatBool(Saving < Budget.FlatScalabilityTolerancePercent);
			}
		}
		Fields.Add(QualitySaving);
		Fields.Add(FlatScalability);

		// Distance saving compares near against far at the highest quality, where culling has the
		// most to give up. Flat here means significance culling is authored but not engaging.
		if (bHasDistanceAxis)
		{
			FString DistanceSaving;
			FString FlatDistanceCulling;
			if (Report.Distances.Num() >= 2 && Report.QualityLevels.Num() > 0)
			{
				double Saving = 0.0;
				const int32 HighestQuality = Report.QualityLevels.Last();
				const FPulseNiagaraCostSample* Near = Row.FindSample(HighestQuality, NearestDistance);
				const FPulseNiagaraCostSample* Far = Row.FindSample(HighestQuality, FarthestDistance);
				if (PulseComputeSaving(Near, Far, Saving))
				{
					DistanceSaving = PulseFormatPercent(Saving);
					FlatDistanceCulling = PulseFormatBool(Saving < Budget.FlatScalabilityTolerancePercent);
				}
			}
			Fields.Add(DistanceSaving);
			Fields.Add(FlatDistanceCulling);
		}

		Csv.WriteRow(Fields);
	}

	return Csv.SaveToFile(RunDir / TEXT("Comparison.csv"), OutError);
}

bool FPulseNiagaraCostReportWriter::WriteRunJson(const FPulseNiagaraCostReport& Report, FString& OutError) const
{
	// Unlike the asset audit's JSON, this file deliberately DOES carry a timestamp and machine name.
	// That report omits them to stay byte-identical for unchanged content; measurements are
	// machine-dependent by nature, so provenance is the more useful property here.
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("generatedAtUtc"), Report.GeneratedAtUtc);
	Writer->WriteValue(TEXT("engineVersion"), Report.EngineVersion);
	Writer->WriteValue(TEXT("machineName"), Report.MachineName);
	Writer->WriteValue(TEXT("rhi"), Report.RHIName);
	Writer->WriteValue(TEXT("renderingAvailable"), Report.bRenderingAvailable);
	Writer->WriteValue(TEXT("measuredInPlayWorld"), Report.bMeasuredInPlayWorld);
	Writer->WriteValue(TEXT("frameBudgetMs"), Report.FrameBudgetMs);

	Writer->WriteArrayStart(TEXT("qualityLevels"));
	for (int32 QualityIndex = 0; QualityIndex < Report.QualityLevels.Num(); ++QualityIndex)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("level"), Report.QualityLevels[QualityIndex]);
		Writer->WriteValue(TEXT("name"), QualityName(Report, QualityIndex));
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();

	Writer->WriteArrayStart(TEXT("distancesCm"));
	for (const float Distance : Report.Distances)
	{
		if (Distance > GPulseNiagaraNoDistance)
		{
			Writer->WriteValue(Distance);
		}
	}
	Writer->WriteArrayEnd();

	Writer->WriteObjectStart(TEXT("measurement"));
	Writer->WriteValue(TEXT("instancesPerSystem"), Report.Measurement.InstancesPerSystem);
	Writer->WriteValue(TEXT("instanceSpacing"), Report.Measurement.InstanceSpacing);
	Writer->WriteValue(TEXT("settleFrames"), Report.Measurement.SettleFrames);
	Writer->WriteValue(TEXT("measureFrames"), Report.Measurement.MeasureFrames);
	Writer->WriteValue(TEXT("fixedDeltaSeconds"), Report.Measurement.FixedDeltaSeconds);
	Writer->WriteValue(TEXT("secondsPerSystemTimeout"), Report.Measurement.SecondsPerSystemTimeout);
	Writer->WriteValue(TEXT("skipGPUSystems"), Report.Measurement.bSkipGPUSystems);
	Writer->WriteObjectEnd();

	Writer->WriteObjectStart(TEXT("totals"));
	Writer->WriteValue(TEXT("systemsDiscovered"), Report.NumSystemsDiscovered);
	Writer->WriteValue(TEXT("systemsMeasured"), Report.NumSystemsMeasured);
	Writer->WriteValue(TEXT("systemsSkipped"), Report.NumSystemsSkipped);
	Writer->WriteObjectEnd();

	Writer->WriteValue(TEXT("measuresGameThreadOnly"), true);
	Writer->WriteValue(TEXT("measurementNote"),
		TEXT("Game-thread cost only. Render-thread and GPU costs are not measured and are absent rather than zero."));

	// Recorded as data, not just prose: an uncculled figure and a culled one are not comparable, and
	// a reader holding a CSV from each needs to be able to tell them apart without guessing.
	Writer->WriteValue(TEXT("scalabilityCullingExercised"), Report.bScalabilityCullingExercised);
	Writer->WriteValue(TEXT("scalabilityCullingNote"), Report.bScalabilityCullingExercised
		? TEXT("Distance culling ran against the level editor viewport's cached views, so costs include culling.")
		: TEXT("Distance culling was paused for this run. Niagara sources its cull distance from a local PlayerController with a real viewport or from an editor level viewport, neither of which exists in a headless commandlet; left enabled, every system with a distance-culling Effect Type would cull immediately and report as free. Costs here are uncculled."));

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

void FPulseNiagaraCostReportWriter::PruneHistoricalRuns() const
{
	if (MaxHistoricalRuns <= 0)
	{
		return;
	}

	TArray<FString> RunFolders;
	IFileManager::Get().FindFiles(RunFolders, *(ReportDir / (FString(GPulseNiagaraRunPrefix) + TEXT("*"))),
		/*Files*/ false, /*Directories*/ true);

	if (RunFolders.Num() <= MaxHistoricalRuns)
	{
		return;
	}

	// Folder names embed a sortable UTC timestamp, so lexical order is chronological order and no
	// filesystem timestamp — which a copy or a sync would rewrite — is consulted.
	RunFolders.Sort();

	const int32 NumToDelete = RunFolders.Num() - MaxHistoricalRuns;
	for (int32 Index = 0; Index < NumToDelete; ++Index)
	{
		IFileManager::Get().DeleteDirectory(*(ReportDir / RunFolders[Index]), /*RequireExists*/ false, /*Tree*/ true);
	}
}
