// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseJsonWriter.h"

#include "Policies/PrettyJsonPrintPolicy.h"
#include "PulseResult.h"
#include "Serialization/JsonWriter.h"

using FPulseJsonWriter = TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>;

/**
 * Scores go out as raw %.2f number tokens. WriteValue(double) hands the policy a %.17g print,
 * which renders the rounded 87.52 as 87.519999999999996 — deterministic but unreadable and in
 * violation of the fixed-precision contract. Rounding before formatting keeps %.2f's own
 * round-to-nearest away from values sitting exactly on a half-cent boundary.
 */
static void PulseWriteScoreValue(FPulseJsonWriter& Writer, const TCHAR* Key, double Value)
{
	const double Rounded = FMath::RoundToDouble(Value * 100.0) / 100.0;
	const FString Number = FString::Printf(TEXT("%.2f"), Rounded);
	Writer.WriteRawJSONValue(FStringView(Key), FStringView(Number));
}

/** Typed metric write: bool as bool, int as number, real as a raw %.4f number, text as string. */
static void PulseWriteMetricValue(FPulseJsonWriter& Writer, const FPulseMetric& Metric)
{
	const FString Key = Metric.Name.ToString();

	if (const bool* BoolValue = Metric.Value.TryGet<bool>())
	{
		Writer.WriteValue(Key, *BoolValue);
	}
	else if (const int64* IntValue = Metric.Value.TryGet<int64>())
	{
		Writer.WriteValue(Key, *IntValue);
	}
	else if (const double* RealValue = Metric.Value.TryGet<double>())
	{
		// Same reasoning as scores: fixed %.4f, never the policy's %.17g.
		const FString Number = FString::Printf(TEXT("%.4f"), *RealValue);
		Writer.WriteRawJSONValue(FStringView(Key), FStringView(Number));
	}
	else if (const FString* TextValue = Metric.Value.TryGet<FString>())
	{
		Writer.WriteValue(Key, *TextValue);
	}
}

static void PulseWriteHeaderObject(FPulseJsonWriter& Writer, const FPulseRunHeader& Header)
{
	Writer.WriteObjectStart(TEXT("header"));
	Writer.WriteValue(TEXT("pulseVersion"), Header.PulseVersion);
	Writer.WriteValue(TEXT("engineVersion"), Header.EngineVersion);
	Writer.WriteValue(TEXT("project"), Header.ProjectName);
	Writer.WriteValue(TEXT("tier"), PulseTierToString(Header.RunTier));
	Writer.WriteValue(TEXT("deepRequested"), Header.bDeepRequested);
	Writer.WriteValue(TEXT("shaderStatsRequested"), Header.bShaderStatsRequested);
	Writer.WriteValue(TEXT("truncated"), Header.bTruncated);
	Writer.WriteValue(TEXT("minSeverityFilter"), Header.MinSeverityFilter);
	Writer.WriteValue(TEXT("categoryFilter"), Header.CategoryFilter);
	Writer.WriteValue(TEXT("pathFilter"), Header.PathFilter);
	Writer.WriteValue(TEXT("settingsHash"), Header.SettingsHash);
	if (!Header.ShaderStatsUnavailableReason.IsEmpty())
	{
		Writer.WriteValue(TEXT("shaderStatsUnavailableReason"), Header.ShaderStatsUnavailableReason);
	}
	// GeneratedAtUtc is deliberately absent — byte-identical reruns on unchanged content are a hard
	// gate, and a timestamp in the content would fail it on every run. It lives in the snapshot
	// FILENAME only.
	Writer.WriteObjectEnd();
}

