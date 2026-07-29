// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

struct FAssetData;

/**
 * Batched material shader pre-warm for the ShaderStats tier. This is the ONLY file in Pulse
 * allowed to touch material compilation internals, and its review rule is exported symbols only:
 * FMaterialStatsUtils::GetRepresentativeInstructionCounts and ExtractMatertialStatsInfo carry no
 * DLL export and referencing either is a link error.
 *
 * Why this exists: UMaterialEditingLibrary::GetStatistics blocks on FinishCompilation one
 * material at a time — potentially hours on a cold DDC. Pre-warming a whole slice submits every
 * compile job up front and waits ONCE across the shader-compiler worker pool, so the per-material
 * GetStatistics calls the material collector makes afterwards hit complete shader maps and return
 * immediately.
 */
class FPulseShaderStatsBatch
{
public:
	/**
	 * Validates the RHI up front: with no usable shader platform, every pre-warm is skipped and
	 * GetUnavailableReason() explains why. Zeros from an unmeasurable platform must never be
	 * reported as measurements. TotalSlices is display-only, for the per-slice progress log.
	 */
	FPulseShaderStatsBatch(int32 InBatchSize, int32 InTotalSlices);

	bool IsAvailable() const
	{
		return UnavailableReason.IsEmpty();
	}

	const FString& GetUnavailableReason() const
	{
		return UnavailableReason;
	}

	int32 GetBatchSize() const
	{
		return BatchSize;
	}

	/**
	 * Loads every material in the slice, submits compile jobs for each incomplete game-thread
	 * shader map, then waits once for the whole batch. Call just before processing the slice's
	 * assets, so the materials are still loaded when their Collect runs. A GC between slices is
	 * harmless — a re-warmed material becomes a DDC fetch, not a recompile.
	 */
	void PrewarmSlice(TArrayView<const FAssetData> Slice);

private:
	int32 BatchSize = 64;

	/** Display only, for the "batch i/N" progress line. */
	int32 TotalSlices = 0;
	int32 SlicesPrewarmed = 0;

	/** Non-empty means no pre-warm will run and the run header should say why. */
	FString UnavailableReason;
};
