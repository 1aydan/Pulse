// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseSceneResult.h"

class UPulseSceneAuditorSettings;
class UWorld;

/** Everything one scene audit was asked to do. */
struct FPulseSceneConfig
{
	/** Maps to audit by package name. Empty = Maps to Cook, or every map under PathFilter. */
	TArray<FString> Maps;

	TArray<FString> PathFilter;
	TArray<FString> ExcludePathPatterns;
	bool bOnlyMapsToCook = true;
	FPulseSceneThresholds Thresholds;
	FString ReportDir;
	int32 MaxHistoricalRuns = 20;

	/** -maps=, -path=, -allmaps (ignore Maps to Cook), -reportdir=. */
	static bool Parse(const FString& Params, FPulseSceneConfig& OutConfig, FString& OutError);

	void ApplySettingsDefaults(const UPulseSceneAuditorSettings& Settings);

	FString ToString() const;

private:
	bool bAllMapsFromCommandLine = false;
};

/**
 * Looks at what a map actually places and lights, and flags what costs frame time for no visible
 * gain: a non-Nanite mesh placed hundreds of times as separate primitives, copy-pasted duplicates
 * stacked on each other, stationary lights that overlap past the shadowmap channel limit and fall back
 * to dynamic shadows, shadow-casting movable lights, small props drawn at any distance.
 *
 * Maps are loaded the way the level collector loads them — LoadWorldPackageForEditor and an editor
 * world scope with physics, navigation, AI and audio all off — inspected, and released. Nothing is
 * modified or saved. The map open in the editor is audited in place, which is also the only way its
 * findings can select actors in the viewport.
 */
class FPulseSceneAnalyzer
{
public:
	using FProgressCallback = TFunction<bool(const FText& Stage, float Fraction)>;

	explicit FPulseSceneAnalyzer(const FPulseSceneConfig& InConfig);

	/** Audits every map the config names. False only when there was nothing to audit or it was cancelled. */
	bool Run(FPulseSceneReport& OutReport, FString& OutError, const FProgressCallback& Progress = FProgressCallback());

	/**
	 * Audits a world that is already loaded and initialized. bLiveEditorWorld is true only for the map
	 * the user has open: the editor keeps its stationary-light channel preview current there, and
	 * nowhere else — a world loaded for the audit has the field, but nothing has ever assigned it.
	 */
	static void AuditWorld(UWorld& World, const FPulseSceneThresholds& Thresholds, bool bLiveEditorWorld, FPulseSceneMap& OutMap);

	/** Map package names the config resolves to, sorted. */
	TArray<FString> ResolveMaps() const;

	/** A report header with timestamps and thresholds filled in. */
	static FPulseSceneReport MakeReport(const FPulseSceneThresholds& Thresholds);

	/** Orders maps worst first: High and Critical findings, then Medium, then primitive count. */
	static void SortMaps(TArray<FPulseSceneMap>& Maps);

private:
	/**
	 * Loads and audits one map. Its streaming sublevels are not loaded by an editor-world scope, so
	 * their package names come back in OutSublevels to be audited as rows of their own.
	 */
	void AuditMapPackage(const FString& PackageName, FPulseSceneMap& OutMap, TArray<FString>& OutSublevels) const;

	const FPulseSceneConfig Config;
};
