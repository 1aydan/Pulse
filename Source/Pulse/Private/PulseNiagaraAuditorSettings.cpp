// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseNiagaraAuditorSettings.h"

UPulseNiagaraAuditorSettings::UPulseNiagaraAuditorSettings()
{
	FDirectoryPath GameRoot;
	GameRoot.Path = TEXT("/Game");
	Scan.ScanPaths.Add(GameRoot);

	Scan.ExcludePathPatterns.Add(TEXT("/Developers/"));
	Scan.ExcludePathPatterns.Add(TEXT("/Collections/"));

	// Low and Epic by default rather than every level: the interesting result is the spread between
	// the extremes, and each added level multiplies a run that is already minutes long.
	Measurement.QualityLevelsToMeasure.Add(0);
	Measurement.QualityLevelsToMeasure.Add(3);

	// Near and far for the same reason. 500cm sits inside any sane cull radius; 20000cm is outside
	// most of them, so a system with working significance settings should collapse between the two.
	Measurement.MeasureDistances.Add(500.0f);
	Measurement.MeasureDistances.Add(20000.0f);
}

float UPulseNiagaraAuditorSettings::GetFrameBudgetMs() const
{
	const float StandardBudgetMs = PulseTargetFrameTimeMs(Budget.PerformanceTarget);
	return StandardBudgetMs > 0.0f ? StandardBudgetMs : Budget.CustomFrameBudgetMs;
}
