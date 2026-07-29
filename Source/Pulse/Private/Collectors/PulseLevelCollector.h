// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseCollector.h"

/**
 * Audits maps (category "Level") without stepping on the World Partition trap: a WP map keeps its
 * actors in external packages, so PersistentLevel->Actors is EMPTY and a naive walk confidently
 * reports ~0 actors for exactly the biggest maps in a project. Two paths instead:
 *
 *  - Fast: partition/streaming tags, plus — for partitioned maps — a full actor census counting
 *    external-actor descriptors in the Asset Registry. Zero package loads.
 *  - Deep: legacy (non-partitioned) maps only, via a self-managed LoadWorldPackageForEditor +
 *    FScopedEditorWorld load and a PersistentLevel->Actors walk. The only SelfManaged collector,
 *    because a world cannot be brought up with a bare FAssetData::GetAsset().
 *
 * The ActorCountSource metric records which path produced the number so the two are never confused.
 */
class FPulseLevelCollector : public IPulseCollector
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
	virtual void Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const override;
};