static void PulseWriteAssetObject(FPulseJsonWriter& Writer, const FPulseCategoryResult& Category, const FPulseAssetResult& Asset, EPulseSeverity MinSeverity)
{
	Writer.WriteObjectStart();
	Writer.WriteValue(TEXT("path"), Asset.AssetPath.ToString());
	Writer.WriteValue(TEXT("class"), Asset.ClassPath.ToString());
	Writer.WriteValue(TEXT("achievedTier"), PulseTierToString(Asset.AchievedTier));
	PulseWriteScoreValue(Writer, TEXT("score"), Asset.Score);
	if (Asset.NumMissingExpectedTags > 0)
	{
		Writer.WriteValue(TEXT("numMissingExpectedTags"), Asset.NumMissingExpectedTags);
	}

	// Key order comes from the category's metric schema, not from the asset's (sorted) metric
	// array, so the JSON shape matches the CSV column order. Absent metrics are omitted — a Fast
	// run simply has no Deep keys.
	Writer.WriteObjectStart(TEXT("metrics"));
	for (const FPulseMetricDesc& Desc : Category.MetricSchema)
	{
		if (const FPulseMetric* Metric = Asset.FindMetric(Desc.Name))
		{
			PulseWriteMetricValue(Writer, *Metric);
		}
	}
	Writer.WriteObjectEnd();

	Writer.WriteArrayStart(TEXT("issues"));
	for (const FPulseIssue& Issue : Asset.Issues)
	{
		if (Issue.Severity < MinSeverity)
		{
			continue;
		}
		Writer.WriteObjectStart();
		Writer.WriteValue(TEXT("ruleId"), Issue.RuleId.ToString());
		Writer.WriteValue(TEXT("severity"), PulseSeverityToString(Issue.Severity));
		Writer.WriteValue(TEXT("tier"), PulseTierToString(Issue.DetectedAtTier));
		Writer.WriteValue(TEXT("message"), Issue.Message);
		Writer.WriteValue(TEXT("recommendation"), Issue.Recommendation);
		Writer.WriteObjectEnd();
	}
	Writer.WriteArrayEnd();
	Writer.WriteObjectEnd();
}

static void PulseWriteCategoryObject(FPulseJsonWriter& Writer, const FPulseCategoryResult& Category, EPulseSeverity MinSeverity)
{
	Writer.WriteObjectStart();
	Writer.WriteValue(TEXT("name"), Category.Category.ToString());
	PulseWriteScoreValue(Writer, TEXT("score"), Category.Score);
	PulseWriteScoreValue(Writer, TEXT("meanAssetScore"), Category.MeanAssetScore);
	Writer.WriteValue(TEXT("numAssets"), Category.NumAssets);
	Writer.WriteValue(TEXT("numAssetsWithIssues"), Category.NumAssetsWithIssues);
	Writer.WriteValue(TEXT("numMissingExpectedTags"), Category.NumMissingExpectedTags);

	Writer.WriteObjectStart(TEXT("issueCountBySeverity"));
	Writer.WriteValue(TEXT("info"), Category.IssueCountBySeverity[static_cast<int32>(EPulseSeverity::Info)]);
	Writer.WriteValue(TEXT("low"), Category.IssueCountBySeverity[static_cast<int32>(EPulseSeverity::Low)]);
	Writer.WriteValue(TEXT("medium"), Category.IssueCountBySeverity[static_cast<int32>(EPulseSeverity::Medium)]);
	Writer.WriteValue(TEXT("high"), Category.IssueCountBySeverity[static_cast<int32>(EPulseSeverity::High)]);
	Writer.WriteValue(TEXT("critical"), Category.IssueCountBySeverity[static_cast<int32>(EPulseSeverity::Critical)]);
	Writer.WriteObjectEnd();

	Writer.WriteArrayStart(TEXT("unevaluatedRules"));
	for (const FName& RuleId : Category.UnevaluatedRuleIds)
	{
		Writer.WriteValue(RuleId.ToString());
	}
	Writer.WriteArrayEnd();

	Writer.WriteArrayStart(TEXT("assets"));
	for (const FPulseAssetResult& Asset : Category.Assets)
	{
		PulseWriteAssetObject(Writer, Category, Asset, MinSeverity);
	}
	Writer.WriteArrayEnd();
	Writer.WriteObjectEnd();
}

FString PulseWriteReportJson(const FPulseReport& Report, EPulseSeverity MinSeverity)
{
	FString Result;
	TSharedRef<FPulseJsonWriter> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Result);

	Writer->WriteObjectStart();
	PulseWriteHeaderObject(*Writer, Report.Header);
	PulseWriteScoreValue(*Writer, TEXT("overallScore"), Report.OverallScore);

	Writer->WriteArrayStart(TEXT("categories"));
	for (const FPulseCategoryResult& Category : Report.Categories)
	{
		PulseWriteCategoryObject(*Writer, Category, MinSeverity);
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();

	// Close() is what flushes the string writer's byte buffer into Result — not optional.
	Writer->Close();
	return Result;
}
