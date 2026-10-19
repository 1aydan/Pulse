// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"

struct FPulseTickReport;

/**
 * Writes one tick capture as a timestamped folder:
 *
 *   <ReportDir>/TickCapture_<timestamp>/
 *       Classes.csv      one row per actor class, most expensive first
 *       Components.csv   one row per (actor class, component class)
 *       Run.json         world, machine, frames, budget, and what the capture does to the numbers
 *
 * Measured milliseconds depend on the machine and on what the game was doing, so Run.json carries
 * the timestamp and machine name the static reports leave out.
 */
class FPulseTickReportWriter
{
public:
	FPulseTickReportWriter(const FString& InReportDir, int32 InMaxHistoricalRuns);

	bool Write(const FPulseTickReport& Report, FString& OutError);

	const FString& GetRunDir() const { return RunDir; }

private:
	bool WriteClassesCsv(const FPulseTickReport& Report, FString& OutError) const;
	bool WriteComponentsCsv(const FPulseTickReport& Report, FString& OutError) const;
	bool WriteRunJson(const FPulseTickReport& Report, FString& OutError) const;
	void PruneHistoricalRuns() const;

	FString ReportDir;
	FString RunDir;
	int32 MaxHistoricalRuns = 0;
};
