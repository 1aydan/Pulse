// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "UObject/SoftObjectPath.h"
#include "PulseContentCleanupSettings.generated.h"

/**
 * Project configuration for the Pulse content cleanup tool, stored in Config/DefaultPulse.ini under
 * [/Script/Pulse.PulseContentCleanupSettings].
 *
 * What counts as "used" — the maps, primary assets and always-loaded assets that keep content alive —
 * is not configured here. It comes from Project Settings > Packaging (Maps to Cook, Additional Asset
 * Directories to Cook), the Asset Manager, Maps & Modes, and the Memory Auditor's AlwaysLoadedAssets:
 * the same sources the cooker reads, so the two cannot disagree about what ships.
 */
UCLASS(Config = Pulse, DefaultConfig, meta = (DisplayName = "Pulse - Content Cleanup"))
class PULSE_API UPulseContentCleanupSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPulseContentCleanupSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/** Only assets under these paths are reported. Reachability is still worked out project-wide. */
	UPROPERTY(EditAnywhere, Config, Category = "Scan", meta = (ContentDir))
	TArray<FDirectoryPath> ScanPaths;

	/** Assets whose package path contains any of these substrings are never reported. */
	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	TArray<FString> ExcludePathPatterns;

	/**
	 * Asset classes never reported as unused, by class name. Editor tooling lives here: it is meant to
	 * be referenced by nothing in the game.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	TArray<FString> IgnoredClassNames;

	/**
	 * Asset classes whose every asset counts as used, along with everything it references — for
	 * content a game finds at runtime by scanning the registry or a folder rather than by reference,
	 * which no static walk can see. Typically a project's own data asset or definition classes.
	 * Subclasses are included.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Scan", meta = (AllowAbstract))
	TArray<FSoftClassPath> AlwaysUsedClasses;

	/**
	 * When the project lists Maps to Cook, only those maps keep content alive, so content that only a
	 * demo or test map references is reported too. Off treats every map as shipping, which is what the
	 * cooker does when the list is empty.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	bool bRespectMapsToCook = true;

	/**
	 * Searches the project's .ini files and C++ source for package paths, and flags any unused asset
	 * named there instead of presenting it as safe to remove. The Asset Registry cannot see a path in
	 * a config value or a ConstructorHelpers call, and those are the usual way a "dead" asset turns out
	 * to be live.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	bool bSearchConfigAndSource = true;

	/** Duplicate groups whose copies waste less than this on disk are not reported, in kilobytes. */
	UPROPERTY(EditAnywhere, Config, Category = "Duplicates", meta = (ClampMin = 0))
	int32 MinDuplicateWasteKB = 64;

	/** Relative paths resolve against the project directory. */
	UPROPERTY(EditAnywhere, Config, Category = "Report")
	FString ReportDirectory = TEXT("Saved/Pulse/ContentCleanup");

	/** Historical run folders kept; oldest pruned. 0 = keep all. */
	UPROPERTY(EditAnywhere, Config, Category = "Report", meta = (ClampMin = 0))
	int32 MaxHistoricalRuns = 20;
};
