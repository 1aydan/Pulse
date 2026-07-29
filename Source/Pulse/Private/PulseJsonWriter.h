// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseTypes.h"

struct FPulseReport;

/**
 * Renders the finalized report as pretty-printed JSON with a FIXED key order, driving TJsonWriter
 * directly — never FJsonObject, whose TMap backing has no ordering guarantee across rehashes.
 *
 * Determinism contract:
 *  - Byte-identical output for unchanged content. GeneratedAtUtc appears NOWHERE in the JSON; it
 *    lives only in the snapshot FILENAME.
 *  - Scores print %.2f and real metrics %.4f as raw number tokens — never the print policy's
 *    %.17g, which would render 87.52 as 87.519999999999996.
 *  - MinSeverity drops issues from the per-asset "issues" arrays ONLY. Assets with zero surviving
 *    issues stay in (their metrics matter), and scores were computed pre-filter and are written
 *    unchanged.
 */
FString PulseWriteReportJson(const FPulseReport& Report, EPulseSeverity MinSeverity);
