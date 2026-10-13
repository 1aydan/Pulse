// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseMemoryAuditorSettings.h"

/**
 * Everything one memory audit was asked to do, resolved from the command line plus settings
 * defaults. Parsing lives here so the commandlet's Main() stays orchestration only — the same split
 * FPulseNiagaraAuditorConfig makes.
 */
struct FPulseMemoryAuditorConfig
{
	/** Package paths top-level assets are taken from. Empty = the settings' ScanPaths. */
	TArray<FString> PathFilter;

	/** Absolute by the time ApplySettingsDefaults finishes. Receives one timestamped subfolder per run. */
	FString ReportDir;

	/** Exit code 1 when the baseline or any top-level asset exceeds a configured budget. */
	bool bFailOverBudget = false;

	/** Settings with command-line overrides folded in, copied so the run cannot see a later edit. */
	FPulseMemoryRootSettings Roots;
	FPulseMemoryEstimationSettings Estimation;
	FPulseMemoryBudget Budget;
	TArray<FString> ExcludePathPatterns;
	int32 MaxDependenciesPerRootInReport = 100;
	int32 MaxHistoricalRuns = 20;

	/**
	 * Parses the commandlet parameter string. Unknown values for known switches are errors; unknown
	 * switches are ignored, because the engine adds its own.
	 */
	static bool Parse(const FString& Params, FPulseMemoryAuditorConfig& OutConfig, FString& OutError);

	/** Fills anything the command line left unset from project settings, and resolves paths. */
	void ApplySettingsDefaults(const UPulseMemoryAuditorSettings& Settings);

	/** One-line human summary for the run log. */
	FString ToString() const;

private:
	bool bSoftReferencesOverridden = false;
	bool bIncludeSoftReferencesFromCommandLine = true;
};
