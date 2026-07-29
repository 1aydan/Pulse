// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseCollector.h"

/**
 * Static mesh audit. Fast tier only, and deliberately so: every number the rules need — triangle
 * and vertex counts, LODs, material slots, UV channels, Nanite state, collision setup, compressed
 * and distance field sizes — is already a registry tag written by UStaticMesh::GetAssetRegistryTags.
 * This collector therefore NEVER causes a package load, even in a -deep run; the driver sees
 * GetMaxUsefulTier() == Fast and skips loading entirely. That is the headline design point of the
 * whole plugin: the richest category is also the cheapest to audit.
 */
class FPulseStaticMeshCollector : public IPulseCollector
{
public:
	virtual FName GetCollectorName() const override;
	virtual FName GetCategory() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual bool SupportsSubclasses() const override;
	virtual EPulseTier GetMaxUsefulTier() const override;
	virtual void GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const override;
	virtual void GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const override;
	virtual void CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
	virtual void Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
};
