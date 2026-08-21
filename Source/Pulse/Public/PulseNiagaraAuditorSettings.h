// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "PulseTypes.h"
#include "UObject/SoftObjectPtr.h"
#include "PulseNiagaraAuditorSettings.generated.h"

class UWorld;

/** Where the auditor looks and where it writes. Mirrors FPulseScanSettings deliberately. */
USTRUCT()
struct FPulseNiagaraAuditorScanSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, Category = "Scan", meta = (ContentDir))
	TArray<FDirectoryPath> ScanPaths;

	/** Package paths containing any of these substrings are skipped entirely. */
	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	TArray<FString> ExcludePathPatterns;

	/** Relative paths resolve against the project directory. */
	UPROPERTY(EditAnywhere, Config, Category = "Report")
	FString ReportDirectory = TEXT("Saved/Pulse/NiagaraAuditor");

	/** Historical run folders kept; oldest pruned. 0 = keep all. */
	UPROPERTY(EditAnywhere, Config, Category = "Report", meta = (ClampMin = 0))
	int32 MaxHistoricalRuns = 20;
};

/**
 * How each system is exercised. Every value here changes what the numbers mean, so all of them are
 * echoed into Run.json — a CSV whose measurement parameters are unknown is not evidence of anything.
 */
USTRUCT()
struct FPulseNiagaraMeasurementSettings
{
	GENERATED_BODY()

	/**
	 * Instances spawned per system. Niagara batches instances of one system into a single
	 * FNiagaraSystemSimulation, so per-instance cost falls as this rises; 1 measures the unbatched
	 * worst case and never the path shipping code actually takes. Both total and per-instance
	 * figures are reported either way, so this chooses the operating point, not the output.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Measurement", meta = (ClampMin = 1, ClampMax = 256))
	int32 InstancesPerSystem = 16;

	/** Grid spacing between instances, in centimetres. */
	UPROPERTY(EditAnywhere, Config, Category = "Measurement", meta = (ClampMin = 1.0))
	float InstanceSpacing = 250.0f;

	/**
	 * Frames ticked before measurement begins, discarded. Spawn, allocation and first-tick costs
	 * all land here; recording them makes a system look expensive in proportion to how early it was
	 * sampled rather than how expensive it is.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Measurement", meta = (ClampMin = 0))
	int32 SettleFrames = 30;

	/** Frames actually recorded. Longer is steadier and linearly slower. */
	UPROPERTY(EditAnywhere, Config, Category = "Measurement", meta = (ClampMin = 1))
	int32 MeasureFrames = 120;

	/**
	 * Simulated frame delta. Fixed, never wall-clock: a headless commandlet ticks as fast as the
	 * machine allows, and a variable delta changes spawn counts and particle lifetimes run to run,
	 * which would make the same content measure differently on a faster box.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Measurement", meta = (ClampMin = 0.001, ClampMax = 1.0))
	float FixedDeltaSeconds = 1.0f / 60.0f;

	/** Wall-clock ceiling for one system at one quality level. Aborts rather than hanging CI. */
	UPROPERTY(EditAnywhere, Config, Category = "Measurement", meta = (ClampMin = 1.0))
	float SecondsPerSystemTimeout = 30.0f;

	/** Systems measured between garbage collections. */
	UPROPERTY(EditAnywhere, Config, Category = "Measurement", meta = (ClampMin = 1))
	int32 GCSystemInterval = 25;

