// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "PulseTickResult.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class UActorComponent;
class UPulseTickAuditorSettings;
class UWorld;
struct FPulseTickProbe;

/**
 * Measures what every actor and component tick in a live world costs, while the game is played.
 *
 * The engine has no per-tick-function timing outside a profiler capture, so the capture takes the
 * ticks over for a few seconds. When it starts, every enabled actor and component tick function in the
 * world is recorded and disabled, and one probe tick function is registered per tick group in use. Each
 * probe runs the disabled ticks of its group itself, through the tick function's own ExecuteTick — the
 * same call the engine makes for ticks that run while paused — honouring their intervals, and times
 * every call. When the capture ends everything is put back as it was.
 *
 * What that costs, said plainly so the numbers are read right:
 *  - Tick group is preserved, prerequisites within a group are not, so play can stutter briefly.
 *  - Work the engine would run on worker threads (parallel animation, async ticks) is timed on the game
 *    thread instead, so those classes read as more expensive than they are in game.
 *  - A tick that game code disables during the capture is re-enabled when it ends.
 *  - Actors spawned during the capture tick normally and are not measured.
 */
class FPulseTickCapture
{
public:
	// Both out of line: FPulseTickProbe is only complete in the .cpp, and an inline constructor would
	// instantiate its destructor here for the probe array's exception cleanup.
	FPulseTickCapture();
	~FPulseTickCapture();

	FPulseTickCapture(const FPulseTickCapture&) = delete;
	FPulseTickCapture& operator=(const FPulseTickCapture&) = delete;

	/** Arms a capture of World. It begins at the start of the next frame so every frame measured is whole. */
	bool Start(UWorld* InWorld, int32 InFramesToCapture, FString& OutError);

	/** Ends early. Ticks are restored; the partial capture stays reportable. */
	void Stop();

	bool IsRunning() const { return bArmed || bCapturing; }
	bool HasData() const { return FramesCompleted > 0; }
	int32 GetFramesCompleted() const { return FramesCompleted; }
	int32 GetFramesToCapture() const { return FramesToCapture; }

	/** Fires once, when the capture finishes or is stopped, after ticks have been restored. */
	FSimpleMulticastDelegate OnFinished;

	/** Aggregates per actor class. Valid any time after the first whole frame. */
	FPulseTickReport BuildReport(const UPulseTickAuditorSettings& Settings) const;

	/** Called by the probes. */
	void RunGroup(ETickingGroup Group, float DeltaTime, ELevelTick TickType);

private:
	/** One actor or component tick function Pulse is driving. */
	struct FTarget
	{
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<UActorComponent> Component;

		/** Raw: valid only while the owning object is, which is checked before every use. */
		FTickFunction* TickFunction = nullptr;

		ETickingGroup Group = TG_PrePhysics;
		float Interval = 0.0f;
		float AccumulatedDelta = 0.0f;

		/** Index into Instances: the actor this tick belongs to. */
		int32 InstanceIndex = INDEX_NONE;

		/** Component class name; empty for the actor's own tick. */
		FString ComponentClass;

		uint64 TotalCycles = 0;
		int32 Calls = 0;

		/** Destroyed, unregistered, or re-enabled by game code: no longer driven. */
		bool bReleased = false;
	};

	/** One actor, so a worst-frame figure can be kept per instance. */
	struct FInstance
	{
		TWeakObjectPtr<UClass> Class;
		uint64 FrameCycles = 0;
		double WorstFrameMs = 0.0;
	};

	void OnWorldTickStart(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds);
	void OnEndPlay(bool bIsSimulating);

	/** Records and disables every enabled tick, and registers the probes. */
	void BeginCapture();

	/** Unregisters the probes and re-enables everything still alive. Idempotent. */
	void EndCapture();

	/** Folds the frame's per-instance totals into each instance's worst frame. */
	void CloseFrame();

	void AddTarget(FTickFunction& TickFunction, AActor& Actor, UActorComponent* Component);
	bool IsTargetAlive(const FTarget& Target) const;

	TWeakObjectPtr<UWorld> World;
	int32 FramesToCapture = 0;
	int32 FramesCompleted = 0;
	bool bArmed = false;
	bool bCapturing = false;
	bool bFrameOpen = false;
	double CaptureStartSeconds = 0.0;
	double CaptureEndSeconds = 0.0;

	TArray<FTarget> Targets;
	TArray<FInstance> Instances;
	TArray<TUniquePtr<FPulseTickProbe>> Probes;

	FDelegateHandle WorldTickStartHandle;
	FDelegateHandle EndPlayHandle;
};
