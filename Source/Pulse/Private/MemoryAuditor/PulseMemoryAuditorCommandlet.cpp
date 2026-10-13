// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseMemoryAuditorCommandlet.h"

#include "Pulse.h"
#include "PulseMemoryAnalyzer.h"
#include "PulseMemoryAuditorConfig.h"
#include "PulseMemoryAuditorSettings.h"
#include "PulseMemoryEstimator.h"
#include "PulseMemoryReportWriter.h"
#include "PulseMemoryResult.h"

UPulseMemoryAuditorCommandlet::UPulseMemoryAuditorCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// A read-only registry walk, like the asset audit: no world, no rendering, no client.
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = false;
}

int32 UPulseMemoryAuditorCommandlet::Main(const FString& Params)
{
	FPulseMemoryAuditorConfig Config;
	FString Error;
	if (!FPulseMemoryAuditorConfig::Parse(Params, Config, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Bad invocation: %s"), *Error);
		return 2;
	}

	const UPulseMemoryAuditorSettings& Settings = *GetDefault<UPulseMemoryAuditorSettings>();
	Config.ApplySettingsDefaults(Settings);

	UE_LOG(LogPulse, Display, TEXT("Pulse memory auditor starting: %s"), *Config.ToString());

	FPulseMemoryReport Report;
	FPulseMemoryAnalyzer Analyzer(Config);
	if (!Analyzer.Run(Report, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Memory audit failed: %s"), *Error);
		return 2;
	}

	FPulseMemoryReportWriter Writer(Config.ReportDir, Config.MaxDependenciesPerRootInReport, Config.MaxHistoricalRuns);
	if (!Writer.Write(Report, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Report write failed: %s"), *Error);
		return 1;
	}

	int32 NumRootsOverBudget = 0;
	for (const FPulseMemoryRoot& Root : Report.Roots)
	{
		if (Report.IsRootOverBudget(Root))
		{
			NumRootsOverBudget++;
		}
	}
	const bool bBaselineOverBudget = Report.IsBaselineOverBudget();

	UE_LOG(LogPulse, Display, TEXT("Pulse memory audit complete. %d top-level assets, %d packages reached by hard reference."),
		Report.Roots.Num(), Report.NumReachablePackages);
	UE_LOG(LogPulse, Display, TEXT("Always-loaded baseline: %s RAM, %s VRAM across %d packages (estimated)."),
		*PulseFormatBytes(Report.Baseline.RAMBytes), *PulseFormatBytes(Report.Baseline.VRAMBytes), Report.NumBaselinePackages);
	UE_LOG(LogPulse, Display, TEXT("Reports written to %s"), *Writer.GetRunDir());

	// An inspected asset was asked about by name, so it gets the same report the panel's right-click
	// writes: the biggest cuts for that one asset, ready to hand to whoever will make them.
	for (int32 RootIndex = 0; RootIndex < Report.Roots.Num(); ++RootIndex)
	{
		if (!Report.Roots[RootIndex].bInspected)
		{
			continue;
		}
		if (!Writer.WriteAssetReport(Report, RootIndex, Error))
		{
			UE_LOG(LogPulse, Error, TEXT("Asset report failed: %s"), *Error);
			return 1;
		}
		UE_LOG(LogPulse, Display, TEXT("Asset report for %s written to %s"),
			*Report.Nodes[Report.Roots[RootIndex].NodeIndex].AssetName, *Writer.GetAssetReportDir());
	}

	if (bBaselineOverBudget)
	{
		UE_LOG(LogPulse, Warning, TEXT("The always-loaded baseline exceeds its budget."));
	}
	if (NumRootsOverBudget > 0)
	{
		UE_LOG(LogPulse, Warning, TEXT("%d top-level asset(s) exceed their budget. See OverBudget in Roots.csv."), NumRootsOverBudget);
	}

	if (Config.bFailOverBudget && (bBaselineOverBudget || NumRootsOverBudget > 0))
	{
		return 1;
	}
	return 0;
}