	/**
	 * Niagara quality levels to measure, as fx.Niagara.QualityLevel values. The count and the names
	 * come from UNiagaraSettings::QualityLevels, so a project that renamed or added levels gets its
	 * own names on the CSVs. Every added level multiplies total run time.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Quality")
	TArray<int32> QualityLevelsToMeasure;

	/**
	 * Skips systems with a GPU emitter instead of reporting them. A headless run issues no scene
	 * render, so GPU simulation never dispatches and such a system measures as nearly free. That is
	 * not a missing number, it is a wrong one, so the default is to skip and say why.
	 *
	 * Ignored by the editor panel, which runs in a world the viewport actually renders.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Quality")
	bool bSkipGPUSystems = true;

	/**
	 * Distances in centimetres, from the viewport camera, at which each system is measured.
	 *
	 * EDITOR PANEL ONLY, and not an arbitrary restriction. Niagara culls by distance using views
	 * cached from a local PlayerController with a real viewport, or from the level editor viewport;
	 * a commandlet has neither, so it pauses culling entirely and measures a single uncculled cost.
	 * Distance means nothing there and the commandlet ignores this list.
	 *
	 * A system that costs the same near and far has significance culling that is not doing
	 * anything, which is exactly what the near/far spread in Comparison.csv is for. One distance is
	 * a sample, not a test.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Distance")
	TArray<float> MeasureDistances;

	/**
	 * Optional dedicated map for editor-panel runs.
	 *
	 * Measuring inside whatever level happens to be open means the level's own content competes for
	 * the game thread, so the same system measures differently depending on the map. A bare test map
	 * removes that variable, which is what makes two runs on different days comparable.
	 *
	 * The panel never opens this on its own — swapping the level would discard unsaved work. It
	 * offers a button, prompts to save, and only then loads. Unset means "measure where I am".
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Stage")
	TSoftObjectPtr<UWorld> TestMap;

	/**
	 * Spawns transient lights, sky, fog and a floor so lit and translucent renderers are not
	 * measured against an empty void.
	 *
	 * Always applied to the commandlet's private world. In the editor it is applied only when the
	 * open level IS TestMap, on the assumption that a dedicated test map is bare and the level you
	 * work in already has its own lighting — adding a second sun to that would be a surprise.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Stage")
	bool bSpawnEnvironmentRig = true;
};

/**
 * Cost thresholds, expressed against the project's frame budget rather than in raw milliseconds so
 * they survive a hardware change. Raw milliseconds still reach the CSV; only the pass/fail
 * comparison is normalised.
 */
USTRUCT()
struct FPulseNiagaraCostBudget
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, Category = "Budget")
	EPulsePerformanceTarget PerformanceTarget = EPulsePerformanceTarget::FPS60;

	/** Read only when PerformanceTarget is Custom. */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0.1))
	float CustomFrameBudgetMs = 16.67f;

	/** Flags a system whose single-instance game-thread cost exceeds this share of the frame. */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0.0, ClampMax = 100.0))
	float MaxFrameBudgetPercentPerInstance = 2.0f;

	/** Flags a system whose whole measured population exceeds this share of the frame. */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0.0, ClampMax = 100.0))
	float MaxFrameBudgetPercentTotal = 10.0f;

	/**
	 * A system whose cost at the lowest measured quality is within this percentage of its cost at
	 * the highest has scalability that is not doing anything. Only meaningful with two or more
	 * quality levels measured.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0.0, ClampMax = 100.0))
	float FlatScalabilityTolerancePercent = 10.0f;
};

/**
 * Project configuration for the Pulse Niagara auditor, stored in Config/DefaultPulse.ini under
 * [/Script/Pulse.PulseNiagaraAuditorSettings].
 *
 * NOTE deliberately a separate UDeveloperSettings from UPulseSettings rather than another member of
 * it. UPulseSettings::ComputeSettingsHash() reflects over every top-level property, and that hash is
 * the audit report's provenance stamp. Folding measurement knobs in would make raising the spawn
 * count change the hash on a static audit those knobs cannot possibly affect.
 */
UCLASS(Config = Pulse, DefaultConfig, meta = (DisplayName = "Pulse - Niagara Auditor"))
class PULSE_API UPulseNiagaraAuditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPulseNiagaraAuditorSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	FPulseNiagaraAuditorScanSettings Scan;

	UPROPERTY(EditAnywhere, Config, Category = "Measurement")
	FPulseNiagaraMeasurementSettings Measurement;

	UPROPERTY(EditAnywhere, Config, Category = "Budget")
	FPulseNiagaraCostBudget Budget;

	/** Frame budget in milliseconds, resolving Custom against CustomFrameBudgetMs. */
	float GetFrameBudgetMs() const;
};
