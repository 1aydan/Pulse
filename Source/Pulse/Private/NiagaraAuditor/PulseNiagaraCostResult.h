// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseNiagaraAuditorSettings.h"

/** Distance recorded on a sample taken without a distance axis, i.e. every commandlet sample. */
static constexpr float GPulseNiagaraNoDistance = -1.0f;

/**
 * One system measured at one quality level and one distance.
 *
 * Absent stays absent. A measurement that did not happen leaves bMeasured false and fills
 * SkipReason; it never reports a zero cost, because a zero in a cost column reads as "free"
 * and is the single most dangerous thing this tool could emit.
 */
struct FPulseNiagaraCostSample
{
	int32 QualityLevel = INDEX_NONE;

	/** Centimetres from the viewport camera, or GPulseNiagaraNoDistance when culling was paused. */
	float Distance = GPulseNiagaraNoDistance;

	/** True only when a full measurement window completed. */
	bool bMeasured = false;

	/** Why this sample has no numbers. Empty when bMeasured. Reaches the CSV verbatim. */
	FString SkipReason;

	/** Instances actually registered and activated. */
	int32 InstancesSpawned = 0;

	/**
	 * Instances still active when the window closed. Below InstancesSpawned means the system either
	 * finished or was culled — which at a far distance is the intended result, not a failure.
	 */
	int32 InstancesActiveAtEnd = 0;

	/**
	 * Game-thread cost of the whole spawned population, in MILLISECONDS per frame — the same unit
	 * as the frame budget, so a number here can be compared against it without arithmetic.
	 *
	 * Derived from a per-frame series rather than the engine's running average: min is not available
	 * any other way, and having the series makes avg and max consistent with it. Avg is over every
	 * frame in the window; min and max are over frames that did work. See IdleFrames.
	 */
	double TotalAvgMs = 0.0;
	double TotalMinMs = 0.0;
	double TotalMaxMs = 0.0;

	/** The same series divided by InstancesSpawned. Reported alongside, never instead. */
	double PerInstanceAvgMs = 0.0;
	double PerInstanceMinMs = 0.0;
	double PerInstanceMaxMs = 0.0;

	/** Frames that contributed to the series. Below MeasureFrames means the timeout fired. */
	int32 FramesRecorded = 0;

	/**
	 * Frames in the window where the system booked no cycles. Niagara's concurrent tick work is
	 * attributed on the frame the async task completes, so a system ticking every frame still shows
	 * some idle ones. The window total is unaffected, so the average holds; a HIGH count means the
	 * spread between min and max is a sampling artefact and only the average should be trusted.
	 */
	int32 IdleFrames = 0;
};

/** One Niagara system, with a sample per (quality level, distance) pair measured. */
struct FPulseNiagaraCostRow
{
	FString AssetPath;
	FString AssetName;

	/** From the registry tag, so it is known before the package is loaded. */
	bool bHasGPUEmitter = false;

	/** From the registry tag. INDEX_NONE when the tag is absent (system saved before fully loaded). */
	int32 NumEmitters = INDEX_NONE;

	/** Effect Type name, or "None". Empty when the tag is absent. */
	FString EffectType;

	TArray<FPulseNiagaraCostSample> Samples;

	/** Finds one sample, or null. Linear: Samples holds one entry per axis combination. */
	const FPulseNiagaraCostSample* FindSample(int32 QualityLevel, float Distance) const
	{
		return Samples.FindByPredicate([QualityLevel, Distance](const FPulseNiagaraCostSample& Sample)
		{
			return Sample.QualityLevel == QualityLevel && FMath::IsNearlyEqual(Sample.Distance, Distance);
		});
	}
};

/** Everything one auditor run produced, plus the context needed to read it a month later. */
struct FPulseNiagaraCostReport
{
	FString GeneratedAtUtc;
	FString EngineVersion;
	FString MachineName;
	FString RHIName;

	/** Quality levels measured, in the order they were run. */
	TArray<int32> QualityLevels;

	/** Display names from UNiagaraSettings::QualityLevels, parallel to QualityLevels. */
	TArray<FString> QualityLevelNames;

	/**
	 * Distances measured, in run order. Exactly one entry, GPulseNiagaraNoDistance, for a
	 * commandlet run — see bScalabilityCullingExercised.
	 */
	TArray<float> Distances;

	/**
	 * False when distance culling was paused for the run, which it always is outside the editor.
	 * Recorded as data rather than prose because it decides whether these costs are comparable
	 * against an in-game capture at all.
	 */
	bool bScalabilityCullingExercised = false;

	/**
	 * False when the commandlet ran without -AllowCommandletRendering. Recorded because it changes
	 * what the numbers mean, and a reader who does not know it will over-trust them.
	 */
	bool bRenderingAvailable = false;

	/** True for a panel or console run, which measures inside a PIE session with a spectator pawn. */
	bool bMeasuredInPlayWorld = false;

	/** Echoed so a CSV is never separated from the parameters that produced it. */
	FPulseNiagaraMeasurementSettings Measurement;

	/** Frame budget the percentage columns are computed against. */
	float FrameBudgetMs = 0.0f;

	TArray<FPulseNiagaraCostRow> Rows;

	int32 NumSystemsDiscovered = 0;
	int32 NumSystemsMeasured = 0;
	int32 NumSystemsSkipped = 0;
};
