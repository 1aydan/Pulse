// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "PulseTypes.h"
#include "PulseTickAuditorSettings.generated.h"

/**
 * Project configuration for the Pulse tick auditor, stored in Config/DefaultPulse.ini under
 * [/Script/Pulse.PulseTickAuditorSettings].
 *
 * The tick auditor measures a live Play In Editor session — whatever is in the level and whatever the
 * player is doing — rather than spawning Blueprints into an empty stage, so these settings shape the
 * capture window and the thresholds, not what gets measured.
 */
UCLASS(Config = Pulse, DefaultConfig, meta = (DisplayName = "Pulse - Tick Auditor"))
class PULSE_API UPulseTickAuditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/**
	 * Frames captured. Long enough to cover what the game does in a typical stretch of play — combat,
	 * AI decisions, timers — but every frame of it runs with Pulse driving the ticks, so keep it short.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Capture", meta = (ClampMin = 1, ClampMax = 36000))
	int32 CaptureFrames = 300;

	UPROPERTY(EditAnywhere, Config, Category = "Budget")
	EPulsePerformanceTarget PerformanceTarget = EPulsePerformanceTarget::FPS60;

	/** Read only when PerformanceTarget is Custom. */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0.1))
	float CustomFrameBudgetMs = 16.67f;

	/** Flags a class whose instances together exceed this share of the frame. */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0.0, ClampMax = 100.0))
	float MaxFrameBudgetPercentPerClass = 2.0f;

	/** Flags a class whose single instance exceeds this share of the frame. */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0.0, ClampMax = 100.0))
	float MaxFrameBudgetPercentPerInstance = 0.5f;

	/**
	 * An actor tick that costs less than this per call, in microseconds, and implements no Event Tick,
	 * is reported as idle: the engine still schedules it every frame for nothing, so turning it off is
	 * free.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Budget", meta = (ClampMin = 0.0))
	float IdleTickMicroseconds = 1.0f;

	/** Relative paths resolve against the project directory. */
	UPROPERTY(EditAnywhere, Config, Category = "Report")
	FString ReportDirectory = TEXT("Saved/Pulse/TickAuditor");

	UPROPERTY(EditAnywhere, Config, Category = "Report", meta = (ClampMin = 0))
	int32 MaxHistoricalRuns = 20;

	float GetFrameBudgetMs() const;
};
