// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
// Included rather than forward declared: FPulseNiagaraCostBudget is held by value below.
#include "PulseNiagaraAuditorSettings.h"

struct FPulseNiagaraCostReport;

/**
 * Writes one auditor run to disk as a timestamped folder:
 *
 *   <ReportDir>/NiagaraAudit_<timestamp>/
 *       <QualityLevelName>.csv   one per measured level, full detail
 *       Comparison.csv           one row per system, cost per level side by side
 *       Run.json                 machine, engine, RHI, and every measurement parameter
 *
 * Quality level names come from UNiagaraSettings::QualityLevels via the report, never from a
 * hardcoded Low/Medium/High list: a project that renamed or added levels would otherwise get
 * confidently mislabelled files.
 *
 * Formatting is fixed-precision throughout so an unchanged run diffs clean — the same determinism
 * contract PulseWriteReportJson holds for the asset audit.
 */
class FPulseNiagaraCostReportWriter
{
public:
	FPulseNiagaraCostReportWriter(const FString& InReportDir, const FPulseNiagaraCostBudget& InBudget, int32 InMaxHistoricalRuns);

	/** Writes every file. False fills OutError with the first failure and stops. */
	bool Write(const FPulseNiagaraCostReport& Report, FString& OutError);

	/** Absolute path of the run folder, valid after a successful Write. */
	const FString& GetRunDir() const { return RunDir; }

private:
	/** Display name for a quality level, falling back to its number when the project named none. */
	static FString QualityName(const FPulseNiagaraCostReport& Report, int32 QualityIndex);

	bool WriteQualityCsv(const FPulseNiagaraCostReport& Report, int32 QualityIndex, FString& OutError) const;
	bool WriteComparisonCsv(const FPulseNiagaraCostReport& Report, FString& OutError) const;
	bool WriteRunJson(const FPulseNiagaraCostReport& Report, FString& OutError) const;

	/** Deletes the oldest run folders beyond MaxHistoricalRuns. Never touches unrelated files. */
	void PruneHistoricalRuns() const;

	FString ReportDir;
	FString RunDir;

	/** By value for the same reason the driver copies its config: callers build these as locals. */
	const FPulseNiagaraCostBudget Budget;

	int32 MaxHistoricalRuns = 0;
};
