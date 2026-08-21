// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "PulseNiagaraAuditorConfig.h"
#include "PulseNiagaraCostResult.h"
#include "PulseNiagaraCostStage.h"
#include "Templates/UniquePtr.h"
#include "UObject/StrongObjectPtr.h"

class UNiagaraSystem;
class UPulseNiagaraAuditorSettings;

/** Live progress for the panel's status line and bar. Cheap enough to poll every frame. */
struct FPulseNiagaraAuditProgress
{
	int32 SamplesCompleted = 0;
	int32 SamplesTotal = 0;
	FString CurrentSystemName;
	FString CurrentQualityName;
	float CurrentDistance = GPulseNiagaraNoDistance;

	/** True while a measurement window is open, i.e. the live figures below mean something. */
	bool bMeasuring = false;

	/** Running average of the in-flight window, in milliseconds. */
	double LiveAvgMs = 0.0;

	int32 LiveFramesRecorded = 0;
	int32 LiveInstanceCount = 0;

	/** Frames the window will record in total, so the overlay can show a per-system bar. */
	int32 MeasureFrames = 0;
};

/**
 * Runs a Niagara cost audit in steps, so one implementation serves both front ends — exactly the
 * split FPulseScanDriver makes for the asset audit. The commandlet calls Step(0.0) until it returns
 * false; the editor panel calls Step(budget) from a ticker so the editor stays responsive.
 *
 * The step granularity is not arbitrary. In PIE a measurement sample is only valid
 * once per rendered frame, so a Step there advances at most one measurement frame no matter how
 * much budget is left; in PrivateWorld mode the driver owns the frames and drains as fast as it can.
 */
class FPulseNiagaraAuditDriver
{
public:
	FPulseNiagaraAuditDriver(const FPulseNiagaraAuditorConfig& InConfig, const UPulseNiagaraAuditorSettings& InSettings,
		EPulseNiagaraStageMode InMode);
	~FPulseNiagaraAuditDriver();

	/** Enumerates systems and stands up the stage. False fills OutError and leaves nothing running. */
	bool Initialize(FString& OutError);

	/** Advances the run. Returns true while work remains. A budget of 0 means drain everything. */
	bool Step(double TimeBudgetSeconds);

	/** Stops at the next step boundary. The partial report stays valid and finalizable. */
	void Cancel();

	bool IsComplete() const;

	/** Tears down the stage and returns the report. Safe on a cancelled or partial run. */
	const FPulseNiagaraCostReport& FinalizeReport();

	FPulseNiagaraAuditProgress GetProgress() const;

	/** Set when a step failed fatally, e.g. a quality level that would not apply. */
	const FString& GetError() const { return FatalError; }

private:
	/** One (quality level, distance) pair. The full axis product, precomputed in run order. */
	struct FPulseNiagaraAxisPoint
	{
		int32 QualityLevel = INDEX_NONE;
		float Distance = GPulseNiagaraNoDistance;
	};

	/** Advances by one unit of work. Returns true when that unit was a measurement frame. */
	bool AdvanceOnce();

	/** Moves to the next asset, loading it and recording any skip. False when none remain. */
	bool BeginNextSystem();

	/** Starts a PIE session for PIEWorld runs. Asynchronous: the world appears a tick or two later. */
	void RequestPlaySession();

	/** Creates the stage once its world exists. Returns false while still waiting for PIE. */
	bool TryCreateStage();

	void FinishCurrentSample();

	// By value, not by reference. The editor panel builds its config as a stack local inside the
	// Run handler and the driver outlives that scope by minutes; the stage in turn holds a reference
	// into Config.Measurement, which is valid precisely because this copy lives as long as the stage.
	const FPulseNiagaraAuditorConfig Config;

	/** Safe as a reference: this is the CDO, which outlives every driver. */
	const UPulseNiagaraAuditorSettings& Settings;

	TUniquePtr<FPulseNiagaraCostStage> Stage;

	TArray<FAssetData> Assets;
	TArray<FPulseNiagaraAxisPoint> AxisPoints;

	/** Strong, because a measurement spans many frames and a GC in between must not free it. */
	TStrongObjectPtr<UNiagaraSystem> CurrentSystem;

	FPulseNiagaraCostReport Report;
	FPulseNiagaraCostRow CurrentRow;
	FString FatalError;

	int32 CurrentAssetIndex = INDEX_NONE;
	int32 CurrentAxisIndex = 0;
	int32 SamplesCompleted = 0;
	int32 SystemsSinceCollect = 0;
	bool bInitialized = false;
	bool bStageCreated = false;
	bool bPlaySessionRequested = false;
	double PlaySessionWaitStartSeconds = 0.0;
	bool bCancelled = false;
	bool bFinalized = false;
	bool bRowMeasuredAnySample = false;
};
