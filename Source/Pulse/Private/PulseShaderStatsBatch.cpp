// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseShaderStatsBatch.h"

#include "AssetRegistry/AssetData.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "Pulse.h"
#include "RHIShaderPlatform.h"
#include "RenderingThread.h"
#include "ShaderCore.h"

FPulseShaderStatsBatch::FPulseShaderStatsBatch(int32 InBatchSize, int32 InTotalSlices)
	: BatchSize(FMath::Max(1, InBatchSize))
	, TotalSlices(InTotalSlices)
{
	// NullRHI does set a real platform, so headless -nullrhi runs measure normally. This guard is
	// for the case where it somehow did not: pre-warming against an invalid platform would make
	// every GetStatistics return zeros, and zeros must never be reported as measurements.
	if (GMaxRHIShaderPlatform >= SP_NumPlatforms)
	{
		UnavailableReason = TEXT("GMaxRHIShaderPlatform is not a valid shader platform; material shader statistics cannot be measured in this session.");
		UE_LOG(LogPulse, Warning, TEXT("Pulse shader stats unavailable: %s"), *UnavailableReason);
	}
}

void FPulseShaderStatsBatch::PrewarmSlice(TArrayView<const FAssetData> Slice)
{
	if (!IsAvailable() || Slice.IsEmpty())
	{
		return;
	}

	++SlicesPrewarmed;
	UE_LOG(LogPulse, Display, TEXT("Pulse shader stats: batch %d/%d (%d materials)"), SlicesPrewarmed, TotalSlices, Slice.Num());

	// Mirrors UMaterialEditingLibrary::GetStatistics' per-material warm-up path, but collects the
	// incomplete resources and waits once for all of them instead of blocking per material.
	TArray<FMaterial*> MaterialsToFinish;
	for (const FAssetData& AssetData : Slice)
	{
		UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(AssetData.GetAsset());
		if (!MaterialInterface)
		{
			UE_LOG(LogPulse, Warning, TEXT("Pulse shader stats: failed to load material %s; it will not be pre-warmed."), *AssetData.GetSoftObjectPath().ToString());
			continue;
		}

		FMaterialResource* Resource = MaterialInterface->GetMaterialResource(GMaxRHIShaderPlatform);
		if (Resource == nullptr)
		{
			// Instances without a static permutation have no resource of their own; their parent
			// material's slice entry covers them.
			continue;
		}

		if (!Resource->IsGameThreadShaderMapComplete())
		{
			Resource->SubmitCompileJobs_GameThread(EShaderCompileJobPriority::High);
			MaterialsToFinish.Add(Resource);
		}
	}

	if (!MaterialsToFinish.IsEmpty())
	{
		// ONE blocking wait for the whole slice: every submitted job runs across the shader
		// compiler worker pool concurrently, which is the entire point of batching.
		FMaterial::FinishCompilation(TEXT("PulseShaderStatsBatch"), MaterialsToFinish);
	}

	// Release RHI resources and pending cleanup objects (shader maps) accumulated by the batch
	// before the driver starts collecting, so slice memory does not stack across the run.
	FlushRenderingCommands();
}
