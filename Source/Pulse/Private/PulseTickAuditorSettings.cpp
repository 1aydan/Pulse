// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseTickAuditorSettings.h"

float UPulseTickAuditorSettings::GetFrameBudgetMs() const
{
	const float StandardBudgetMs = PulseTargetFrameTimeMs(PerformanceTarget);
	return StandardBudgetMs > 0.0f ? StandardBudgetMs : CustomFrameBudgetMs;
}
