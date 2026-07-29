// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"
#include "PulseResult.h"
#include "PulseTypes.h"

class UPulseSettings;
struct FAssetData;

/** One metric a collector can emit, declared up front so CSV columns are fixed across runs. */
struct FPulseMetricDesc
{
	/** Unqualified, e.g. "Triangles". The report writer prefixes the category on output. */
	FName Name;

	EPulseMetricKind Kind = EPulseMetricKind::Int;
	EPulseMetricUnit Unit = EPulseMetricUnit::None;

	// Lowest tier at which this metric is producible. A Fast run leaves Deep-tier columns empty
	// rather than absent, so the CSV header is identical run to run and diffs show only real change.
	EPulseTier MinTier = EPulseTier::Fast;

	const TCHAR* Description = nullptr;
};

/** One rule a collector can fire, declared up front so the report can list rules it did NOT evaluate. */
struct FPulseRuleDesc
{
	/** Stable, greppable, "<Category>.<Condition>", e.g. "StaticMesh.MissingLODs". Never rename. */
	FName RuleId;

	EPulseSeverity DefaultSeverity = EPulseSeverity::Medium;
	EPulseTier MinTier = EPulseTier::Fast;

	const TCHAR* Description = nullptr;

	// Filled into every FPulseIssue this rule produces, so the collector only supplies the observed
	// numbers and the fix text is authored in exactly one place.
	const TCHAR* Recommendation = nullptr;
};

/** Everything a collector is allowed to see. Deliberately narrow: no registry, no package loading. */
struct FPulseCollectContext
{
	/** Always valid. At Fast tier this is the ONLY thing populated. */
	const FAssetData& AssetData;

	/**
	 * The loaded object, or null. Non-null only when the achieved tier is >= Deep AND the collector
	 * declared EPulseLoadMode::Driver. The driver owns the load and the GC that follows it; a
	 * collector must never call LoadPackage/StaticLoadObject itself, or the GC cadence in the scan
	 * driver stops being a bound.
	 */
	UObject* LoadedObject = nullptr;

	/** Thresholds. Read once by the driver via GetDefault<UPulseSettings>() and handed down. */
	const UPulseSettings& Settings;

	/** The tier this particular asset actually reached. May be below RunTier if loading failed. */
	EPulseTier AchievedTier = EPulseTier::Fast;

	/** The tier the whole run was configured for. Use AchievedTier for decisions. */
	EPulseTier RunTier = EPulseTier::Fast;
};

/**
 * One asset class family's audit logic. Implement, then register with TPulseAutoCollector<> from
 * inside this plugin, or via FPulseCollectorRegistry::Register from an external module's
 * StartupModule — see PulseCollectorRegistry.h.
 *
 * The contract is deliberately split into Collect* (produce numbers) and Evaluate (turn numbers
 * into issues). That split is what makes tiers honest: Evaluate always sees the full metric set
 * the achieved tier could produce, so a rule whose input metric is missing simply does not fire,
 * and the driver reports it as "not evaluated" rather than "passed".
 *
 * Instances are created once at module startup and live for the process. They must be stateless
 * with respect to individual assets.
 */
class IPulseCollector : public IModularFeature
{
public:
	virtual ~IPulseCollector() = default;

	// ---- Identity -------------------------------------------------------------------------------

	/** Unique. Used for log lines, sorting, and duplicate rejection. e.g. "Pulse.StaticMesh". */
	virtual FName GetCollectorName() const = 0;

	/**
	 * Report category. Becomes the Assets_<Category>.csv filename, the -category= token, the JSON
	 * key, and the scoring bucket. Two collectors may share a category; results merge.
	 */
	virtual FName GetCategory() const = 0;

	/** Asset class this collector claims. Drives the asset registry filter. */
	virtual UClass* GetSupportedClass() const = 0;

	/** True to also claim subclasses. UTexture2D wants this; UWorld does not. */
	virtual bool SupportsSubclasses() const { return true; }

	// ---- Tier capability ------------------------------------------------------------------------

	/**
	 * Highest tier at which this collector does anything new. The driver uses this to decide
	 * whether an asset needs loading at all: a collector reporting Fast never causes a package
	 * load, even in a -deep run. Getting this right is the difference between a 40-second Deep
	 * run and a 40-minute one.
	 */
	virtual EPulseTier GetMaxUsefulTier() const { return EPulseTier::Fast; }

	/** Who loads at Deep tier. See EPulseLoadMode. */
	virtual EPulseLoadMode GetDeepLoadMode() const { return EPulseLoadMode::Driver; }

	// ---- Schema ---------------------------------------------------------------------------------

	/** Every metric this collector can ever emit, in CSV column order. Constant for the process. */
	virtual void GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const = 0;

	/** Every rule this collector can ever fire. Drives the "rules skipped at this tier" section. */
	virtual void GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const = 0;

	// ---- Work -----------------------------------------------------------------------------------

	/** Registry tags only. Must not load, must be fast. Always called. */
	virtual void CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const = 0;

	/** Called only when Context.AchievedTier >= Deep. LoadedObject is valid unless SelfManaged. */
	virtual void CollectDeep(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
	{
	}

	/** Called only when Context.AchievedTier == ShaderStats. Only the material collector implements this. */
	virtual void CollectShaderStats(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
	{
	}

	/**
	 * Turn the accumulated metrics into issues. Called exactly once, after all Collect* for this
	 * asset. Emit only RuleId + Severity + Message; the driver fills Recommendation from
	 * GetRuleSchema() and applies settings severity overrides and rule disables afterwards. Do NOT
	 * set Result.Score — scoring is centralised so the formula is uniform and settings-driven.
	 */
	virtual void Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const = 0;
};
