// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseCollector.h"

/**
 * Audits UMaterialInterface assets — masters and instances alike.
 *
 * Fast tier reads the registry tags each family actually carries (masters: domain, blend mode,
 * shading model; instances: parent path). Deep resolves the effective blend mode, two-sided flag,
 * and static switch count through the parent chain. ShaderStats reads instruction and sampler
 * counts from the compiled shader map via UMaterialEditingLibrary::GetStatistics.
 *
 * Usage flags (bUsedWithX) are deliberately out of scope in v1: the UPROPERTYs are deprecated in
 * 5.8 and the UMaterialInterface-level GetUsageByFlag accessor does not exist in 5.7, so there is
 * no way to read them that compiles clean against both engines without version guards.
 */
class FPulseMaterialCollector : public IPulseCollector
{
public:
	virtual FName GetCollectorName() const override;
	virtual FName GetCategory() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual bool SupportsSubclasses() const override;
	virtual EPulseTier GetMaxUsefulTier() const override;
	virtual EPulseLoadMode GetDeepLoadMode() const override;
	virtual void GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const override;
	virtual void GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const override;
	virtual void CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
	virtual void CollectDeep(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
	virtual void CollectShaderStats(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
	virtual void Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
};
