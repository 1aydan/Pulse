// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseRunConfig.h"

#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PulseSettings.h"

bool FPulseRunConfig::Parse(const FString& Params, FPulseRunConfig& OutConfig, FString& OutError)
{
	const TCHAR* Stream = *Params;

	OutConfig.bDeepRequested = FParse::Param(Stream, TEXT("deep"));
	OutConfig.bShaderStatsRequested = FParse::Param(Stream, TEXT("shaderstats"));

	// -shaderstats implies -deep: instruction counts need the material loaded AND compiled, which
	// strictly dominates the Deep cost. The commandlet logs the promotion so nobody misreads a run.
	if (OutConfig.bShaderStatsRequested)
	{
		OutConfig.RunTier = EPulseTier::ShaderStats;
	}
	else if (OutConfig.bDeepRequested)
	{
		OutConfig.RunTier = EPulseTier::Deep;
	}

	FString CategoryList;
	if (FParse::Value(Stream, TEXT("category="), CategoryList))
	{
		CategoryList.ParseIntoArray(OutConfig.CategoryFilter, TEXT(","), /*bCullEmpty*/ true);
		if (OutConfig.CategoryFilter.IsEmpty())
		{
			OutError = TEXT("-category= was given but contained no category names.");
			return false;
		}
	}

	FString PathList;
	if (FParse::Value(Stream, TEXT("path="), PathList))
	{
		PathList.ParseIntoArray(OutConfig.PathFilter, TEXT(","), /*bCullEmpty*/ true);
		if (OutConfig.PathFilter.IsEmpty())
		{
			OutError = TEXT("-path= was given but contained no paths.");
			return false;
		}
	}

	FString MinSeverityText;
	if (FParse::Value(Stream, TEXT("minseverity="), MinSeverityText))
	{
		if (!PulseParseSeverity(MinSeverityText, OutConfig.MinSeverity))
		{
			OutError = FString::Printf(TEXT("Unknown -minseverity value '%s'. Expected Info, Low, Medium, High, or Critical."), *MinSeverityText);
			return false;
		}
	}

	FParse::Value(Stream, TEXT("maxassets="), OutConfig.MaxAssetsPerCategory);
	if (OutConfig.MaxAssetsPerCategory < 0)
	{
		OutError = TEXT("-maxassets= must be zero or positive.");
		return false;
	}

	FParse::Value(Stream, TEXT("reportdir="), OutConfig.ReportDir);
	FParse::Value(Stream, TEXT("failunder="), OutConfig.FailUnderScore);

	if (FParse::Value(Stream, TEXT("gcfreq="), OutConfig.GCPackageInterval))
	{
		if (OutConfig.GCPackageInterval < 1)
		{
			OutError = TEXT("-gcfreq= must be at least 1.");
			return false;
		}
		OutConfig.bGCIntervalOverridden = true;
	}

	FString FormatList;
	if (FParse::Value(Stream, TEXT("format="), FormatList))
	{
		TArray<FString> Formats;
		FormatList.ParseIntoArray(Formats, TEXT(","), /*bCullEmpty*/ true);

		OutConfig.bWriteCsv = false;
		OutConfig.bWriteJson = false;
		for (const FString& Format : Formats)
		{
			if (Format.Equals(TEXT("csv"), ESearchCase::IgnoreCase))
			{
				OutConfig.bWriteCsv = true;
			}
			else if (Format.Equals(TEXT("json"), ESearchCase::IgnoreCase))
			{
				OutConfig.bWriteJson = true;
			}
			else
			{
				OutError = FString::Printf(TEXT("Unknown -format value '%s'. Expected csv, json, or csv,json."), *Format);
				return false;
			}
		}
		if (!OutConfig.bWriteCsv && !OutConfig.bWriteJson)
		{
			OutError = TEXT("-format= was given but selected no formats.");
			return false;
		}
	}

	return true;
}

void FPulseRunConfig::ApplySettingsDefaults(const UPulseSettings& Settings)
{
	if (ReportDir.IsEmpty())
	{
		ReportDir = Settings.Scan.ReportDirectory;
	}
	if (FPaths::IsRelative(ReportDir))
	{
		ReportDir = FPaths::ProjectDir() / ReportDir;
	}
	ReportDir = FPaths::ConvertRelativePathToFull(ReportDir);

	if (!bGCIntervalOverridden)
	{
		GCPackageInterval = Settings.Scan.GCPackageInterval;
	}

	if (PathFilter.IsEmpty())
	{
		for (const FDirectoryPath& Dir : Settings.Scan.ScanPaths)
		{
			PathFilter.Add(Dir.Path);
		}
	}
}

FString FPulseRunConfig::ToString() const
{
	return FString::Printf(
		TEXT("tier=%s categories=%s paths=%s minseverity=%s maxassets=%d reportdir=%s formats=%s%s failunder=%.2f gcfreq=%d"),
		PulseTierToString(RunTier),
		CategoryFilter.IsEmpty() ? TEXT("(all)") : *FString::Join(CategoryFilter, TEXT("+")),
		PathFilter.IsEmpty() ? TEXT("(settings)") : *FString::Join(PathFilter, TEXT("+")),
		PulseSeverityToString(MinSeverity),
		MaxAssetsPerCategory,
		*ReportDir,
		bWriteCsv ? TEXT("csv") : TEXT(""),
		bWriteJson ? (bWriteCsv ? TEXT("+json") : TEXT("json")) : TEXT(""),
		FailUnderScore,
		GCPackageInterval);
}
