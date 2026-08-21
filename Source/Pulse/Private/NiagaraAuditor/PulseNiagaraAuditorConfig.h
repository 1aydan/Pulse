// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseNiagaraAuditorSettings.h"

class UPulseNiagaraAuditorSettings;

/**
 * Everything one auditor run was asked to do, resolved from the command line plus settings
 * defaults. All switch parsing lives here so the commandlet's Main() stays orchestration only —
 * the same split FPulseRunConfig makes for the asset audit.
 */
struct FPulseNiagaraAuditorConfig
{
	/** Package paths to scan. Empty = the settings' ScanPaths. */
	TArray<FString> PathFilter;

	/** Cap on systems measured, applied to the sorted list. 0 = unlimited. */
	int32 MaxSystems = 0;

	/** Absolute by the time parsing finishes. Receives one timestamped subfolder per run. */
	FString ReportDir;

	/** Exit code 1 when any system lands over budget. Off by default: reporting is not gating. */
	bool bFailOverBudget = false;

	/** Settings with any command-line overrides already folded in. Handed to the stage verbatim. */
	FPulseNiagaraMeasurementSettings Measurement;

	/** Thresholds, copied so the report and the exit code cannot disagree about them. */
	FPulseNiagaraCostBudget Budget;

	/** Resolved frame budget in milliseconds. */
	float FrameBudgetMs = 0.0f;

	/**
	 * Parses the commandlet parameter string. Unknown values for known switches are errors; unknown
	 * switches are ignored, because the engine adds its own.
	 */
	static bool Parse(const FString& Params, FPulseNiagaraAuditorConfig& OutConfig, FString& OutError);

	/** Fills anything the command line left unset from project settings, and resolves paths. */
	void ApplySettingsDefaults(const UPulseNiagaraAuditorSettings& Settings);

	/** One-line human summary for the run log. */
	FString ToString() const;

private:
	/** Set once the matching switch was given, so settings do not overwrite an explicit choice. */
	bool bInstancesOverridden = false;
	bool bMeasureFramesOverridden = false;
	bool bSettleFramesOverridden = false;
	bool bQualityLevelsOverridden = false;
	bool bSkipGPUOverridden = false;
};
