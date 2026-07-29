// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseNiagaraCollector.h"

#include "AssetRegistry/AssetData.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"
#include "PulseCollectorRegistry.h"
#include "PulseSettings.h"
#include "PulseTagUtils.h"

// Verified against UE 5.7.4 and 5.8.0 NiagaraSystem.cpp (NiagaraSystemPrivate, top of file): the
// tag names below are declared identically in both. All of them are emitted together under
// WITH_EDITOR by UNiagaraSystem's registry-tag path, so a system saved before it fully loaded has
// NONE of them — which is what the Niagara.TagsIncomplete rule reports.

/**
 * Single authority for rule ids, severities, tiers, and fix text. Evaluate fires issues by looking
 * rules up here, so a severity or tier can never disagree between the schema and an emitted issue.
 * Function-local static because FName construction must stay out of the pre-main window.
 */
static const TArray<FPulseRuleDesc>& PulseNiagaraRuleTable()
{
	static const TArray<FPulseRuleDesc> Rules =
	{
		{
			FName(TEXT("Niagara.TagsIncomplete")), EPulseSeverity::Info, EPulseTier::Fast,
			TEXT("The NumEmitters registry tag is absent. Niagara emits its registry tags only when the system is fully loaded at save time, so this system's Fast metrics are missing rather than zero."),
			TEXT("Resave the Niagara system in the editor so its Asset Registry tags are rewritten; until then Fast-tier metrics for it are unavailable and its rules cannot be evaluated.")
		},
		{
			FName(TEXT("Niagara.ExcessiveEmitters")), EPulseSeverity::Medium, EPulseTier::Fast,
			TEXT("Emitter count exceeds MaxEmitters. Uses ActiveEmitters when the tag is present (scalability-aware), otherwise NumEmitters."),
			TEXT("Merge emitters that share materials and behavior, or split the effect into separate systems; every active emitter is its own simulation pass and renderer set per instance.")
		},
		{
			FName(TEXT("Niagara.ExcessiveRenderers")), EPulseSeverity::Low, EPulseTier::Fast,
			TEXT("ActiveRenderers exceeds MaxRenderers."),
			TEXT("Remove or merge renderers in the Niagara editor; each active renderer adds a draw-call family to every instance of the system.")
		},
		{
			FName(TEXT("Niagara.GPUSimMissingFixedBounds")), EPulseSeverity::High, EPulseTier::Fast,
			TEXT("One or more GPU emitters compute dynamic bounds. Reading bounds back from a GPU simulation stalls the pipeline."),
			TEXT("Set fixed bounds on the system or the flagged GPU emitters in the Niagara editor; dynamic bounds on a GPU sim force a GPU readback stall every frame the system is visible.")
		},
		{
			FName(TEXT("Niagara.MissingEffectType")), EPulseSeverity::Low, EPulseTier::Fast,
			TEXT("No Effect Type asset is assigned, so scalability, culling, and significance settings never apply to this system."),
			TEXT("Assign an Effect Type in the Niagara system's properties so scalability and significance culling manage it; systems without one never scale down on low-end devices.")
		},
		{
			FName(TEXT("Niagara.NotPoolable")), EPulseSeverity::Low, EPulseTier::Deep,
			TEXT("MaxPoolSize and PoolPrimeSize are both 0, so the world component pool can never retain an instance of this system. Pooling itself is chosen per spawn call via ENCPoolMethod — this rule is about capacity, not a toggle."),
			TEXT("Set MaxPoolSize (and optionally PoolPrimeSize) in the system's Performance settings so ENCPoolMethod spawn calls can reuse pooled instances instead of constructing a new component every spawn.")
		},
		{
			FName(TEXT("Niagara.ExcessiveWarmup")), EPulseSeverity::Medium, EPulseTier::Fast,
			TEXT("WarmupTime exceeds MaxWarmupTimeSeconds. Warmup simulates the whole system synchronously when it activates."),
			TEXT("Reduce WarmupTime or increase the warmup tick delta in the system's properties; long warmups run the full simulation on the game thread at activation and read as a hitch.")
		}
	};
	return Rules;
}

