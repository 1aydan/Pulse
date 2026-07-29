// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseAssetEnumerator.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "PulseCollector.h"
#include "PulseRunConfig.h"
#include "PulseSettings.h"

void FPulseAssetEnumerator::EnumerateForCollector(
	const IAssetRegistry& AssetRegistry,
	const IPulseCollector& Collector,
	const FPulseRunConfig& Config,
	const UPulseSettings& Settings,
	TArray<FAssetData>& OutAssets,
	bool& bOutTruncated)
{
	OutAssets.Reset();
	bOutTruncated = false;

	FARFilter Filter;
	Filter.ClassPaths.Add(Collector.GetSupportedClass()->GetClassPathName());
	Filter.bRecursiveClasses = Collector.SupportsSubclasses();
	Filter.bRecursivePaths = true;
	for (const FString& Path : Config.PathFilter)
	{
		Filter.PackagePaths.Add(FName(*Path));
	}

	AssetRegistry.GetAssets(Filter, OutAssets);

	if (!Settings.Scan.ExcludePathPatterns.IsEmpty())
	{
		OutAssets.RemoveAll([&Settings](const FAssetData& AssetData)
		{
			const FString PackageName = AssetData.PackageName.ToString();
			for (const FString& Pattern : Settings.Scan.ExcludePathPatterns)
			{
				if (PackageName.Contains(Pattern, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		});
	}

	// Case-SENSITIVE string compare for a total order. Case-insensitive compares can call two
	// paths differing only in case equal, and the tie would then be broken by the registry's
	// enumeration order — which is not stable between runs.
	OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString().Compare(B.GetSoftObjectPath().ToString(), ESearchCase::CaseSensitive) < 0;
	});

	// The cap is a prefix of the already-sorted list, so a truncated run is at least a
	// reproducible truncated run. bTruncated marks its scores as non-comparable.
	if (Config.MaxAssetsPerCategory > 0 && OutAssets.Num() > Config.MaxAssetsPerCategory)
	{
		OutAssets.SetNum(Config.MaxAssetsPerCategory);
		bOutTruncated = true;
	}
}
