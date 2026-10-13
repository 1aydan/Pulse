// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"

struct FPulseMemoryReport;

/**
 * Writes one memory audit to disk as a timestamped folder:
 *
 *   <ReportDir>/MemoryAudit_<timestamp>/
 *       Roots.csv          one row per top-level asset: total, unique and may-load cost
 *       Dependencies.csv   each root's largest dependencies, with the reference chain to each
 *       Assets.csv         every package any root loads, with its retained cost and reach
 *       Run.json           baseline, totals, settings, and what the numbers do and do not include
 *
 * Every byte column is a raw integer so the files sort and diff without parsing units. Unlike the
 * Niagara auditor's output, nothing here depends on the machine, so file contents carry no
 * timestamp: an unchanged project produces byte-identical files and a diff shows only real change.
 */
class FPulseMemoryReportWriter
{
public:
	FPulseMemoryReportWriter(const FString& InReportDir, int32 InMaxDependenciesPerRoot, int32 InMaxHistoricalRuns);

	/** Writes every file. False fills OutError with the first failure and stops. */
	bool Write(const FPulseMemoryReport& Report, FString& OutError);

	/** Absolute path of the run folder, valid after a successful Write. */
	const FString& GetRunDir() const { return RunDir; }

private:
	bool WriteRootsCsv(const FPulseMemoryReport& Report, FString& OutError) const;
	bool WriteDependenciesCsv(const FPulseMemoryReport& Report, FString& OutError) const;
	bool WriteAssetsCsv(const FPulseMemoryReport& Report, FString& OutError) const;
	bool WriteRunJson(const FPulseMemoryReport& Report, FString& OutError) const;

	/** Deletes the oldest run folders beyond MaxHistoricalRuns. Never touches unrelated files. */
	void PruneHistoricalRuns() const;

	FString ReportDir;
	FString RunDir;
	int32 MaxDependenciesPerRoot = 100;
	int32 MaxHistoricalRuns = 0;
};

/**
 * Root indices sorted most-resident-first, ties broken by asset path (case-sensitive, for a total
 * order). Shared with the panel so the list and the CSV agree on what "worst" means.
 */
TArray<int32> PulseSortRootsByResidentCost(const FPulseMemoryReport& Report);