/** Fires one issue, taking Severity and DetectedAtTier from the rule table so they are authored once. */
static void PulseNiagaraAddIssue(FPulseAssetResult& InOutResult, FName RuleId, FString Message)
{
	for (const FPulseRuleDesc& Rule : PulseNiagaraRuleTable())
	{
		if (Rule.RuleId == RuleId)
		{
			FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
			Issue.RuleId = Rule.RuleId;
			Issue.Severity = Rule.DefaultSeverity;
			Issue.Message = MoveTemp(Message);
			Issue.DetectedAtTier = Rule.MinTier;
			return;
		}
	}
	checkf(false, TEXT("Niagara rule %s fired but is not in the schema table"), *RuleId.ToString());
}

FName FPulseNiagaraCollector::GetCollectorName() const
{
	return FName(TEXT("Pulse.Niagara"));
}

FName FPulseNiagaraCollector::GetCategory() const
{
	return FName(TEXT("Niagara"));
}

UClass* FPulseNiagaraCollector::GetSupportedClass() const
{
	return UNiagaraSystem::StaticClass();
}

bool FPulseNiagaraCollector::SupportsSubclasses() const
{
	return true;
}

EPulseTier FPulseNiagaraCollector::GetMaxUsefulTier() const
{
	return EPulseTier::Deep;
}

void FPulseNiagaraCollector::GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const
{
	OutMetrics.Append({
		{ FName(TEXT("NumEmitters")), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Fast,
			TEXT("Total emitter handles in the system, including disabled ones (registry tag).") },
		{ FName(TEXT("ActiveEmitters")), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Fast,
			TEXT("Emitters enabled and allowed by scalability at save time (registry tag).") },
		{ FName(TEXT("ActiveRenderers")), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Fast,
			TEXT("Enabled renderer properties across all active emitters (registry tag).") },
		{ FName(TEXT("HasGPUEmitter")), EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Fast,
			TEXT("True when any emitter simulates on the GPU (registry tag).") },
		{ FName(TEXT("GPUSimsMissingFixedBounds")), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Fast,
			TEXT("GPU emitters relying on dynamically computed bounds instead of fixed bounds (registry tag).") },
		{ FName(TEXT("EffectType")), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
			TEXT("Name of the assigned Effect Type asset, or None (registry tag).") },
		{ FName(TEXT("WarmupTime")), EPulseMetricKind::Real, EPulseMetricUnit::Seconds, EPulseTier::Fast,
			TEXT("Seconds of simulation run synchronously when the system activates (registry tag).") },
		{ FName(TEXT("MaxPoolSize")), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Deep,
			TEXT("World component pool capacity for this system (UFXSystemAsset::MaxPoolSize).") },
		{ FName(TEXT("PoolPrimeSize")), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Deep,
			TEXT("Instances pre-allocated at load to prime the component pool (UFXSystemAsset::PoolPrimeSize).") },
		{ FName(TEXT("IsPoolable")), EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Deep,
			TEXT("True when MaxPoolSize or PoolPrimeSize is non-zero, i.e. an ENCPoolMethod spawn can actually reuse an instance.") },
		{ FName(TEXT("CPUEmitters")), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Deep,
			TEXT("Enabled standard-mode emitters simulating on the CPU.") },
		{ FName(TEXT("GPUEmitters")), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Deep,
			TEXT("Enabled standard-mode emitters simulating on the GPU.") },
		{ FName(TEXT("StatelessEmitters")), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Deep,
			TEXT("Enabled lightweight (stateless-mode) emitter handles; these have no per-emitter sim target.") }
	});
}

void FPulseNiagaraCollector::GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const
{
	OutRules.Append(PulseNiagaraRuleTable());
}

