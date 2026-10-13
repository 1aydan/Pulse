// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "UObject/SoftObjectPath.h"
#include "PulseMemoryAuditorSettings.generated.h"

/** Where the auditor looks for top-level assets and where it writes. Mirrors FPulseScanSettings. */
USTRUCT()
struct FPulseMemoryAuditorScanSettings
{
	GENERATED_BODY()

	/**
	 * Top-level assets are only taken from under these paths. Dependencies are followed wherever they
	 * lead — engine and plugin content included — because a reference into /Engine costs memory just
	 * the same.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Scan", meta = (ContentDir))
	TArray<FDirectoryPath> ScanPaths;

	/** Top-level candidates whose package path contains any of these substrings are skipped. */
	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	TArray<FString> ExcludePathPatterns;

	/** Relative paths resolve against the project directory. */
	UPROPERTY(EditAnywhere, Config, Category = "Report")
	FString ReportDirectory = TEXT("Saved/Pulse/MemoryAuditor");

	/** Historical run folders kept; oldest pruned. 0 = keep all. */
	UPROPERTY(EditAnywhere, Config, Category = "Report", meta = (ClampMin = 0))
	int32 MaxHistoricalRuns = 20;

	/**
	 * Dependencies listed per top-level asset in Dependencies.csv, largest first. The panel always
	 * shows the full list; the CSV is capped because a map can reach tens of thousands of packages.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Report", meta = (ClampMin = 1))
	int32 MaxDependenciesPerRootInReport = 100;
};

/**
 * What counts as top-level: the assets whose loading is decided by something other than another
 * asset's reference. Each kind is listed separately in the results so it can be filtered.
 */
USTRUCT()
struct FPulseMemoryRootSettings
{
	GENERATED_BODY()

	/** Every map under the scan paths. */
	UPROPERTY(EditAnywhere, Config, Category = "Roots")
	bool bIncludeMaps = true;

	/** Every primary asset the Asset Manager knows about under the scan paths. */
	UPROPERTY(EditAnywhere, Config, Category = "Roots")
	bool bIncludePrimaryAssets = true;

	/**
	 * Treats the global default GameMode and the GameInstance class from Maps & Modes as always
	 * loaded. Their hard references — pawn, controller, HUD — are resident in every map, so this is
	 * usually the most important set in the report.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Roots")
	bool bIncludeGameMapsSettings = true;

	/**
	 * More assets that are resident for the whole session: a game-wide data asset, a UI root widget,
	 * a subsystem's config. Together with the Maps & Modes classes these form the baseline — the
	 * memory paid in every map before the map itself loads anything.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Roots")
	TArray<FSoftObjectPath> AlwaysLoadedAssets;

	/**
	 * Makes every asset of these classes under the scan paths top-level, e.g. /Script/Engine.Blueprint
	 * to rank every Blueprint by what loading it drags in. Subclasses are included.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Roots", meta = (AllowAbstract))
	TArray<FSoftClassPath> AdditionalRootClasses;
};

/**
 * How sizes are estimated from Asset Registry tags. Nothing here loads a package; the knobs exist
 * because some costs depend on project settings the registry cannot see.
 */
USTRUCT()
struct FPulseMemoryEstimationSettings
{
	GENERATED_BODY()

	/**
	 * Also follows soft references and reports what they could bring in as "may load". Soft
	 * references are loaded by code on demand, so they never count toward the totals.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Estimation")
	bool bIncludeSoftReferences = true;

	/**
	 * Compressed audio is estimated as uncompressed 16-bit PCM divided by this ratio. Around 8-12 for
	 * typical Ogg/Bink quality settings; lower for ADPCM (about 4) and 1 for PCM.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Estimation", meta = (ClampMin = 1.0))
	float AudioCompressionRatio = 8.0f;
};

/**
 * Optional ceilings. 0 disables a check. Megabytes rather than bytes so they can be typed by hand.
 */
USTRUCT()
struct FPulseMemoryBudget
{
	GENERATED_BODY()

	/** Ceiling on what the always-loaded set keeps resident in system memory. */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0))
	int32 BaselineRAMBudgetMB = 0;

	/** Ceiling on what the always-loaded set keeps resident in video memory. */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0))
	int32 BaselineVRAMBudgetMB = 0;

	/** Ceiling on any one top-level asset's total system memory. */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0))
	int32 RootRAMBudgetMB = 0;

	/** Ceiling on any one top-level asset's total video memory. */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0))
	int32 RootVRAMBudgetMB = 0;
};

/**
 * Project configuration for the Pulse memory auditor, stored in Config/DefaultPulse.ini under
 * [/Script/Pulse.PulseMemoryAuditorSettings].
 *
 * Separate from UPulseSettings for the same reason the Niagara auditor's settings are: that class's
 * SettingsHash stamps the asset audit's reports, and adding a budget here must not change it.
 */
UCLASS(Config = Pulse, DefaultConfig, meta = (DisplayName = "Pulse - Memory Auditor"))
class PULSE_API UPulseMemoryAuditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPulseMemoryAuditorSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	FPulseMemoryAuditorScanSettings Scan;

	UPROPERTY(EditAnywhere, Config, Category = "Roots")
	FPulseMemoryRootSettings Roots;

	UPROPERTY(EditAnywhere, Config, Category = "Estimation")
	FPulseMemoryEstimationSettings Estimation;

	UPROPERTY(EditAnywhere, Config, Category = "Budget")
	FPulseMemoryBudget Budget;
};
