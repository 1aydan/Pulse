// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseSceneAuditorSettings.h"
#include "PulseTypes.h"

/** One problem in one map, with the actors that make it up. */
struct FPulseSceneFinding
{
	/** e.g. Scene.RepeatedMesh. Stable across versions so reports can be diffed and filtered. */
	FString RuleId;

	EPulseSeverity Severity = EPulseSeverity::Low;

	FString Message;

	/** What to do about it. */
	FString Recommendation;

	/** The asset the finding is about, when there is one (the repeated mesh, say). */
	FString AssetPath;

	/** How many things the finding covers: placements, lights, components. */
	int32 Count = 0;

	/** Actors involved, by object name (for selecting them) and label (for reading). Capped. */
	TArray<FString> ActorNames;
	TArray<FString> ActorLabels;
};

/** One map, audited. */
struct FPulseSceneMap
{
	FString MapName;
	FString MapPath;

	bool bAudited = false;
	FString SkipReason;

	/** World Partition maps are audited as loaded: the persistent level and always-loaded actors only. */
	bool bWorldPartition = false;

	int32 NumActors = 0;
	int32 NumPrimitives = 0;
	int32 NumStaticMeshComponents = 0;
	int32 NumNaniteComponents = 0;
	int32 NumInstancedComponents = 0;
	int32 NumInstances = 0;
	int32 NumSkeletalMeshComponents = 0;

	/** LOD0 triangles across non-Nanite static mesh placements, instances included. */
	int64 NonNaniteTriangles = 0;

	int32 NumUniqueMeshes = 0;
	int32 NumUniqueMaterials = 0;

	/**
	 * Distinct (mesh, material) pairs among non-Nanite placements: roughly what the renderer can batch
	 * into, since identical pairs are instanced automatically. Not a draw call count — passes, LODs and
	 * visibility multiply it — but it moves with one.
	 */
	int32 NumMeshMaterialPairs = 0;

	int32 NumLights = 0;
	int32 NumMovableShadowLights = 0;
	int32 NumStationaryLights = 0;
	int32 NumMovableShadowCasters = 0;
	int32 NumTranslucentPrimitives = 0;

	/** Most severe first. */
	TArray<FPulseSceneFinding> Findings;

	int32 CountFindings(EPulseSeverity AtLeast) const
	{
		int32 Count = 0;
		for (const FPulseSceneFinding& Finding : Findings)
		{
			Count += static_cast<uint8>(Finding.Severity) >= static_cast<uint8>(AtLeast) ? 1 : 0;
		}
		return Count;
	}
};

struct FPulseSceneReport
{
	FString GeneratedAtUtc;
	FString EngineVersion;
	FPulseSceneThresholds Thresholds;

	/** Maps with the most severe findings first. */
	TArray<FPulseSceneMap> Maps;
};
