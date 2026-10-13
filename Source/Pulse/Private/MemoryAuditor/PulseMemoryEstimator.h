// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseMemoryResult.h"

struct FAssetData;
class FAssetPackageData;

/** One package's estimated cost and how much to trust it. */
struct FPulseMemoryEstimate
{
	FPulseMemoryCost Cost;
	EPulseMemorySizeSource Source = EPulseMemorySizeSource::Unknown;
	FString Note;
};

/**
 * Estimates what a package costs once loaded, from Asset Registry metadata alone — no package is
 * ever loaded. The classes that dominate memory have tag-driven estimators:
 *
 *   Texture2D     format + dimensions + mip settings, split into resident and streaming mips
 *   StaticMesh    vertex and index buffers from vertex/triangle counts, Nanite pages as streaming
 *   SkeletalMesh  the same plus skin weights
 *   AnimSequence  the engine's own compressed-size tag
 *   SoundWave     duration x rate x channels over a configured compression ratio, as streaming
 *
 * Everything else falls back to the package's size on disk, bucketed by class, and says so. These
 * are estimates for ranking, not budgeting: they ignore device-profile LOD settings, platform
 * format differences and shader maps, and the Source field travels with every number so nobody
 * mistakes one for a measurement.
 */
class FPulseMemoryEstimator
{
public:
	explicit FPulseMemoryEstimator(const FPulseMemoryEstimationSettings& InSettings);

	/** Estimates one package. Assets is every registry entry in it, and may be empty. */
	FPulseMemoryEstimate EstimatePackage(const TArray<FAssetData>& Assets, const FAssetPackageData* PackageData) const;

	/**
	 * The asset a package is named for — the one whose name matches the package's — or the first
	 * entry when none does. Null for an empty list.
	 */
	static const FAssetData* PickMainAsset(const TArray<FAssetData>& Assets);

private:
	bool EstimateTexture2D(const FAssetData& Asset, FPulseMemoryEstimate& OutEstimate) const;
	bool EstimateStaticMesh(const FAssetData& Asset, FPulseMemoryEstimate& OutEstimate) const;
	bool EstimateSkeletalMesh(const FAssetData& Asset, FPulseMemoryEstimate& OutEstimate) const;
	bool EstimateAnimSequence(const FAssetData& Asset, FPulseMemoryEstimate& OutEstimate) const;
	bool EstimateSoundWave(const FAssetData& Asset, FPulseMemoryEstimate& OutEstimate) const;

	/** Disk size as the stand-in, in the bucket the asset's class would occupy at runtime. */
	void EstimateFromDiskSize(const FAssetData* MainAsset, const FAssetPackageData* PackageData, FPulseMemoryEstimate& OutEstimate) const;

	// By value: callers build settings as locals and the estimator outlives none of them, but a copy
	// costs nothing and removes the question.
	const FPulseMemoryEstimationSettings Settings;
};

/** "12.4 MB", "1.06 GB", "640 KB", "312 B". Shared by the panel and log lines; reports keep raw bytes. */
FString PulseFormatBytes(int64 Bytes);
