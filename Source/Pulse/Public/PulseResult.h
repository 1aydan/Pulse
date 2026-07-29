// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/TVariant.h"
#include "PulseTypes.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"

/**
 * One measured quantity for one asset. Storage is a variant so the wrong-field bug class cannot
 * exist; Unit and SourceTier ride alongside because neither is derivable from the stored type.
 * Plain struct on purpose — the report model is never reflected, edited, or garbage collected.
 */
struct FPulseMetric
{
	/** Category-qualified at write time by the report writer: "Texture" + "SourceWidth". */
	FName Name;

	TVariant<bool, int64, double, FString> Value;

	EPulseMetricUnit Unit = EPulseMetricUnit::None;

	/** Which tier produced this. A Fast report structurally cannot carry a Deep metric. */
	EPulseTier SourceTier = EPulseTier::Fast;

	static FPulseMetric MakeInt(FName InName, int64 InValue, EPulseMetricUnit InUnit, EPulseTier InTier)
	{
		FPulseMetric Metric;
		Metric.Name = InName;
		Metric.Value.Set<int64>(InValue);
		Metric.Unit = InUnit;
		Metric.SourceTier = InTier;
		return Metric;
	}

	static FPulseMetric MakeReal(FName InName, double InValue, EPulseMetricUnit InUnit, EPulseTier InTier)
	{
		FPulseMetric Metric;
		Metric.Name = InName;
		Metric.Value.Set<double>(InValue);
		Metric.Unit = InUnit;
		Metric.SourceTier = InTier;
		return Metric;
	}

	static FPulseMetric MakeBool(FName InName, bool bInValue, EPulseTier InTier)
	{
		FPulseMetric Metric;
		Metric.Name = InName;
		Metric.Value.Set<bool>(bInValue);
		Metric.SourceTier = InTier;
		return Metric;
	}

	static FPulseMetric MakeText(FName InName, FString InValue, EPulseTier InTier)
	{
		FPulseMetric Metric;
		Metric.Name = InName;
		Metric.Value.Set<FString>(MoveTemp(InValue));
		Metric.SourceTier = InTier;
		return Metric;
	}

	/**
	 * Locale-invariant, fixed-precision CSV rendering. Reals always print %.4f — never %g, whose
	 * exponent switching makes an unchanged value render differently depending on magnitude and
	 * wrecks report diffs. Quoting is the CSV writer's job, not ours.
	 */
	FString ToCsvField() const
	{
		if (const bool* BoolValue = Value.TryGet<bool>())
		{
			return *BoolValue ? TEXT("true") : TEXT("false");
		}
		if (const int64* IntValue = Value.TryGet<int64>())
		{
			return FString::Printf(TEXT("%lld"), *IntValue);
		}
		if (const double* RealValue = Value.TryGet<double>())
		{
			return FString::Printf(TEXT("%.4f"), *RealValue);
		}
		if (const FString* TextValue = Value.TryGet<FString>())
		{
			return *TextValue;
		}
		return FString();
	}

	/** For aggregation (e.g. total texture memory). Text metrics contribute 0.0. */
	double AsDouble() const
	{
		if (const bool* BoolValue = Value.TryGet<bool>())
		{
			return *BoolValue ? 1.0 : 0.0;
		}
		if (const int64* IntValue = Value.TryGet<int64>())
		{
			return static_cast<double>(*IntValue);
		}
		if (const double* RealValue = Value.TryGet<double>())
		{
			return *RealValue;
		}
		return 0.0;
	}
};

/** One rule firing against one asset. */
struct FPulseIssue
{
	/**
	 * "<Category>.<Condition>", e.g. "StaticMesh.MissingLODs". Stable forever — renaming one is a
	 * breaking change to every CI baseline that references it.
	 */
	FName RuleId;

	EPulseSeverity Severity = EPulseSeverity::Info;

	/** What was observed, with the numbers in it: "8192x8192 source exceeds MaxDimension 4096." */
	FString Message;

	/** Filled by the driver from FPulseRuleDesc::Recommendation, so fix text is authored once. */
	FString Recommendation;

