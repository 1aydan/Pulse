// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseCollector.h"

/**
 * Audits UNiagaraSystem assets. Fast tier reads the seven registry tags the system emits under
 * WITH_EDITOR (emitter/renderer counts, GPU bounds, effect type, warmup); Deep tier loads the
 * system for what tags cannot say: pool capacity on the UFXSystemAsset base and the per-emitter
 * CPU/GPU/stateless sim-target split from the emitter handles.
 *
 * Pooling caveat: the engine has NO bPoolingEnabled — pooling is chosen per spawn call via
 * ENCPoolMethod. The only auditable asset-side fact is whether MaxPoolSize/PoolPrimeSize allow the
 * pool to retain instances at all, so the NotPoolable rule says "cannot be pooled", never
 * "pooling is disabled".
 */
class FPulseNiagaraCollector : public IPulseCollector
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
	virtual void CollectDeep(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
	virtual void Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
};
