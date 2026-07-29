// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseResult.h"
#include "PulseTypes.h"

struct FAssetData;

// Typed Asset Registry tag readers shared by every collector. All of them return false when the
// tag is ABSENT, which is not the same as zero: a stale registry (async compilation still running
// at save time, an old resave) yields absent tags, and reporting those as healthy zeros is how an
// audit tool silently lies. The Add* helpers record absence on the result for the trust rollup.

bool PulseGetTagInt(const FAssetData& AssetData, FName TagName, int64& OutValue);
bool PulseGetTagBool(const FAssetData& AssetData, FName TagName, bool& bOutValue);
bool PulseGetTagReal(const FAssetData& AssetData, FName TagName, double& OutValue);
bool PulseGetTagString(const FAssetData& AssetData, FName TagName, FString& OutValue);

/**
 * Reads an integer tag and appends it as a metric. Returns false and increments
 * InOutResult.NumMissingExpectedTags when the tag is absent — no metric is emitted, so the rule
 * that would have consumed it simply does not fire.
 */
bool PulseAddIntTagMetric(FPulseAssetResult& InOutResult, const FAssetData& AssetData, FName TagName, FName MetricName, EPulseMetricUnit Unit);

/** Bool-tag variant of PulseAddIntTagMetric. */
bool PulseAddBoolTagMetric(FPulseAssetResult& InOutResult, const FAssetData& AssetData, FName TagName, FName MetricName);

/** String-tag variant. bExpected controls whether absence counts against the trust rollup. */
bool PulseAddTextTagMetric(FPulseAssetResult& InOutResult, const FAssetData& AssetData, FName TagName, FName MetricName, bool bExpected = true);

/**
 * Normalizes an export-form class path — Class'/Script/Engine.Actor' — to a plain object path,
 * /Script/Engine.Actor. Blueprint ParentClass/NativeParentClass tags are stored in export form.
 */
FString PulseNormalizeExportedClassPath(const FString& ExportForm);
