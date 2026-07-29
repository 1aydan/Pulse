// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseCollector.h"

/**
 * Texture audit. Fast tier reads registry tags only — crucially, the Dimensions tag comes from
 * GetImportedSize(), so the Fast metrics are named SourceWidth/SourceHeight and every size rule
 * is explicit that it judged the SOURCE, not the cooked result. Deep tier loads the texture for
 * the numbers the registry cannot provide: real memory (all mips), cooked dimensions, mip count.
 */
class FPulseTextureCollector : public IPulseCollector
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

	/** Registry tags: source dimensions, format/compression/LODGroup text, streaming flags. */
	virtual void CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;

	/** Loaded UTexture2D: memory (TMC_AllMips), cooked width/height, mip count. */
	virtual void CollectDeep(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;

	virtual void Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
};
