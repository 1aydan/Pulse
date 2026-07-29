// Copyright (c) 2026 Pulse contributors. MIT License.

#include "Collectors/PulseBlueprintCollector.h"

#include "Blueprint/BlueprintSupport.h"
#include "Engine/Blueprint.h"
#include "Engine/EngineBaseTypes.h"
#include "GameFramework/Actor.h"
#include "PulseCollectorRegistry.h"
#include "PulseSettings.h"
#include "PulseTagUtils.h"
#include "UObject/Class.h"

// Metric and rule names as plain literals, not FNames, so nothing here constructs an FName during
// static initialization (see the TPulseAutoCollector comment in PulseCollectorRegistry.h).
static const TCHAR* GPulseBlueprintMetricParentClass = TEXT("ParentClass");
static const TCHAR* GPulseBlueprintMetricNativeParentClass = TEXT("NativeParentClass");
static const TCHAR* GPulseBlueprintMetricNumReplicatedProperties = TEXT("NumReplicatedProperties");
static const TCHAR* GPulseBlueprintMetricNativeComponents = TEXT("NativeComponents");
static const TCHAR* GPulseBlueprintMetricBlueprintComponents = TEXT("BlueprintComponents");
static const TCHAR* GPulseBlueprintMetricIsDataOnly = TEXT("IsDataOnly");
static const TCHAR* GPulseBlueprintMetricBlueprintType = TEXT("BlueprintType");
static const TCHAR* GPulseBlueprintMetricTickEnabled = TEXT("TickEnabled");
static const TCHAR* GPulseBlueprintMetricTickStartEnabled = TEXT("TickStartEnabled");
static const TCHAR* GPulseBlueprintMetricTickInterval = TEXT("TickInterval");

static const TCHAR* GPulseBlueprintRuleTickEnabled = TEXT("Blueprint.TickEnabled");
static const TCHAR* GPulseBlueprintRuleTooManyComponents = TEXT("Blueprint.TooManyComponents");
static const TCHAR* GPulseBlueprintRuleTooManyReplicatedProperties = TEXT("Blueprint.TooManyReplicatedProperties");
static const TCHAR* GPulseBlueprintRuleCdoUnavailable = TEXT("Blueprint.CdoUnavailable");

/**
 * Reads a class-path tag (stored in export form, Class'/Script/Engine.Actor') and records it as a
 * normalized object-path text metric. Absence counts against the trust rollup like any other
 * expected tag.
 */
static void PulseBlueprintAddClassPathMetric(FPulseAssetResult& InOutResult, const FAssetData& AssetData, FName TagName, FName MetricName)
{
	FString ExportForm;
	if (!PulseGetTagString(AssetData, TagName, ExportForm))
	{
		InOutResult.NumMissingExpectedTags++;
		return;
	}
	InOutResult.Metrics.Add(FPulseMetric::MakeText(MetricName, PulseNormalizeExportedClassPath(ExportForm), EPulseTier::Fast));
}

/**
 * True when the Blueprint's class chain says it is an actor. GeneratedClass is authoritative when
 * it exists; the editor-side ParentClass is the fallback so a Blueprint whose compile failed (null
 * GeneratedClass) is still recognised as an actor Blueprint and reported rather than skipped.
 */
static bool PulseBlueprintChainIsActor(const UBlueprint* Blueprint)
{
	if (const UClass* GeneratedClass = Blueprint->GeneratedClass)
	{
		return GeneratedClass->IsChildOf(AActor::StaticClass());
	}
	if (const UClass* ParentClass = Blueprint->ParentClass)
	{
		return ParentClass->IsChildOf(AActor::StaticClass());
	}
	return false;
}

/** Emits RuleId + Severity + Message only; the driver fills Recommendation and DetectedAtTier. */
static void PulseBlueprintAddIssue(FPulseAssetResult& InOutResult, const TCHAR* RuleId, EPulseSeverity Severity, FString Message)
{
	FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
	Issue.RuleId = FName(RuleId);
	Issue.Severity = Severity;
	Issue.Message = MoveTemp(Message);
}

FName FPulseBlueprintCollector::GetCollectorName() const
{
	return FName(TEXT("Pulse.Blueprint"));
}

FName FPulseBlueprintCollector::GetCategory() const
{
	return FName(TEXT("Blueprint"));
}

