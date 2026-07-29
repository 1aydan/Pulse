// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseTagUtils.h"

#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"

bool PulseGetTagInt(const FAssetData& AssetData, FName TagName, int64& OutValue)
{
	FString Text;
	if (!AssetData.GetTagValue(TagName, Text))
	{
		return false;
	}
	return LexTryParseString(OutValue, *Text);
}

bool PulseGetTagBool(const FAssetData& AssetData, FName TagName, bool& bOutValue)
{
	FString Text;
	if (!AssetData.GetTagValue(TagName, Text))
	{
		return false;
	}
	bOutValue = Text.ToBool();
	return true;
}

bool PulseGetTagReal(const FAssetData& AssetData, FName TagName, double& OutValue)
{
	FString Text;
	if (!AssetData.GetTagValue(TagName, Text))
	{
		return false;
	}
	return LexTryParseString(OutValue, *Text);
}

bool PulseGetTagString(const FAssetData& AssetData, FName TagName, FString& OutValue)
{
	return AssetData.GetTagValue(TagName, OutValue);
}

bool PulseAddIntTagMetric(FPulseAssetResult& InOutResult, const FAssetData& AssetData, FName TagName, FName MetricName, EPulseMetricUnit Unit)
{
	int64 Value = 0;
	if (!PulseGetTagInt(AssetData, TagName, Value))
	{
		InOutResult.NumMissingExpectedTags++;
		return false;
	}
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(MetricName, Value, Unit, EPulseTier::Fast));
	return true;
}

bool PulseAddBoolTagMetric(FPulseAssetResult& InOutResult, const FAssetData& AssetData, FName TagName, FName MetricName)
{
	bool bValue = false;
	if (!PulseGetTagBool(AssetData, TagName, bValue))
	{
		InOutResult.NumMissingExpectedTags++;
		return false;
	}
	InOutResult.Metrics.Add(FPulseMetric::MakeBool(MetricName, bValue, EPulseTier::Fast));
	return true;
}

bool PulseAddTextTagMetric(FPulseAssetResult& InOutResult, const FAssetData& AssetData, FName TagName, FName MetricName, bool bExpected)
{
	FString Value;
	if (!PulseGetTagString(AssetData, TagName, Value))
	{
		if (bExpected)
		{
			InOutResult.NumMissingExpectedTags++;
		}
		return false;
	}
	InOutResult.Metrics.Add(FPulseMetric::MakeText(MetricName, MoveTemp(Value), EPulseTier::Fast));
	return true;
}

FString PulseNormalizeExportedClassPath(const FString& ExportForm)
{
	// Export form is Class'/Script/Engine.Actor'. ExportTextPathToObjectPath strips the class
	// wrapper and quotes; it passes plain object paths through unchanged, so this is safe to call
	// on tags that were never wrapped.
	return FPackageName::ExportTextPathToObjectPath(ExportForm);
}