void FPulseNiagaraCollector::CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	// Tag names match the metric names one-to-one. Each helper skips the metric and bumps
	// NumMissingExpectedTags when the tag is absent; Evaluate turns the NumEmitters gap into the
	// TagsIncomplete issue so incomplete tags are a visible finding, not just a trust counter.
	PulseAddIntTagMetric(InOutResult, Context.AssetData, FName(TEXT("NumEmitters")), FName(TEXT("NumEmitters")), EPulseMetricUnit::Count);
	PulseAddIntTagMetric(InOutResult, Context.AssetData, FName(TEXT("ActiveEmitters")), FName(TEXT("ActiveEmitters")), EPulseMetricUnit::Count);
	PulseAddIntTagMetric(InOutResult, Context.AssetData, FName(TEXT("ActiveRenderers")), FName(TEXT("ActiveRenderers")), EPulseMetricUnit::Count);
	PulseAddBoolTagMetric(InOutResult, Context.AssetData, FName(TEXT("HasGPUEmitter")), FName(TEXT("HasGPUEmitter")));
	PulseAddIntTagMetric(InOutResult, Context.AssetData, FName(TEXT("GPUSimsMissingFixedBounds")), FName(TEXT("GPUSimsMissingFixedBounds")), EPulseMetricUnit::Count);
	PulseAddTextTagMetric(InOutResult, Context.AssetData, FName(TEXT("EffectType")), FName(TEXT("EffectType")), /*bExpected*/ true);

	// WarmupTime is real-valued and there is no real-tag helper, so the absent-vs-zero discipline
	// is applied by hand: absent tag means no metric plus a trust-counter bump, never a zero.
	double WarmupTime = 0.0;
	if (PulseGetTagReal(Context.AssetData, FName(TEXT("WarmupTime")), WarmupTime))
	{
		InOutResult.Metrics.Add(FPulseMetric::MakeReal(FName(TEXT("WarmupTime")), WarmupTime, EPulseMetricUnit::Seconds, EPulseTier::Fast));
	}
	else
	{
		InOutResult.NumMissingExpectedTags++;
	}
}

void FPulseNiagaraCollector::CollectDeep(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const UNiagaraSystem* System = Cast<UNiagaraSystem>(Context.LoadedObject);
	if (System == nullptr)
	{
		return;
	}

	// Pool capacity lives on the UFXSystemAsset base (Particles/ParticleSystem.h). Both are plain
	// uint32 UPROPERTYs; IsPoolable is the derived fact the NotPoolable rule actually consumes.
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(TEXT("MaxPoolSize")), static_cast<int64>(System->MaxPoolSize), EPulseMetricUnit::Count, EPulseTier::Deep));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(TEXT("PoolPrimeSize")), static_cast<int64>(System->PoolPrimeSize), EPulseMetricUnit::Count, EPulseTier::Deep));
	const bool bIsPoolable = System->MaxPoolSize > 0 || System->PoolPrimeSize > 0;
	InOutResult.Metrics.Add(FPulseMetric::MakeBool(FName(TEXT("IsPoolable")), bIsPoolable, EPulseTier::Deep));

	int64 NumCPUEmitters = 0;
	int64 NumGPUEmitters = 0;
	int64 NumStatelessEmitters = 0;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (!Handle.GetIsEnabled())
		{
			continue;
		}
		if (Handle.GetEmitterMode() == ENiagaraEmitterMode::Stateless)
		{
			// Stateless emitters have no FVersionedNiagaraEmitterData and therefore no SimTarget;
			// they are their own bucket rather than a guess at CPU or GPU.
			NumStatelessEmitters++;
			continue;
		}
		// GetEmitterData() can be null even for a standard-mode handle (e.g. stripped or unresolved
		// versioned data). An unclassifiable emitter is counted nowhere rather than miscounted.
		if (const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
		{
			if (EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim)
			{
				NumGPUEmitters++;
			}
			else
			{
				NumCPUEmitters++;
			}
		}
	}

	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(TEXT("CPUEmitters")), NumCPUEmitters, EPulseMetricUnit::Count, EPulseTier::Deep));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(TEXT("GPUEmitters")), NumGPUEmitters, EPulseMetricUnit::Count, EPulseTier::Deep));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(TEXT("StatelessEmitters")), NumStatelessEmitters, EPulseMetricUnit::Count, EPulseTier::Deep));
}

