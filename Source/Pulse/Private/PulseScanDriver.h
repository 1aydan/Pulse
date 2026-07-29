// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "PulseResult.h"
#include "PulseRunConfig.h"

class FPulseShaderStatsBatch;
class IPulseCollector;
class UPulseSettings;

/** Progress snapshot for UI and log display. */
struct FPulseScanProgress
{
	int32 AssetsProcessed = 0;
	int32 AssetsTotal = 0;
	FName CurrentCategory;
	FString CurrentAssetPath;
};

/**
 * The audit engine, shared by the commandlet and the editor panel. Step-based on purpose: Step()
 * processes assets until its wall-clock budget expires, so the editor drives it from a ticker at
 * frame budget while the commandlet loops it with an unbounded budget. A blocking loop here would
 * hang the editor and could never be retrofitted.
 *
 * The public surface below is frozen — the commandlet and SPulseAuditPanel both depend on it.
 */
class FPulseScanDriver
{
public:
	FPulseScanDriver(const FPulseRunConfig& InConfig, const UPulseSettings& InSettings);
	~FPulseScanDriver();

	/**
	 * Enumerates assets per collector (deterministically sorted), builds the work list, and
	 * prepares shader-stats batches when the tier calls for them. Returns false with a reason
	 * when no collector matched the category filter. An empty asset list is NOT a failure.
	 */
	bool Initialize(FString& OutError);

	/**
	 * Processes assets for up to TimeBudgetSeconds of wall clock; unbounded when <= 0. Returns
	 * true while work remains. Owns package loading and the GC cadence — collectors never load.
	 */
	bool Step(double TimeBudgetSeconds);

	/** Abandons remaining work. The partial report can still be finalized and inspected. */
	void Cancel();

	bool IsComplete() const;

	FPulseScanProgress GetProgress() const;

	/**
	 * Sorts every array (fixing float accumulation order), scores assets, categories, and the
	 * project, and stamps the run header. Call once, after the last Step. Idempotent thereafter.
	 */
	FPulseReport& FinalizeReport();

	const FPulseReport& GetReport() const { return Report; }

private:
	/** One collector's slice of the run. */
	struct FPulseCollectorWork
	{
		IPulseCollector* Collector = nullptr;

		/** The tier this collector actually runs at: min(run tier, collector max useful tier). */
		EPulseTier EffectiveTier = EPulseTier::Fast;

		/** Sorted by object path string. */
		TArray<FAssetData> Assets;

		int32 NextAssetIndex = 0;
	};

	void ProcessOneAsset(FPulseCollectorWork& Work);
	void MaybeCollectGarbage(bool bForce);
	FPulseCategoryResult& FindOrAddCategory(FName Category);
	void BuildRunHeader();

	FPulseRunConfig Config;
	const UPulseSettings* Settings = nullptr;

	TArray<FPulseCollectorWork> WorkQueue;
	int32 CurrentWorkIndex = 0;

	int32 AssetsProcessed = 0;
	int32 AssetsTotal = 0;

	/** Deep-tier bookkeeping for the three-trigger GC cadence. */
	int32 PackagesTouchedSinceGC = 0;
	bool bLastPackageWasMap = false;

	TUniquePtr<FPulseShaderStatsBatch> ShaderStatsBatch;

	FPulseReport Report;

	bool bInitialized = false;
	bool bCancelled = false;
	bool bFinalized = false;
};
