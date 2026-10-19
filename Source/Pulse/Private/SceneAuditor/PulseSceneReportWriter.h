// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"

struct FPulseSceneReport;

/**
 * Writes one scene audit as a timestamped folder:
 *
 *   <ReportDir>/SceneAudit_<timestamp>/
 *       Maps.csv       one row per map: counts, triangles, lights, findings by severity
 *       Findings.csv   one row per finding, with the actors involved
 *       Run.json       thresholds and totals
 */
class FPulseSceneReportWriter
{
public:
	FPulseSceneReportWriter(const FString& InReportDir, int32 InMaxHistoricalRuns);

	bool Write(const FPulseSceneReport& Report, FString& OutError);

	const FString& GetRunDir() const { return RunDir; }

private:
	bool WriteMapsCsv(const FPulseSceneReport& Report, FString& OutError) const;
	bool WriteFindingsCsv(const FPulseSceneReport& Report, FString& OutError) const;
	bool WriteRunJson(const FPulseSceneReport& Report, FString& OutError) const;
	void PruneHistoricalRuns() const;

	FString ReportDir;
	FString RunDir;
	int32 MaxHistoricalRuns = 0;
};
