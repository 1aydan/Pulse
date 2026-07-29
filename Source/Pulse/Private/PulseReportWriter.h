// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseTypes.h"

struct FPulseReport;

/**
 * Writes every report artifact for one finalized run:
 *
 *   Summary.csv                 category scores + headline counters
 *   Assets_<Category>.csv       one row per asset, columns from each collector's metric schema
 *   Issues.csv                  one row per issue
 *   Statistics.json             the full report; byte-stable between runs on unchanged content
 *   PulseAudit_<timestamp>.json snapshot copy of Statistics.json, pruned to MaxHistoricalSnapshots
 *
 * MinSeverity filters ROWS ONLY — scores were computed before filtering and must not change.
 */
class FPulseReportWriter
{
public:
	FPulseReportWriter(const FString& InReportDir, bool bInWriteCsv, bool bInWriteJson, int32 InMaxHistoricalSnapshots);

	/** Writes everything. Returns false with a reason on the first IO failure. */
	bool Write(const FPulseReport& Report, EPulseSeverity MinSeverity, FString& OutError) const;

private:
	FString ReportDir;
	bool bWriteCsv = true;
	bool bWriteJson = true;
	int32 MaxHistoricalSnapshots = 50;
};
