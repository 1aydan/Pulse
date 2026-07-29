// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseTypes.generated.h"

/**
 * How much work a scan is allowed to do per asset. Ordered, not a bitmask: each tier strictly
 * contains the one below it, so `Tier >= EPulseTier::Deep` is meaningful. -shaderstats implies
 * -deep for the same reason — instruction counts need the material loaded AND compiled, which
 * strictly dominates the Deep cost for that asset.
 */
UENUM()
enum class EPulseTier : uint8
{
	/** Asset Registry tags only. Never loads a package. The CI default. */
	Fast = 0,

	/** Loads packages for metrics tags cannot provide (texture memory, tick flags, collision). */
	Deep = 1,

	/** Deep plus material shader compilation for instruction counts. Slowest; explicitly opt-in. */
	ShaderStats = 2
};

/** Issue severity. Ordered so a minimum-severity filter is a plain >= comparison. */
UENUM()
enum class EPulseSeverity : uint8
{
	Info = 0,
	Low = 1,
	Medium = 2,
	High = 3,
	Critical = 4
};

/** Storage type of a metric, declared in the schema so CSV columns are typed before data exists. */
enum class EPulseMetricKind : uint8
{
	Bool,
	Int,
	Real,
	Text
};

/** Display/aggregation unit of a metric. Separate from storage: a raw int64 cannot say "bytes". */
enum class EPulseMetricUnit : uint8
{
	None,
	Count,
	Bytes,
	Pixels,
	Triangles,
	Vertices,
	Bones,
	Seconds,
	Instructions
};

/** Who loads the package when a collector runs at Deep tier or above. */
enum class EPulseLoadMode : uint8
{
	/** The scan driver calls FAssetData::GetAsset() and owns the GC cadence. The default. */
	Driver,

	/** The collector performs its own scoped load. Worlds need this: LoadWorldPackageForEditor
	    plus FScopedEditorWorld, not a bare GetAsset(). */
	SelfManaged
};

inline const TCHAR* PulseTierToString(EPulseTier Tier)
{
	switch (Tier)
	{
		case EPulseTier::Fast:        return TEXT("Fast");
		case EPulseTier::Deep:        return TEXT("Deep");
		case EPulseTier::ShaderStats: return TEXT("ShaderStats");
	}
	return TEXT("Unknown");
}

inline const TCHAR* PulseSeverityToString(EPulseSeverity Severity)
{
	switch (Severity)
	{
		case EPulseSeverity::Info:     return TEXT("Info");
		case EPulseSeverity::Low:      return TEXT("Low");
		case EPulseSeverity::Medium:   return TEXT("Medium");
		case EPulseSeverity::High:     return TEXT("High");
		case EPulseSeverity::Critical: return TEXT("Critical");
	}
	return TEXT("Unknown");
}

inline bool PulseParseSeverity(const FString& Text, EPulseSeverity& OutSeverity)
{
	if (Text.Equals(TEXT("Info"), ESearchCase::IgnoreCase))     { OutSeverity = EPulseSeverity::Info;     return true; }
	if (Text.Equals(TEXT("Low"), ESearchCase::IgnoreCase))      { OutSeverity = EPulseSeverity::Low;      return true; }
	if (Text.Equals(TEXT("Medium"), ESearchCase::IgnoreCase))   { OutSeverity = EPulseSeverity::Medium;   return true; }
	if (Text.Equals(TEXT("High"), ESearchCase::IgnoreCase))     { OutSeverity = EPulseSeverity::High;     return true; }
	if (Text.Equals(TEXT("Critical"), ESearchCase::IgnoreCase)) { OutSeverity = EPulseSeverity::Critical; return true; }
	return false;
}