	/** The tier at which this fired. Lets a report explain that a rule needed -deep and got it. */
	EPulseTier DetectedAtTier = EPulseTier::Fast;
};

/** Everything known about one asset. */
struct FPulseAssetResult
{
	/** Sorted on lexically via ToString() for determinism — never by FName comparison index. */
	FSoftObjectPath AssetPath;

	FTopLevelAssetPath ClassPath;

	FName Category;

	/** Highest tier this asset actually reached. Below the run tier if its package failed to load. */
	EPulseTier AchievedTier = EPulseTier::Fast;

	/** Set by FPulseScoring, never by a collector. 0..100. */
	float Score = 100.0f;

	/**
	 * Expected registry tags that were absent. Missing is not zero: a stale registry (async
	 * compilation, old save) yields absent tags, and reporting those as healthy zeros is how an
	 * audit tool silently lies. Rolled up per category as a trust signal.
	 */
	int32 NumMissingExpectedTags = 0;

	TArray<FPulseIssue> Issues;
	TArray<FPulseMetric> Metrics;

	const FPulseMetric* FindMetric(FName InName) const
	{
		for (const FPulseMetric& Metric : Metrics)
		{
			if (Metric.Name == InName)
			{
				return &Metric;
			}
		}
		return nullptr;
	}

	bool GetIntMetric(FName InName, int64& OutValue) const
	{
		if (const FPulseMetric* Metric = FindMetric(InName))
		{
			if (const int64* IntValue = Metric->Value.TryGet<int64>())
			{
				OutValue = *IntValue;
				return true;
			}
		}
		return false;
	}

	bool GetBoolMetric(FName InName, bool& bOutValue) const
	{
		if (const FPulseMetric* Metric = FindMetric(InName))
		{
			if (const bool* BoolValue = Metric->Value.TryGet<bool>())
			{
				bOutValue = *BoolValue;
				return true;
			}
		}
		return false;
	}
};

/** One category's rollup. */
struct FPulseCategoryResult
{
	FName Category;

	int32 NumAssets = 0;

	/** Assets with at least one issue of severity >= Low. Drives the breadth term in scoring. */
	int32 NumAssetsWithIssues = 0;

	/** Indexed by EPulseSeverity. */
	int32 IssueCountBySeverity[5] = { 0, 0, 0, 0, 0 };

	/** Sum of per-asset NumMissingExpectedTags. Non-zero means this run is not fully trustworthy. */
	int32 NumMissingExpectedTags = 0;

	float MeanAssetScore = 100.0f;
	float Score = 100.0f;

	/** Rules this category declares but could not evaluate at the achieved tier. */
	TArray<FName> UnevaluatedRuleIds;

	TArray<FPulseAssetResult> Assets;
};

/** Reproducibility fingerprint. Written to report headers, never into row data. */
struct FPulseRunHeader
{
	/** From the .uplugin VersionName. */
	FString PulseVersion;

	/** FEngineVersion::Current().ToString(). */
	FString EngineVersion;

	FString ProjectName;

	EPulseTier RunTier = EPulseTier::Fast;
	bool bDeepRequested = false;
	bool bShaderStatsRequested = false;

	/** True if -maxassets clipped anything. Scores from a truncated run are not comparable. */
	bool bTruncated = false;

	FString MinSeverityFilter;
	TArray<FString> CategoryFilter;
	TArray<FString> PathFilter;

	/**
	 * MD5 over the settings block. A threshold edit shows up as one changed header line instead
	 * of unexplained churn across thousands of rows.
	 */
	FString SettingsHash;

	/** Set when shader stats were requested but could not be measured. Zeros are never reported. */
	FString ShaderStatsUnavailableReason;

	/** ISO 8601 UTC. Lives ONLY here and in the snapshot filename, so reports stay diffable. */
	FString GeneratedAtUtc;
};

struct FPulseReport
{
	FPulseRunHeader Header;

	float OverallScore = 100.0f;

	/** Sorted lexically by Category. */
	TArray<FPulseCategoryResult> Categories;
};
