// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseCollector.h"

/**
 * Audits Blueprint assets for the classic Blueprint performance sins: ticking by default, component
 * bloat, and replicated-property sprawl.
 *
 * Fast tier reads the FBlueprintTags registry tags (parent chain, component counts, replicated
 * properties, data-only flag). Deep tier inspects the generated class's CDO for actor tick settings
 * — the single biggest Blueprint perf lever there is.
 *
 * Claims EXACT UBlueprint only (SupportsSubclasses false): the NativeComponents and
 * BlueprintComponents registry tags are emitted only by UBlueprint itself, not by Anim/Widget
 * Blueprint subclasses, so restricting the registry filter to the exact class is what keeps the
 * component rule honest instead of miscounting every animation Blueprint as zero-component.
 */
class FPulseBlueprintCollector : public IPulseCollector
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
	virtual void CollectDeep(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
	virtual void Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
};