void FPulseNiagaraCollector::Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FPulseNiagaraThresholds& Thresholds = Context.Settings.Niagara;

	// Niagara writes its whole tag block atomically, so NumEmitters missing means every Fast tag is
	// missing. Fire the explicit Info issue so the stale save is a finding, not just a counter.
	int64 NumEmitters = 0;
	const bool bHasNumEmitters = InOutResult.GetIntMetric(FName(TEXT("NumEmitters")), NumEmitters);
	if (!bHasNumEmitters)
	{
		PulseNiagaraAddIssue(InOutResult, FName(TEXT("Niagara.TagsIncomplete")),
			TEXT("NumEmitters registry tag is absent: the system was saved before it fully loaded, so no Fast metrics exist for it. Resave to refresh registry tags."));
	}

	// Prefer ActiveEmitters (excludes disabled and scalability-culled emitters); fall back to the
	// raw handle count when only NumEmitters made it into the registry.
	int64 ActiveEmitters = 0;
	const bool bHasActiveEmitters = InOutResult.GetIntMetric(FName(TEXT("ActiveEmitters")), ActiveEmitters);
	if (bHasActiveEmitters || bHasNumEmitters)
	{
		const int64 EmitterCount = bHasActiveEmitters ? ActiveEmitters : NumEmitters;
		const TCHAR* EmitterCountSource = bHasActiveEmitters ? TEXT("ActiveEmitters") : TEXT("NumEmitters");
		if (EmitterCount > Thresholds.MaxEmitters)
		{
			PulseNiagaraAddIssue(InOutResult, FName(TEXT("Niagara.ExcessiveEmitters")),
				FString::Printf(TEXT("%s %lld exceeds MaxEmitters %d."), EmitterCountSource, EmitterCount, Thresholds.MaxEmitters));
		}
	}

	int64 ActiveRenderers = 0;
	if (InOutResult.GetIntMetric(FName(TEXT("ActiveRenderers")), ActiveRenderers) && ActiveRenderers > Thresholds.MaxRenderers)
	{
		PulseNiagaraAddIssue(InOutResult, FName(TEXT("Niagara.ExcessiveRenderers")),
			FString::Printf(TEXT("ActiveRenderers %lld exceeds MaxRenderers %d."), ActiveRenderers, Thresholds.MaxRenderers));
	}

	if (Thresholds.bFlagGPUMissingBounds)
	{
		int64 GPUSimsMissingFixedBounds = 0;
		if (InOutResult.GetIntMetric(FName(TEXT("GPUSimsMissingFixedBounds")), GPUSimsMissingFixedBounds) && GPUSimsMissingFixedBounds > 0)
		{
			PulseNiagaraAddIssue(InOutResult, FName(TEXT("Niagara.GPUSimMissingFixedBounds")),
				FString::Printf(TEXT("%lld GPU emitter sim(s) use dynamic bounds; each forces a GPU readback stall."), GPUSimsMissingFixedBounds));
		}
	}

	if (Thresholds.bRequireEffectType)
	{
		// The tag writer emits the literal string "None" for a null EffectType pointer, so this is
		// an exact-match check, not a null check.
		if (const FPulseMetric* EffectTypeMetric = InOutResult.FindMetric(FName(TEXT("EffectType"))))
		{
			const FString* EffectTypeName = EffectTypeMetric->Value.TryGet<FString>();
			if (EffectTypeName != nullptr && EffectTypeName->Equals(TEXT("None"), ESearchCase::CaseSensitive))
			{
				PulseNiagaraAddIssue(InOutResult, FName(TEXT("Niagara.MissingEffectType")),
					TEXT("EffectType is None; scalability, culling, and significance settings never apply to this system."));
			}
		}
	}

	if (Thresholds.bFlagNonPoolableSystems)
	{
		// Deep-only: IsPoolable simply does not exist at Fast tier, so the rule stays silent there
		// and the driver reports it as unevaluated rather than passed.
		bool bIsPoolable = false;
		if (InOutResult.GetBoolMetric(FName(TEXT("IsPoolable")), bIsPoolable) && !bIsPoolable)
		{
			PulseNiagaraAddIssue(InOutResult, FName(TEXT("Niagara.NotPoolable")),
				TEXT("System cannot be pooled (MaxPoolSize and PoolPrimeSize are both 0); every ENCPoolMethod spawn constructs a new component."));
		}
	}

	if (const FPulseMetric* WarmupMetric = InOutResult.FindMetric(FName(TEXT("WarmupTime"))))
	{
		const double* WarmupTime = WarmupMetric->Value.TryGet<double>();
		if (WarmupTime != nullptr && *WarmupTime > Thresholds.MaxWarmupTimeSeconds)
		{
			PulseNiagaraAddIssue(InOutResult, FName(TEXT("Niagara.ExcessiveWarmup")),
				FString::Printf(TEXT("WarmupTime %.4f s exceeds MaxWarmupTimeSeconds %.4f."), *WarmupTime, Thresholds.MaxWarmupTimeSeconds));
		}
	}
}

static TPulseAutoCollector<FPulseNiagaraCollector> GPulseNiagaraCollectorRegistrar;
