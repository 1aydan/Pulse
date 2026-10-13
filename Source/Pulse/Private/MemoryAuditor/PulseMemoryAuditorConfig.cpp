// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseMemoryAuditorConfig.h"

#include "Misc/Parse.h"
#include "Misc/Paths.h"

bool FPulseMemoryAuditorConfig::Parse(const FString& Params, FPulseMemoryAuditorConfig& OutConfig, FString& OutError)
{
	FString RawValue;

	if (FParse::Value(*Params, TEXT("path="), RawValue))
	{
		RawValue.ParseIntoArray(OutConfig.PathFilter, TEXT(","), /*InCullEmpty*/ true);
		for (FString& Path : OutConfig.PathFilter)
		{
			Path.TrimStartAndEndInline();
		}
		OutConfig.PathFilter.RemoveAll([](const FString& Path) { return Path.IsEmpty(); });
		if (OutConfig.PathFilter.Num() == 0)
		{
			OutError = TEXT("-path= was given but listed no paths.");
			return false;
		}
	}

	if (FParse::Value(*Params, TEXT("inspect="), RawValue))
	{
		RawValue.ParseIntoArray(OutConfig.InspectAssets, TEXT(","), /*InCullEmpty*/ true);
		for (FString& Path : OutConfig.InspectAssets)
		{
			Path.TrimStartAndEndInline();
		}
		OutConfig.InspectAssets.RemoveAll([](const FString& Path) { return Path.IsEmpty(); });
		if (OutConfig.InspectAssets.Num() == 0)
		{
			OutError = TEXT("-inspect= was given but listed no assets.");
			return false;
		}
	}

	if (FParse::Value(*Params, TEXT("reportdir="), RawValue))
	{
		OutConfig.ReportDir = RawValue.TrimQuotes();
	}

	if (FParse::Param(*Params, TEXT("nosoft")))
	{
		OutConfig.bSoftReferencesOverridden = true;
		OutConfig.bIncludeSoftReferencesFromCommandLine = false;
	}

	OutConfig.bFailOverBudget = FParse::Param(*Params, TEXT("failoverbudget"));
	return true;
}

void FPulseMemoryAuditorConfig::ApplySettingsDefaults(const UPulseMemoryAuditorSettings& Settings)
{
	Roots = Settings.Roots;
	Estimation = Settings.Estimation;
	Budget = Settings.Budget;
	ExcludePathPatterns = Settings.Scan.ExcludePathPatterns;
	MaxDependenciesPerRootInReport = Settings.Scan.MaxDependenciesPerRootInReport;
	MaxHistoricalRuns = Settings.Scan.MaxHistoricalRuns;

	if (bSoftReferencesOverridden)
	{
		Estimation.bIncludeSoftReferences = bIncludeSoftReferencesFromCommandLine;
	}

	if (PathFilter.Num() == 0)
	{
		for (const FDirectoryPath& Directory : Settings.Scan.ScanPaths)
		{
			if (!Directory.Path.IsEmpty())
			{
				PathFilter.Add(Directory.Path);
			}
		}
	}

	if (ReportDir.IsEmpty())
	{
		ReportDir = Settings.Scan.ReportDirectory;
	}
	if (FPaths::IsRelative(ReportDir))
	{
		ReportDir = FPaths::ProjectDir() / ReportDir;
	}
	ReportDir = FPaths::ConvertRelativePathToFull(ReportDir);
}

FString FPulseMemoryAuditorConfig::ToString() const
{
	return FString::Printf(
		TEXT("paths=[%s] maps=%s primaryassets=%s blueprints=%s gamemaps=%s alwaysloaded=%d rootclasses=%d inspect=%d soft=%s reportdir=%s"),
		*FString::Join(PathFilter, TEXT(",")),
		Roots.bIncludeMaps ? TEXT("true") : TEXT("false"),
		Roots.bIncludePrimaryAssets ? TEXT("true") : TEXT("false"),
		Roots.bIncludeBlueprints ? TEXT("true") : TEXT("false"),
		Roots.bIncludeGameMapsSettings ? TEXT("true") : TEXT("false"),
		Roots.AlwaysLoadedAssets.Num(),
		Roots.AdditionalRootClasses.Num(),
		InspectAssets.Num(),
		Estimation.bIncludeSoftReferences ? TEXT("true") : TEXT("false"),
		*ReportDir);
}
