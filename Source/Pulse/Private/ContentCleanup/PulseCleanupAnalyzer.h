// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseCleanupResult.h"

class IAssetRegistry;
class UPulseContentCleanupSettings;

/** Everything one cleanup run was asked to do, from the command line plus settings defaults. */
struct FPulseCleanupConfig
{
	/** Paths whose assets are reported. Empty = the settings' ScanPaths. */
	TArray<FString> PathFilter;

	/** Absolute once ApplySettingsDefaults has run. */
	FString ReportDir;

	TArray<FString> ExcludePathPatterns;
	TArray<FString> IgnoredClassNames;
	TArray<FSoftClassPath> AlwaysUsedClasses;
	bool bRespectMapsToCook = true;
	bool bSearchConfigAndSource = true;
	int32 MinDuplicateWasteKB = 64;
	int32 MaxHistoricalRuns = 20;

	/** -path=, -reportdir=, -allmaps (ignore Maps to Cook), -nosearch (skip the config/source search). */
	static bool Parse(const FString& Params, FPulseCleanupConfig& OutConfig, FString& OutError);

	void ApplySettingsDefaults(const UPulseContentCleanupSettings& Settings);

	FString ToString() const;

private:
	bool bAllMapsFromCommandLine = false;
	bool bNoSearchFromCommandLine = false;
};

/**
 * Finds content nothing that ships can load, and imports that exist more than once.
 *
 * "Used" is decided by the memory auditor's dependency walk from the same roots the cooker starts
 * from — maps (or Maps to Cook when it is set), primary assets, the Maps & Modes classes, always-loaded
 * assets, and Additional Asset Directories to Cook — following hard AND soft references, because a soft
 * reference is still a way to be loaded. Anything under the scan paths that walk never reaches is
 * reported, with its referencers and any config or source file naming it.
 *
 * Registry-only like the memory auditor: nothing is loaded and nothing is ever deleted.
 */
class FPulseCleanupAnalyzer
{
public:
	using FProgressCallback = TFunction<bool(const FText& Stage, float Fraction)>;

	explicit FPulseCleanupAnalyzer(const FPulseCleanupConfig& InConfig);

	bool Run(FPulseCleanupReport& OutReport, FString& OutError, const FProgressCallback& Progress = FProgressCallback());

private:
	bool IsExcluded(FName PackageName) const;

	/** Marks every unused asset named in a project .ini or C++ file. Returns the number of files read. */
	int32 SearchConfigAndSource(FPulseCleanupReport& Report) const;

	const FPulseCleanupConfig Config;
};
