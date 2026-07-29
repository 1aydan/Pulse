// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseCollector.h"

/**
 * Skeletal mesh audit. Every rule input here is an Asset Registry tag — bones, LODs, morph
 * targets, LOD0 triangles/vertices, Nanite flag, physics asset assignment — so this collector
 * reports GetMaxUsefulTier() == Fast and never causes a package load, even in a -deep run.
 */
class FPulseSkeletalMeshCollector : public IPulseCollector
{
public:
	// ---- Identity -------------------------------------------------------------------------------

	virtual FName GetCollectorName() const override;
	virtual FName GetCategory() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual bool SupportsSubclasses() const override;

	// ---- Tier capability ------------------------------------------------------------------------

	virtual EPulseTier GetMaxUsefulTier() const override;

	// ---- Schema ---------------------------------------------------------------------------------

	virtual void GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const override;
	virtual void GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const override;

	// ---- Work -----------------------------------------------------------------------------------

	virtual void CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
	virtual void Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
};