UClass* FPulseBlueprintCollector::GetSupportedClass() const
{
	return UBlueprint::StaticClass();
}

bool FPulseBlueprintCollector::SupportsSubclasses() const
{
	// Exact class only. UBlueprint subclasses (Anim, Widget, ...) do not emit the NativeComponents
	// and BlueprintComponents tags, and letting them into this collector would turn every absent
	// component tag into either a false "missing tag" trust hit or a silent zero.
	return false;
}

EPulseTier FPulseBlueprintCollector::GetMaxUsefulTier() const
{
	// Deep adds the CDO tick inspection. ShaderStats adds nothing for Blueprints.
	return EPulseTier::Deep;
}

void FPulseBlueprintCollector::GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const
{
	OutMetrics.Add({ FName(GPulseBlueprintMetricParentClass), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Object path of the immediate parent class, normalized from the registry's export form. May be another Blueprint.") });
	OutMetrics.Add({ FName(GPulseBlueprintMetricNativeParentClass), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Object path of the nearest native C++ ancestor class, normalized from the registry's export form.") });
	OutMetrics.Add({ FName(GPulseBlueprintMetricNumReplicatedProperties), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Fast,
		TEXT("Replicated properties declared by the Blueprint class.") });
	OutMetrics.Add({ FName(GPulseBlueprintMetricNativeComponents), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Fast,
		TEXT("Components inherited from native C++ construction.") });
	OutMetrics.Add({ FName(GPulseBlueprintMetricBlueprintComponents), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Fast,
		TEXT("Components added by the Blueprint's Simple Construction Script.") });
	OutMetrics.Add({ FName(GPulseBlueprintMetricIsDataOnly), EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("True when the Blueprint carries only property overrides — no graphs, no added components.") });
	OutMetrics.Add({ FName(GPulseBlueprintMetricBlueprintType), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("EBlueprintType of the asset, e.g. BPTYPE_Normal or BPTYPE_Const.") });
	OutMetrics.Add({ FName(GPulseBlueprintMetricTickEnabled), EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Deep,
		TEXT("Actor CDO PrimaryActorTick.bCanEverTick — whether the class can ever tick. Actor Blueprints only.") });
	OutMetrics.Add({ FName(GPulseBlueprintMetricTickStartEnabled), EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Deep,
		TEXT("Actor CDO PrimaryActorTick.bStartWithTickEnabled — whether instances begin ticking at spawn. Actor Blueprints only.") });
	OutMetrics.Add({ FName(GPulseBlueprintMetricTickInterval), EPulseMetricKind::Real, EPulseMetricUnit::Seconds, EPulseTier::Deep,
		TEXT("Actor CDO PrimaryActorTick.TickInterval; 0 means every frame. Actor Blueprints only.") });
}

void FPulseBlueprintCollector::GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const
{
	OutRules.Add({ FName(GPulseBlueprintRuleTickEnabled), EPulseSeverity::Medium, EPulseTier::Deep,
		TEXT("Actor Blueprint ticks by default from spawn at an interval at or below AcceptableTickInterval."),
		TEXT("Replace per-frame Tick with timers, custom events, or delegate bindings; if periodic work is genuinely needed, set a sensible TickInterval in Class Defaults (for example 0.25 s) or disable Start with Tick Enabled.") });
	OutRules.Add({ FName(GPulseBlueprintRuleTooManyComponents), EPulseSeverity::Medium, EPulseTier::Fast,
		TEXT("Native plus Blueprint-added component count exceeds MaxComponents."),
		TEXT("Remove redundant components or split the actor into smaller cooperating actors; every component adds per-instance registration, memory, and often tick cost.") });
	OutRules.Add({ FName(GPulseBlueprintRuleTooManyReplicatedProperties), EPulseSeverity::Low, EPulseTier::Fast,
		TEXT("Replicated property count exceeds MaxReplicatedProperties."),
		TEXT("Trim replicated state: derive values locally where possible, apply replication conditions such as COND_InitialOnly, or convert one-shot notifications into RPCs.") });
	OutRules.Add({ FName(GPulseBlueprintRuleCdoUnavailable), EPulseSeverity::Info, EPulseTier::Deep,
		TEXT("Actor Blueprint's generated class or class default object was unavailable at Deep tier, so tick behaviour could not be measured."),
		TEXT("Open, compile, and resave the Blueprint in the editor; a Blueprint whose class fails to build cannot be audited for tick settings and is likely broken at runtime.") });
}

void FPulseBlueprintCollector::CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FAssetData& AssetData = Context.AssetData;

	// Class-path tags are stored in export form (Class'/Script/Engine.Actor'); normalize on read.
	PulseBlueprintAddClassPathMetric(InOutResult, AssetData, FBlueprintTags::ParentClassPath, FName(GPulseBlueprintMetricParentClass));
	PulseBlueprintAddClassPathMetric(InOutResult, AssetData, FBlueprintTags::NativeParentClassPath, FName(GPulseBlueprintMetricNativeParentClass));

	PulseAddIntTagMetric(InOutResult, AssetData, FBlueprintTags::NumReplicatedProperties, FName(GPulseBlueprintMetricNumReplicatedProperties), EPulseMetricUnit::Count);

	// FBlueprintTags::NumNativeComponents / NumBlueprintComponents are the constants whose FName
	// VALUES are "NativeComponents" / "BlueprintComponents" — the metric names mirror the tag text.
	PulseAddIntTagMetric(InOutResult, AssetData, FBlueprintTags::NumNativeComponents, FName(GPulseBlueprintMetricNativeComponents), EPulseMetricUnit::Count);
	PulseAddIntTagMetric(InOutResult, AssetData, FBlueprintTags::NumBlueprintComponents, FName(GPulseBlueprintMetricBlueprintComponents), EPulseMetricUnit::Count);

	PulseAddBoolTagMetric(InOutResult, AssetData, FBlueprintTags::IsDataOnly, FName(GPulseBlueprintMetricIsDataOnly));

	// BlueprintType is informational; older saves lack it, so absence is not a trust signal.
	PulseAddTextTagMetric(InOutResult, AssetData, FBlueprintTags::BlueprintType, FName(GPulseBlueprintMetricBlueprintType), /*bExpected*/ false);
}

void FPulseBlueprintCollector::CollectDeep(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	// Driver load mode: LoadedObject is whatever the registry filter matched. Exact-class filter
	// means this should always be a plain UBlueprint, but stay null-safe regardless.
	const UBlueprint* Blueprint = Cast<UBlueprint>(Context.LoadedObject);
	if (Blueprint == nullptr)
	{
		return;
	}

	if (UClass* GeneratedClass = Blueprint->GeneratedClass)
	{
		// Never force CDO construction from an audit: a broken Blueprint's CDO build can assert,
		// and constructing objects mid-scan changes the GC workload the driver budgeted for.
		if (const AActor* ActorCDO = Cast<AActor>(GeneratedClass->GetDefaultObject(/*bCreateIfNeeded*/ false)))
		{
			InOutResult.Metrics.Add(FPulseMetric::MakeBool(FName(GPulseBlueprintMetricTickEnabled), ActorCDO->PrimaryActorTick.bCanEverTick, EPulseTier::Deep));
			InOutResult.Metrics.Add(FPulseMetric::MakeBool(FName(GPulseBlueprintMetricTickStartEnabled), ActorCDO->PrimaryActorTick.bStartWithTickEnabled, EPulseTier::Deep));
			InOutResult.Metrics.Add(FPulseMetric::MakeReal(FName(GPulseBlueprintMetricTickInterval), ActorCDO->PrimaryActorTick.TickInterval, EPulseMetricUnit::Seconds, EPulseTier::Deep));
		}
	}

	// Non-actor Blueprints legitimately produce no tick metrics and no missing-tag count. An actor
	// Blueprint with no tick metrics is detected in Evaluate (Blueprint.CdoUnavailable) — the
	// absence of the metric plus the parent chain is the whole signal, so nothing to record here.
}

void FPulseBlueprintCollector::Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FPulseBlueprintThresholds& Thresholds = Context.Settings.Blueprint;

	// Data-only Blueprints are near-free at runtime: keep their metrics for the census, but skip
	// every rule so the report stays signal. Absent IsDataOnly tag means we cannot prove data-only,
	// so evaluation proceeds — absent is never treated as a value.
	bool bIsDataOnly = false;
	if (Thresholds.bSkipDataOnlyBlueprints && InOutResult.GetBoolMetric(FName(GPulseBlueprintMetricIsDataOnly), bIsDataOnly) && bIsDataOnly)
	{
		return;
	}

	// Blueprint.TickEnabled — Deep. All three tick metrics are emitted together, so gating on the
	// two bools is sufficient; the interval read cannot miss after they succeed.
	bool bTickEnabled = false;
	bool bTickStartEnabled = false;
	if (Thresholds.bFlagTickEnabledByDefault
		&& InOutResult.GetBoolMetric(FName(GPulseBlueprintMetricTickEnabled), bTickEnabled) && bTickEnabled
		&& InOutResult.GetBoolMetric(FName(GPulseBlueprintMetricTickStartEnabled), bTickStartEnabled) && bTickStartEnabled)
	{
		double TickInterval = 0.0;
		if (const FPulseMetric* IntervalMetric = InOutResult.FindMetric(FName(GPulseBlueprintMetricTickInterval)))
		{
			if (const double* IntervalValue = IntervalMetric->Value.TryGet<double>())
			{
				TickInterval = *IntervalValue;
			}
		}
		if (TickInterval <= Thresholds.AcceptableTickInterval)
		{
			PulseBlueprintAddIssue(InOutResult, GPulseBlueprintRuleTickEnabled, EPulseSeverity::Medium,
				FString::Printf(TEXT("Tick enabled by default and at spawn with TickInterval %.4f s, at or below AcceptableTickInterval %.4f s."),
					TickInterval, Thresholds.AcceptableTickInterval));
		}
	}

	// Blueprint.TooManyComponents — both counts must be present; a stale registry must not let one
	// absent tag read as zero and mask the total.
	int64 NativeComponents = 0;
	int64 BlueprintComponents = 0;
	if (InOutResult.GetIntMetric(FName(GPulseBlueprintMetricNativeComponents), NativeComponents)
		&& InOutResult.GetIntMetric(FName(GPulseBlueprintMetricBlueprintComponents), BlueprintComponents))
	{
		const int64 TotalComponents = NativeComponents + BlueprintComponents;
		if (TotalComponents > static_cast<int64>(Thresholds.MaxComponents))
		{
			PulseBlueprintAddIssue(InOutResult, GPulseBlueprintRuleTooManyComponents, EPulseSeverity::Medium,
				FString::Printf(TEXT("%lld native + %lld Blueprint components = %lld exceeds MaxComponents %d."),
					NativeComponents, BlueprintComponents, TotalComponents, Thresholds.MaxComponents));
		}
	}

	// Blueprint.TooManyReplicatedProperties
	int64 NumReplicatedProperties = 0;
	if (InOutResult.GetIntMetric(FName(GPulseBlueprintMetricNumReplicatedProperties), NumReplicatedProperties)
		&& NumReplicatedProperties > static_cast<int64>(Thresholds.MaxReplicatedProperties))
	{
		PulseBlueprintAddIssue(InOutResult, GPulseBlueprintRuleTooManyReplicatedProperties, EPulseSeverity::Low,
			FString::Printf(TEXT("%lld replicated properties exceeds MaxReplicatedProperties %d."),
				NumReplicatedProperties, Thresholds.MaxReplicatedProperties));
	}

	// Blueprint.CdoUnavailable — Deep. An actor Blueprint that reached Deep tier but produced no
	// tick metrics means the generated class or its CDO could not be inspected. Report it rather
	// than skip silently: "no data" must never masquerade as "no problem".
	if (Context.AchievedTier >= EPulseTier::Deep)
	{
		const UBlueprint* Blueprint = Cast<UBlueprint>(Context.LoadedObject);
		if (Blueprint != nullptr
			&& PulseBlueprintChainIsActor(Blueprint)
			&& InOutResult.FindMetric(FName(GPulseBlueprintMetricTickEnabled)) == nullptr)
		{
			PulseBlueprintAddIssue(InOutResult, GPulseBlueprintRuleCdoUnavailable, EPulseSeverity::Info,
				TEXT("Actor Blueprint's generated class or class default object was unavailable at Deep tier; tick settings could not be measured."));
		}
	}
}

static TPulseAutoCollector<FPulseBlueprintCollector> GPulseBlueprintCollectorRegistrar;
