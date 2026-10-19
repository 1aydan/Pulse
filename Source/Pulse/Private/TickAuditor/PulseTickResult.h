// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"

/** One component class's share of an actor class's tick cost. */
struct FPulseTickComponentCost
{
	/** Component class, e.g. CharacterMovementComponent. */
	FString ClassName;

	/** Ticking components of this class across every instance of the actor class. */
	int32 Count = 0;

	/** Average milliseconds per frame, all of them together. */
	double MsPerFrame = 0.0;

	/** Shortest tick interval among them, in seconds. 0 = every frame. */
	float MinTickInterval = 0.0f;
};

/** One actor class, as it ticked during the capture. */
struct FPulseTickClassRow
{
	/** Display name: the Blueprint's name, or the native class's. */
	FString ClassName;

	/** Blueprint asset path; empty for a native class. */
	FString AssetPath;

	/** Nearest native ancestor, e.g. Character. Same as ClassName for a native class. */
	FString NativeParent;

	/** Instances with at least one enabled tick when the capture started. */
	int32 NumInstances = 0;

	/** Instances whose own actor tick was enabled. */
	int32 NumActorTicks = 0;

	/** Enabled component ticks across all instances. */
	int32 NumComponentTicks = 0;

	/** The Blueprint implements Event Tick. */
	bool bHasEventTick = false;

	/** Shortest actor tick interval among instances, in seconds. 0 = every frame. */
	float MinActorTickInterval = 0.0f;

	/** Every instance together, average per frame: what this class costs the game thread. The column to rank by. */
	double TotalMsPerFrame = 0.0;

	/** The actor ticks alone (Event Tick and native Tick), every instance, per frame. */
	double ActorMsPerFrame = 0.0;

	/** TotalMsPerFrame / NumInstances. */
	double PerInstanceMsPerFrame = 0.0;

	/** The most expensive single instance's worst frame. */
	double WorstInstanceFrameMs = 0.0;

	/** Most expensive first. */
	TArray<FPulseTickComponentCost> Components;

	/** At least one tick function runs every frame, with no interval. */
	bool bTicksEveryFrame = false;

	/** Actor ticks enabled, but they cost next to nothing and there is no Event Tick: turn them off. */
	bool bIdleActorTick = false;

	bool bOverClassBudget = false;
	bool bOverInstanceBudget = false;
};

/** Everything one capture produced. */
struct FPulseTickReport
{
	FString GeneratedAtUtc;
	FString EngineVersion;
	FString MachineName;

	/** Package name of the world captured, PIE prefix stripped. */
	FString WorldName;

	int32 FramesCaptured = 0;
	double CapturedSeconds = 0.0;

	/** Ticks Pulse drove, and the total they cost per frame. */
	int32 NumTickFunctions = 0;
	double TotalMsPerFrame = 0.0;

	/** Frame budget the percentages and flags are computed against. */
	float FrameBudgetMs = 0.0f;
	float MaxFrameBudgetPercentPerClass = 0.0f;
	float MaxFrameBudgetPercentPerInstance = 0.0f;
	float IdleTickMicroseconds = 0.0f;

	/** Ticks that stopped being valid mid-capture (destroyed actors). Their partial time still counts. */
	int32 NumTargetsLost = 0;

	/** Most expensive first. */
	TArray<FPulseTickClassRow> Rows;
};
