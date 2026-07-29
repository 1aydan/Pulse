// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"

class IAssetRegistry;
class IPulseCollector;
class UPulseSettings;
struct FAssetData;
struct FPulseRunConfig;

/**
 * Builds one collector's work list from the Asset Registry: one FARFilter per collector, the
 * settings path exclusions, a deterministic sort, and the -maxassets cap. Enumeration never loads
 * a package — everything here is registry metadata.
 */
class FPulseAssetEnumerator
{
public:
	/**
	 * Fills OutAssets with every asset the collector claims, sorted lexically by object path
	 * string (case-sensitive — FName comparison order is name-table index order and is not
	 * reproducible between processes). Sets bOutTruncated when Config.MaxAssetsPerCategory
	 * clipped the sorted list; callers OR this into the run header's bTruncated.
	 */
	static void EnumerateForCollector(
		const IAssetRegistry& AssetRegistry,
		const IPulseCollector& Collector,
		const FPulseRunConfig& Config,
		const UPulseSettings& Settings,
		TArray<FAssetData>& OutAssets,
		bool& bOutTruncated);
};
