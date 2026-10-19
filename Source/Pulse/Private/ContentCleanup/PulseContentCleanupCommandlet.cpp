// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseContentCleanupCommandlet.h"

#include "MemoryAuditor/PulseMemoryEstimator.h"
#include "Pulse.h"
#include "PulseCleanupAnalyzer.h"
#include "PulseCleanupReportWriter.h"
#include "PulseContentCleanupSettings.h"

UPulseContentCleanupCommandlet::UPulseContentCleanupCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = false;
}

int32 UPulseContentCleanupCommandlet::Main(const FString& Params)
{
	FPulseCleanupConfig Config;
	FString Error;
	if (!FPulseCleanupConfig::Parse(Params, Config, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Bad invocation: %s"), *Error);
		return 2;
	}
	const UPulseContentCleanupSettings& Settings = *GetDefault<UPulseContentCleanupSettings>();
	Config.ApplySettingsDefaults(Settings);

	UE_LOG(LogPulse, Display, TEXT("Pulse content cleanup starting: %s"), *Config.ToString());

	FPulseCleanupReport Report;
	FPulseCleanupAnalyzer Analyzer(Config);
	if (!Analyzer.Run(Report, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Content cleanup failed: %s"), *Error);
		return 2;
	}

	FPulseCleanupReportWriter Writer(Config.ReportDir, Config.MaxHistoricalRuns);
	if (!Writer.Write(Report, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Report write failed: %s"), *Error);
		return 1;
	}

	int32 NumMentioned = 0;
	for (const FPulseCleanupAsset& Asset : Report.Unused)
	{
		NumMentioned += Asset.IsMentioned() ? 1 : 0;
	}

	UE_LOG(LogPulse, Display, TEXT("Pulse content cleanup complete. %d of %d assets unused or redirectors (%s on disk), %d named in config or source."),
		Report.Unused.Num(), Report.NumAssetsScanned, *PulseFormatBytes(Report.GetUnusedDiskBytes(/*bIncludeMentioned*/ true)), NumMentioned);
	UE_LOG(LogPulse, Display, TEXT("%d duplicate import group(s) wasting %s on disk."),
		Report.Duplicates.Num(), *PulseFormatBytes(Report.GetDuplicateWastedDiskBytes()));
	UE_LOG(LogPulse, Display, TEXT("Reports written to %s"), *Writer.GetRunDir());
	return 0;
}
