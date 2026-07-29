// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseScoring.h"

#include "PulseResult.h"
#include "PulseSettings.h"

static double PulseSeverityWeight(const FPulseScoringSettings& Scoring, EPulseSeverity Severity)
{
	switch (Severity)
	{
		case EPulseSeverity::Info:     return Scoring.WeightInfo;
		case EPulseSeverity::Low:      return Scoring.WeightLow;
		case EPulseSeverity::Medium:   return Scoring.WeightMedium;
		case EPulseSeverity::High:     return Scoring.WeightHigh;
		case EPulseSeverity::Critical: return Scoring.WeightCritical;
	}
	return 0.0;
}

/**
 * Fixes every ordering the report depends on, BEFORE any summation, so float accumulation order —
 * and therefore every score — is bit-identical between runs. All comparisons go through ToString()
 * with case-sensitive Compare: FName operator< is name-table index order, which varies per process
 * and is the single most common determinism bug in UE tooling.
 *
 * StableSort rather than Sort throughout: equal keys are possible (the same rule firing twice on
 * one asset at the same severity), and an unstable sort would let their relative order — and the
 * report bytes — drift between runs even though the inputs were identical.
 */
static void PulseSortReportForDeterminism(FPulseReport& Report)
{
	Report.Categories.StableSort([](const FPulseCategoryResult& A, const FPulseCategoryResult& B)
	{
		return A.Category.ToString().Compare(B.Category.ToString(), ESearchCase::CaseSensitive) < 0;
	});

	for (FPulseCategoryResult& Category : Report.Categories)
	{
		Category.Assets.StableSort([](const FPulseAssetResult& A, const FPulseAssetResult& B)
		{
			return A.AssetPath.ToString().Compare(B.AssetPath.ToString(), ESearchCase::CaseSensitive) < 0;
		});

		for (FPulseAssetResult& Asset : Category.Assets)
		{
			Asset.Issues.StableSort([](const FPulseIssue& A, const FPulseIssue& B)
			{
				if (A.Severity != B.Severity)
				{
					return A.Severity > B.Severity;
				}
				return A.RuleId.ToString().Compare(B.RuleId.ToString(), ESearchCase::CaseSensitive) < 0;
			});

			Asset.Metrics.StableSort([](const FPulseMetric& A, const FPulseMetric& B)
			{
				return A.Name.ToString().Compare(B.Name.ToString(), ESearchCase::CaseSensitive) < 0;
			});
		}
	}
}

void FPulseScoring::Finalize(FPulseReport& Report, const FPulseScoringSettings& Scoring)
{
	PulseSortReportForDeterminism(Report);

	// The ClampMin/ClampMax property metas only guard edits made through the editor UI; a hand-edited
	// ini can still deliver 0 (division by zero) or an out-of-range Gamma, so re-clamp here.
	const double DecayScale = FMath::Max(1.0, static_cast<double>(Scoring.AssetDecayScale));
	const double Gamma = FMath::Clamp(static_cast<double>(Scoring.CategoryBreadthWeight), 0.0, 1.0);

	for (FPulseCategoryResult& Category : Report.Categories)
	{
		const int32 NumAssets = Category.Assets.Num();
		Category.NumAssets = NumAssets;

		// Empty categories keep their default Score of 100 but are excluded from the overall blend
		// below — scoring absent content as perfect would inflate projects that simply have none.
		if (NumAssets == 0)
		{
			continue;
		}

		Category.NumAssetsWithIssues = 0;
		Category.NumMissingExpectedTags = 0;
		for (int32& Count : Category.IssueCountBySeverity)
		{
			Count = 0;
		}

		double DeficitSum = 0.0;
		int32 FailCount = 0;

		for (FPulseAssetResult& Asset : Category.Assets)
		{
			double Penalty = 0.0;
			bool bHasFailingIssue = false;

			for (const FPulseIssue& Issue : Asset.Issues)
			{
				Penalty += PulseSeverityWeight(Scoring, Issue.Severity);
				Category.IssueCountBySeverity[static_cast<int32>(Issue.Severity)]++;
				bHasFailingIssue |= Issue.Severity >= EPulseSeverity::Low;
			}

			const double AssetScore = FMath::Clamp(100.0 * FMath::Exp(-Penalty / DecayScale), 0.0, 100.0);
			Asset.Score = static_cast<float>(AssetScore);

			DeficitSum += 100.0 - AssetScore;
			if (bHasFailingIssue)
			{
				FailCount++;
			}
			Category.NumMissingExpectedTags += Asset.NumMissingExpectedTags;
		}

		Category.NumAssetsWithIssues = FailCount;

		const double MeanDeficit = DeficitSum / NumAssets;
		const double FailShare = static_cast<double>(FailCount) / NumAssets;
		const double CategoryScore = (100.0 - MeanDeficit) * (1.0 - Gamma * FailShare);

		Category.MeanAssetScore = static_cast<float>(FMath::Clamp(100.0 - MeanDeficit, 0.0, 100.0));
		Category.Score = static_cast<float>(FMath::Clamp(CategoryScore, 0.0, 100.0));
	}

	// Overall: the category-weight blend over categories that actually have assets. Find, not
	// FindRef — FindRef's value-initialized default would weight unlisted categories at 0 and
	// silently drop them from the blend; the documented default is 1.0.
	double WeightedScoreSum = 0.0;
	double WeightSum = 0.0;

	for (const FPulseCategoryResult& Category : Report.Categories)
	{
		if (Category.NumAssets == 0)
		{
			continue;
		}

		const float* Weight = Scoring.CategoryWeights.Find(Category.Category);
		const double CategoryWeight = Weight ? static_cast<double>(*Weight) : 1.0;

		WeightedScoreSum += CategoryWeight * static_cast<double>(Category.Score);
		WeightSum += CategoryWeight;
	}

	// No scorable categories (empty scan) or all weights zero: keep the default 100 rather than
	// dividing by zero. A truncated or empty run is flagged through the header, not the score.
	if (WeightSum > 0.0)
	{
		Report.OverallScore = static_cast<float>(FMath::Clamp(WeightedScoreSum / WeightSum, 0.0, 100.0));
	}
}
