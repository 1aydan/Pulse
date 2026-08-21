// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseNiagaraCostResult.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"

class AActor;
class FParticlePerfStatsListener_GatherAll;
class UNiagaraComponent;
class UNiagaraSystem;
class APlayerController;
class UWorld;

/** Which world the stage measures in. The choice decides what can honestly be measured. */
enum class EPulseNiagaraStageMode : uint8
{
	/**
	 * A private Game world the stage creates and ticks itself. Used by the commandlet. Nothing
	 * renders it and it has no player controller, so Niagara can cache no views — distance culling
	 * is paused for the run and the distance axis is unavailable.
	 */
	PrivateWorld,

	/**
	 * A Play In Editor session, with the player controller put into spectator state. Used by the
	 * panel and the console commands.
	 *
	 * This is the only mode that measures what the game actually does. PIE gives a real local
	 * PlayerController with a real viewport, which is the FIRST branch of
	 * FNiagaraWorldManager::PrepareCachedViewInfo — the same path a shipping build takes — rather
	 * than the editor-viewport fallback. Tick groups, game mode and world type are all the game's,
	 * not the editor's.
	 */
	PIEWorld,
};

/**
 * Spawns Niagara systems, ticks them, and reports what they cost.
 *
 * Measurement is stepped rather than blocking: BeginMeasurement, then TickMeasurement once per
 * frame until it returns false, then EndMeasurement. The commandlet drains that loop as fast as it
 * can; the panel and console commands advance exactly one frame per tick, because in PIE the frames
 * belong to the session and a sample is only valid once per rendered frame.
 *
 * In PrivateWorld mode the class is also its own engine loop: TickMeasurement hand-drives the frame
 * the engine would normally drive — OnBeginFrame (which is what ticks the perf-stats manager), the
 * world tick FNiagaraWorldManager rides on via FWorldDelegates, end-of-frame render-state updates,
 * the core ticker, and OnEndFrame.
 *
 * Only game-thread cost is reported. Render-thread and GPU figures are never emitted: a headless
 * run issues no scene render, so they would be structurally zero rather than genuinely cheap.
 */
class FPulseNiagaraCostStage : public FGCObject
{
public:
	FPulseNiagaraCostStage(const FPulseNiagaraMeasurementSettings& InMeasurement, EPulseNiagaraStageMode InMode);
	virtual ~FPulseNiagaraCostStage() override;

	/** Stands up the world, the environment, and the stats listener. False fills OutError. */
	bool Create(FString& OutError);

	/** Idempotent. Called by the destructor, so an early return never leaks the world. */
	void Destroy();

	/**
	 * Applies a Niagara quality level and verifies it took.
	 *
	 * Deliberately NOT Scalability::SetQualityLevels: Niagara reads its own fx.Niagara.QualityLevel,
	 * which general scalability only moves if the project's Scalability.ini happens to map
	 * EffectsQuality onto it. Where it does not, every quality level would measure identically and
	 * nothing would report an error — so the observed level is read back and disagreement is fatal.
	 */
	bool SetQualityLevel(int32 QualityLevel, FString& OutError);

	/** False in PIE, where a Step may advance at most one measurement frame. */
	bool AllowsMultipleFramesPerStep() const { return Mode == EPulseNiagaraStageMode::PrivateWorld; }

	/** True when distance culling ran for real, i.e. PIE. */
	bool IsCullingExercised() const { return Mode == EPulseNiagaraStageMode::PIEWorld; }

	/** True when the measured world is the configured test map. Always false in PrivateWorld mode. */
	bool IsInTestMap() const { return bInTestMap; }

	/** Compares a world against a configured test map by package name. A null TestMap never matches. */
	static bool IsWorldTheTestMap(const UWorld* InWorld, const TSoftObjectPtr<UWorld>& TestMap);

	/** Spawns the population and starts the settle window. */
	void BeginMeasurement(UNiagaraSystem& System, int32 QualityLevel, float Distance);

	/** Advances one frame. Returns true while more frames are needed. */
	bool TickMeasurement();

	/** Despawns and returns the finished sample. Valid only after TickMeasurement returned false. */
	FPulseNiagaraCostSample EndMeasurement();

	bool IsMeasuring() const { return bMeasuring; }

	/** Running average of the in-flight window, in milliseconds. Zero before the first frame lands. */
	double GetLiveAverageMs() const;

	/** Frames recorded so far in the in-flight window. */
	int32 GetLiveFramesRecorded() const { return FrameMilliseconds.Num(); }

	/** Instances currently on the stage. */
	int32 GetLiveInstanceCount() const { return Components.Num(); }

	//~ Begin FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;
	//~ End FGCObject

private:
	/** One hand-driven frame. PrivateWorld only — in PIE the session owns the frame. */
	void PumpFrame();

	/** Lights, sky, fog and a floor, so a rendered stage exercises the usual passes. */
	void BuildEnvironment();

	/** Where this measurement's instances go: relative to the player's view in PIE, origin otherwise. */
	FVector ResolveSpawnOrigin(float Distance) const;

	void SpawnInstances(UNiagaraSystem& System, float Distance);
	void DespawnInstances();

	/** Instance offset in the stage actor's local space: a centred, camera-facing grid. */
	FVector GridLocation(int32 Index) const;

	/** Half the diagonal extent of that grid, used to size the floor and to sanity-check distances. */
	float GridHalfExtent() const;

	bool HasTimedOut() const;

	const FPulseNiagaraMeasurementSettings& Measurement;
	const EPulseNiagaraStageMode Mode;

	TObjectPtr<UWorld> World;
	TObjectPtr<AActor> StageActor;
	TObjectPtr<AActor> EnvironmentActor;

	/** The PIE player controller whose view drives distance culling. Null outside PIE. */
	TWeakObjectPtr<APlayerController> ViewController;

	/**
	 * The view captured once at Create, and used for every spawn afterwards.
	 *
	 * Cached rather than queried per measurement on purpose: the whole distance axis is meaningless
	 * if the viewpoint moves between samples, so the run measures against one fixed view even if
	 * something manages to move the pawn.
	 */
	FVector LockedViewLocation = FVector::ZeroVector;
	FRotator LockedViewRotation = FRotator::ZeroRotator;
	TArray<TObjectPtr<UNiagaraComponent>> Components;

	TSharedPtr<FParticlePerfStatsListener_GatherAll, ESPMode::ThreadSafe> Listener;

	// ---- Live measurement state -----------------------------------------------------------------

	/** Weak: a GC between frames must not be prevented by, or invalidate, this pointer silently. */
	TWeakObjectPtr<UNiagaraSystem> MeasuredSystem;

	TArray<double> FrameMilliseconds;
	uint64 PreviousTotalCycles = 0;
	double MeasurementStartSeconds = 0.0;
	int32 SettleFramesRemaining = 0;
	int32 MeasureFramesRemaining = 0;
	int32 MeasuredQualityLevel = INDEX_NONE;
	float MeasuredDistance = GPulseNiagaraNoDistance;
	bool bMeasuring = false;
	bool bStatsReset = false;
	FString PendingSkipReason;

	// ---- Restored on teardown -------------------------------------------------------------------

	bool bQualityOverrideApplied = false;
	bool bInputLocked = false;
	bool bInTestMap = false;
};
