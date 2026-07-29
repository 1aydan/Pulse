// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"

struct FPulseReport;
struct FPulseScoringSettings;

/**
 * The scoring formula, in one place so its semantics cannot drift between the commandlet and the
 * editor panel. Pure math over an already-populated report; no engine dependencies beyond FMath.
 *
 * Per asset:     S_a = 100 * exp(-(sum of severity weights) / AssetDecayScale)
 * Per category:  S_c = (100 - mean deficit) * (1 - Gamma * share of assets with issues)
 * Overall:       weighted blend of category scores over categories that actually have assets.
 */
class FPulseScoring
{
public:
	/**
	 * Sorts assets (by path string), issues (severity desc, then rule id), and metrics (by name)
	 * BEFORE summing, so float accumulation order is fixed and the result is bit-identical between
	 * runs. Then computes every score and the category rollup counters. Call exactly once.
	 */
	static void Finalize(FPulseReport& InOutReport, const FPulseScoringSettings& Scoring);
};
