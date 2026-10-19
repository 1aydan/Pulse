// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"

struct FPulseCleanupReport;

/**
 * Writes one cleanup run as a timestamped folder:
 *
 *   <ReportDir>/ContentCleanup_<timestamp>/
 *       Unused.csv       one row per unused asset or redirector, largest first
 *       Duplicates.csv   one row per copy, grouped by source file, most wasteful group first
 *       Run.json         what kept content alive, what was searched, totals
 *
 * Deterministic like the memory auditor's output: raw byte counts, stable ordering, no timestamp in
 * file contents.
 */
class FPulseCleanupReportWriter
{
public:
	FPulseCleanupReportWriter(const FString& InReportDir, int32 InMaxHistoricalRuns);

	bool Write(const FPulseCleanupReport& Report, FString& OutError);

	const FString& GetRunDir() const { return RunDir; }

private:
	bool WriteUnusedCsv(const FPulseCleanupReport& Report, FString& OutError) const;
	bool WriteDuplicatesCsv(const FPulseCleanupReport& Report, FString& OutError) const;
	bool WriteRunJson(const FPulseCleanupReport& Report, FString& OutError) const;
	void PruneHistoricalRuns() const;

	FString ReportDir;
	FString RunDir;
	int32 MaxHistoricalRuns = 0;
};
