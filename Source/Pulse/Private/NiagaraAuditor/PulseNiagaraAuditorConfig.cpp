// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseNiagaraAuditorConfig.h"

#include "Misc/Parse.h"
#include "Misc/Paths.h"

/** Splits a comma-separated switch value, trimming each entry and dropping empties. */
static TArray<FString> PulseSplitList(const FString& Value)
{
	TArray<FString> Entries;
	Value.ParseIntoArray(Entries, TEXT(","), /*InCullEmpty*/ true);
	for (FString& Entry : Entries)
	{
		Entry.TrimStartAndEndInline();
	}
	Entries.RemoveAll([](const FString& Entry) { return Entry.IsEmpty(); });
	return Entries;
}

/** Reads a positive integer switch. Absent leaves OutValue alone; present-but-bad is an error. */
static bool PulseParsePositiveInt(const FString& Params, const TCHAR* Switch, int32& OutValue, bool& bOutWasGiven, FString& OutError)
{
	FString RawValue;
	if (!FParse::Value(*Params, Switch, RawValue))
	{
		return true;
	}

	if (!RawValue.IsNumeric())
	{
		OutError = FString::Printf(TEXT("-%s expects a number, got '%s'."), Switch, *RawValue);
		return false;
	}

	const int32 Parsed = FCString::Atoi(*RawValue);
	if (Parsed < 1)
	{
		OutError = FString::Printf(TEXT("-%s must be at least 1, got %d."), Switch, Parsed);
		return false;
	}

	OutValue = Parsed;
	bOutWasGiven = true;
	return true;
}

bool FPulseNiagaraAuditorConfig::Parse(const FString& Params, FPulseNiagaraAuditorConfig& OutConfig, FString& OutError)
{
	FString RawValue;

	if (FParse::Value(*Params, TEXT("path="), RawValue))
	{
		OutConfig.PathFilter = PulseSplitList(RawValue);
		if (OutConfig.PathFilter.Num() == 0)
		{
			OutError = TEXT("-path= was given but listed no paths.");
			return false;
		}
	}

	if (FParse::Value(*Params, TEXT("quality="), RawValue))
	{
		const TArray<FString> Entries = PulseSplitList(RawValue);
		if (Entries.Num() == 0)
		{
			OutError = TEXT("-quality= was given but listed no levels.");
			return false;
		}

		OutConfig.Measurement.QualityLevelsToMeasure.Reset();
		for (const FString& Entry : Entries)
		{
			if (!Entry.IsNumeric())
			{
				OutError = FString::Printf(TEXT("-quality= expects numbers, got '%s'."), *Entry);
				return false;
			}
			OutConfig.Measurement.QualityLevelsToMeasure.AddUnique(FCString::Atoi(*Entry));
		}
		OutConfig.bQualityLevelsOverridden = true;
	}

	if (!PulseParsePositiveInt(Params, TEXT("instances="), OutConfig.Measurement.InstancesPerSystem, OutConfig.bInstancesOverridden, OutError))
	{
		return false;
	}
	if (!PulseParsePositiveInt(Params, TEXT("frames="), OutConfig.Measurement.MeasureFrames, OutConfig.bMeasureFramesOverridden, OutError))
	{
		return false;
	}

	// Settle frames alone may legitimately be zero, so it does not go through the positive-int path.
	if (FParse::Value(*Params, TEXT("settle="), RawValue))
	{
		if (!RawValue.IsNumeric() || FCString::Atoi(*RawValue) < 0)
		{
			OutError = FString::Printf(TEXT("-settle= expects a non-negative number, got '%s'."), *RawValue);
			return false;
		}
		OutConfig.Measurement.SettleFrames = FCString::Atoi(*RawValue);
		OutConfig.bSettleFramesOverridden = true;
	}

	bool bMaxSystemsGiven = false;
	if (!PulseParsePositiveInt(Params, TEXT("maxsystems="), OutConfig.MaxSystems, bMaxSystemsGiven, OutError))
	{
		return false;
	}

	if (FParse::Value(*Params, TEXT("reportdir="), RawValue))
	{
		OutConfig.ReportDir = RawValue.TrimQuotes();
	}

	// Opt-in rather than opt-out: a GPU system measured headlessly is wrong, not merely incomplete,
	// so including them has to be a deliberate act with the caveat understood.
	if (FParse::Param(*Params, TEXT("includegpu")))
	{
		OutConfig.Measurement.bSkipGPUSystems = false;
		OutConfig.bSkipGPUOverridden = true;
	}

	OutConfig.bFailOverBudget = FParse::Param(*Params, TEXT("failoverbudget"));
	return true;
}

void FPulseNiagaraAuditorConfig::ApplySettingsDefaults(const UPulseNiagaraAuditorSettings& Settings)
{
	// Start from the settings, then re-apply only the fields the command line actually named. Doing
	// it in this order means a new measurement setting is picked up automatically instead of needing
	// a matching line here.
	const FPulseNiagaraMeasurementSettings CommandLineMeasurement = Measurement;
	Measurement = Settings.Measurement;

	if (bInstancesOverridden)
	{
		Measurement.InstancesPerSystem = CommandLineMeasurement.InstancesPerSystem;
	}
	if (bMeasureFramesOverridden)
	{
		Measurement.MeasureFrames = CommandLineMeasurement.MeasureFrames;
	}
	if (bSettleFramesOverridden)
	{
		Measurement.SettleFrames = CommandLineMeasurement.SettleFrames;
	}
	if (bQualityLevelsOverridden)
	{
		Measurement.QualityLevelsToMeasure = CommandLineMeasurement.QualityLevelsToMeasure;
	}
	if (bSkipGPUOverridden)
	{
		Measurement.bSkipGPUSystems = CommandLineMeasurement.bSkipGPUSystems;
	}

	if (PathFilter.Num() == 0)
	{
		for (const FDirectoryPath& Directory : Settings.Scan.ScanPaths)
		{
			if (!Directory.Path.IsEmpty())
			{
				PathFilter.Add(Directory.Path);
			}
		}
	}

	// An empty quality list would silently measure nothing; Epic quality is the least surprising
	// single level to fall back to.
	if (Measurement.QualityLevelsToMeasure.Num() == 0)
	{
		Measurement.QualityLevelsToMeasure.Add(3);
	}

	Budget = Settings.Budget;
	FrameBudgetMs = Settings.GetFrameBudgetMs();

	if (ReportDir.IsEmpty())
	{
		ReportDir = Settings.Scan.ReportDirectory;
	}
	if (FPaths::IsRelative(ReportDir))
	{
		ReportDir = FPaths::ProjectDir() / ReportDir;
	}
	ReportDir = FPaths::ConvertRelativePathToFull(ReportDir);
}

FString FPulseNiagaraAuditorConfig::ToString() const
{
	TArray<FString> QualityStrings;
	for (const int32 QualityLevel : Measurement.QualityLevelsToMeasure)
	{
		QualityStrings.Add(FString::FromInt(QualityLevel));
	}

	return FString::Printf(
		TEXT("paths=[%s] quality=[%s] instances=%d settle=%d frames=%d maxsystems=%d skipgpu=%s reportdir=%s"),
		*FString::Join(PathFilter, TEXT(",")),
		*FString::Join(QualityStrings, TEXT(",")),
		Measurement.InstancesPerSystem,
		Measurement.SettleFrames,
		Measurement.MeasureFrames,
		MaxSystems,
		Measurement.bSkipGPUSystems ? TEXT("true") : TEXT("false"),
		*ReportDir);
}
