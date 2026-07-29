// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseTypes.h"

class UPulseSettings;

/**
 * Everything a single audit run was asked to do, resolved from the command line plus settings
 * defaults. All switch parsing lives here so the commandlet's Main() stays orchestration only.
 */
struct FPulseRunConfig
{
	EPulseTier RunTier = EPulseTier::Fast;

	/** The raw requests, recorded separately so the report header can show what was asked for. */
	bool bDeepRequested = false;
	bool bShaderStatsRequested = false;

	/** Category names to include. Empty = all registered collectors. */
	TArray<FString> CategoryFilter;

	/** Package paths to scan. Empty = UPulseSettings ScanPaths. */
	TArray<FString> PathFilter;

	/** Report-row filter only. Applied at write time, after scoring — never affects scores. */
	EPulseSeverity MinSeverity = EPulseSeverity::Info;

	/** Per-category cap, applied as a prefix of the sorted asset list. 0 = unlimited. */
	int32 MaxAssetsPerCategory = 0;

	/** Absolute by the time parsing finishes. */
	FString ReportDir;

	bool bWriteCsv = true;
	bool bWriteJson = true;

	/** Exit code 1 when the overall score lands below this. 0 = never fail. */
	float FailUnderScore = 0.0f;

	int32 GCPackageInterval = 100;

	/**
	 * Parses the commandlet parameter string. Unknown values for known switches are errors (exit
	 * code 2 at the call site); unknown switches are ignored, because the engine adds its own.
	 */
	static bool Parse(const FString& Params, FPulseRunConfig& OutConfig, FString& OutError);

	/** Fills anything the command line left unset from project settings, and resolves paths. */
	void ApplySettingsDefaults(const UPulseSettings& Settings);

	/** One-line human summary for the run log. */
	FString ToString() const;

private:
	/** True once -gcfreq was given, so settings do not overwrite it. */
	bool bGCIntervalOverridden = false;
};
