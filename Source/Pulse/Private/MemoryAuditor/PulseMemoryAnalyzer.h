// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseMemoryAuditorConfig.h"
#include "PulseMemoryEstimator.h"
#include "PulseMemoryResult.h"

class IAssetRegistry;

/**
 * Builds the memory report: finds the top-level assets, walks their dependencies through the Asset
 * Registry, sizes every package reached, and computes each root's total, unique and may-load cost.
 * Registry-only — nothing is loaded — so a whole project takes seconds and the result is the same on
 * every run over unchanged content.
 *
 * One synchronous Run() serves both front ends. Unlike the Niagara auditor there is no frame-locked
 * work to spread out, so the panel wraps the call in a slow-task dialog instead of a ticker.
 */
class FPulseMemoryAnalyzer
{
public:
	/** Stage label and 0..1 progress. Returning false cancels the run. */
	using FProgressCallback = TFunction<bool(const FText& Stage, float Fraction)>;

	explicit FPulseMemoryAnalyzer(const FPulseMemoryAuditorConfig& InConfig);

	/** False fills OutError: no registry, no roots found, or cancelled. */
	bool Run(FPulseMemoryReport& OutReport, FString& OutError, const FProgressCallback& Progress = FProgressCallback());

private:
	struct FRootCandidate
	{
		FName PackageName;
		EPulseMemoryRootKind Kind = EPulseMemoryRootKind::PrimaryAsset;
		FString Detail;
	};

	/** Every top-level candidate, deduplicated by package with the kind precedence applied. */
	void GatherRoots(const IAssetRegistry& Registry, TArray<FRootCandidate>& OutRoots) const;

	/** True when PackageName sits under a scan path and matches no exclusion. */
	bool IsInScanScope(FName PackageName) const;

	/** Returns the node for PackageName, creating and sizing it on first sight. INDEX_NONE when the registry has no such package. */
	int32 FindOrAddNode(const IAssetRegistry& Registry, FName PackageName, FPulseMemoryReport& Report, TArray<int32>& Queue) const;

	/** Fills one node's hard and soft dependency lists, creating nodes for anything new. */
	void ExpandNode(const IAssetRegistry& Registry, int32 NodeIndex, FPulseMemoryReport& Report, TArray<int32>& Queue) const;

	/** Retained cost per node, from the dominator tree of the hard graph under a virtual root. */
	static void ComputeRetained(FPulseMemoryReport& Report);

	const FPulseMemoryAuditorConfig Config;
	const FPulseMemoryEstimator Estimator;
};
