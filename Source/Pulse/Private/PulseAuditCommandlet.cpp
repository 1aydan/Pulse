// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseAuditCommandlet.h"

#include "Pulse.h"
#include "PulseReportWriter.h"
#include "PulseResult.h"
#include "PulseRunConfig.h"
#include "PulseScanDriver.h"
#include "PulseSettings.h"

UPulseAuditCommandlet::UPulseAuditCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = false;
}

int32 UPulseAuditCommandlet::Main(const FString& Params)
{
	FPulseRunConfig Config;
	FString Error;
	if (!FPulseRunConfig::Parse(Params, Config, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Bad invocation: %s"), *Error);
		return 2;
	}

	const UPulseSettings& Settings = *GetDefault<UPulseSettings>();
	Config.ApplySettingsDefaults(Settings);

	if (Config.bShaderStatsRequested && !Config.bDeepRequested)
	{
		UE_LOG(LogPulse, Display, TEXT("-shaderstats implies -deep; running at tier ShaderStats."));
	}

	UE_LOG(LogPulse, Display, TEXT("Pulse audit starting: %s"), *Config.ToString());
	UE_LOG(LogPulse, Display, TEXT("Settings hash: %s"), *Settings.ComputeSettingsHash());

	// Collector selection — the -category= filter and the settings' DisabledCategories — lives in the
	// driver, so the commandlet and the editor panel cannot drift apart on which assets get audited.
	FPulseScanDriver Driver(Config, Settings);
	if (!Driver.Initialize(Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Scan initialization failed: %s"), *Error);
		return 2;
	}

	// Unbounded budget: the commandlet has no frame to yield to, so one Step drains everything.
	while (Driver.Step(/*TimeBudgetSeconds*/ 0.0))
	{
	}

	const FPulseReport& Report = Driver.FinalizeReport();

	FPulseReportWriter Writer(Config.ReportDir, Config.bWriteCsv, Config.bWriteJson, Settings.Scan.MaxHistoricalSnapshots);
	if (!Writer.Write(Report, Config.MinSeverity, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Report write failed: %s"), *Error);
		return 1;
	}

	UE_LOG(LogPulse, Display, TEXT("Pulse audit complete. Overall score: %.2f"), Report.OverallScore);
	for (const FPulseCategoryResult& Category : Report.Categories)
	{
		UE_LOG(LogPulse, Display, TEXT("  %s: %.2f (%d assets, %d with issues)"),
			*Category.Category.ToString(), Category.Score, Category.NumAssets, Category.NumAssetsWithIssues);
	}

	if (Config.FailUnderScore > 0.0f && Report.OverallScore < Config.FailUnderScore)
	{
		UE_LOG(LogPulse, Error, TEXT("Overall score %.2f is below -failunder=%.2f."), Report.OverallScore, Config.FailUnderScore);
		return 1;
	}

	return 0;
}
