// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseNiagaraCostStage.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Editor/EditorEngine.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "NiagaraComponent.h"
#include "NiagaraPlatformSet.h"
#include "NiagaraSystem.h"
#include "Particles/ParticlePerfStatsManager.h"
#include "Pulse.h"

#define LOCTEXT_NAMESPACE "PulseNiagaraAuditor"

/** Instances per row on the stage grid. Layout only — spacing is what affects culling, not width. */
static constexpr int32 GPulseNiagaraGridColumns = 8;

/** Cycles to milliseconds — the same unit as the frame budget every threshold is expressed in. */
static double PulseCyclesToMilliseconds(uint64 Cycles)
{
	return FPlatformTime::ToMilliseconds64(Cycles);
}

FPulseNiagaraCostStage::FPulseNiagaraCostStage(const FPulseNiagaraMeasurementSettings& InMeasurement, EPulseNiagaraStageMode InMode)
	: Measurement(InMeasurement)
	, Mode(InMode)
{
}

FPulseNiagaraCostStage::~FPulseNiagaraCostStage()
{
	Destroy();
}

void FPulseNiagaraCostStage::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(World);
	Collector.AddReferencedObject(StageActor);
	Collector.AddReferencedObject(EnvironmentActor);
	Collector.AddReferencedObjects(Components);
}

FString FPulseNiagaraCostStage::GetReferencerName() const
{
	return TEXT("FPulseNiagaraCostStage");
}

bool FPulseNiagaraCostStage::IsWorldTheTestMap(const UWorld* InWorld, const TSoftObjectPtr<UWorld>& TestMap)
{
	if (InWorld == nullptr || TestMap.IsNull())
	{
		return false;
	}

	// By package name, not by resolving the soft pointer: resolving would LOAD the test map just to
	// answer a comparison, which is the opposite of what a check like this should cost.
	return InWorld->GetOutermost()->GetName() == TestMap.ToSoftObjectPath().GetLongPackageName();
}

bool FPulseNiagaraCostStage::Create(FString& OutError)
{
#if !WITH_PER_SYSTEM_PARTICLE_PERF_STATS
	OutError = TEXT("Per-system particle perf stats are compiled out of this build (WITH_PER_SYSTEM_PARTICLE_PERF_STATS is 0). Niagara cost cannot be measured here.");
	return false;
#else
	checkf(World == nullptr, TEXT("FPulseNiagaraCostStage::Create called twice"));

	if (Mode == EPulseNiagaraStageMode::PIEWorld)
	{
		// The driver only creates the stage once GEditor->PlayWorld is up, so a null here means the
		// PIE session died between the check and now.
		World = GEditor != nullptr ? GEditor->PlayWorld : nullptr;
		if (World == nullptr)
		{
			OutError = TEXT("No Play In Editor session is running, so there is no game world to measure in.");
			return false;
		}

		ViewController = GEngine->GetFirstLocalPlayerController(World);
		if (!ViewController.IsValid())
		{
			OutError = TEXT("The PIE session has no local player controller, so Niagara would have no view to cull against.");
			return false;
		}

		// Spectator state even when the map already uses APulseAuditGameMode, because most
		// maps will not: whatever pawn the project's default game mode spawned would otherwise tick
		// its physics and animation alongside the systems being measured. ChangeState spawns the
		// spectator pawn itself, so nothing needs to be constructed here.
		if (APawn* PossessedPawn = ViewController->GetPawn())
		{
			ViewController->UnPossess();
			PossessedPawn->Destroy();
		}
		ViewController->ChangeState(FName(TEXT("Spectating")));

		// Input off for the duration. A spectator pawn is free-flying, and one nudge of WASD mid-run
		// moves the view every remaining distance sample is measured against — silently, and only
		// visible later as a distance axis that stopped separating.
		ViewController->SetIgnoreMoveInput(true);
		ViewController->SetIgnoreLookInput(true);
		bInputLocked = true;

		ViewController->GetPlayerViewPoint(LockedViewLocation, LockedViewRotation);

		// Pitch and roll are discarded, and the controller is re-aimed level.
		//
		// The grid is placed at LockedViewLocation + Forward * Distance, so any downward pitch sinks
		// it by Distance * sin(pitch) — a metre or two at 500 cm, but tens of metres at 20,000 cm,
		// which puts the far samples underneath the floor while the near ones sit above it. Since
		// the floor is what gives the population its shadows and depth-fade, that is a difference in
		// rendering conditions along the very axis being compared, not just an ugly frame.
		//
		// Levelling makes distance purely horizontal, so every population lands at eye height
		// whatever the distance and whatever the editor camera happened to be doing.
		LockedViewRotation = FRotator(0.0, LockedViewRotation.Yaw, 0.0);
		ViewController->SetControlRotation(LockedViewRotation);

		// PIE package names carry a UEDPIE_N_ prefix, so the comparison has to be against the
		// stripped name or a test map would never match its own PIE duplicate.
		const FString PlayPackageName = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
		bInTestMap = !Measurement.TestMap.IsNull()
			&& PlayPackageName == Measurement.TestMap.ToSoftObjectPath().GetLongPackageName();

		if (!Measurement.TestMap.IsNull() && !bInTestMap)
		{
			// Not fatal — measuring where you are is a legitimate choice — but the numbers will carry
			// whatever else this level is spending on the game thread, so say so once.
			UE_LOG(LogPulse, Warning,
				TEXT("Measuring in '%s' rather than the configured test map '%s'. The level's own content competes for the game thread, so these costs are not comparable with test-map runs."),
				*PlayPackageName, *Measurement.TestMap.ToSoftObjectPath().GetLongPackageName());
		}
	}
	else
	{
		// bInformEngineOfWorld is false because the world context is created explicitly below; letting
		// CreateWorld do it as well would register the world twice. bAddToRoot defaults to true, which
		// is what keeps the stage alive across the collections run between systems.
		World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
		if (World == nullptr)
		{
			OutError = TEXT("UWorld::CreateWorld returned null.");
			return false;
		}

		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(World);

		// FNiagaraWorldManager is created off FWorldDelegates::OnPostWorldInitialization (already
		// broadcast by CreateWorld) and ticks off the OnWorldPreActorTick/OnWorldPostActorTick pair
		// that UWorld::Tick broadcasts, so a plain Game world needs no Niagara-specific setup.
		World->InitializeActorsForPlay(FURL());
		World->BeginPlay();
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	StageActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (StageActor == nullptr)
	{
		OutError = TEXT("Failed to spawn the stage actor.");
		Destroy();
		return false;
	}

	USceneComponent* RootComponent = NewObject<USceneComponent>(StageActor, TEXT("PulseStageRoot"), RF_Transient);
	StageActor->SetRootComponent(RootComponent);
	RootComponent->RegisterComponent();

	// Private worlds are always bare, so the rig is purely a settings choice there. In the editor it
	// is additionally gated on being in the configured test map — see bSpawnEnvironmentRig.
	if (Measurement.bSpawnEnvironmentRig && (Mode == EPulseNiagaraStageMode::PrivateWorld || bInTestMap))
	{
		BuildEnvironment();
	}

	// System stats only. Component stats would let a per-instance figure be read directly, but they
	// allocate a stats block per component and this stage spawns them by the hundred; the
	// per-instance number is the system total over a known instance count either way.
	Listener = MakeShared<FParticlePerfStatsListener_GatherAll, ESPMode::ThreadSafe>(
		/*bNeedsWorldStats*/ false, /*bNeedsSystemStats*/ true, /*bNeedsComponentStats*/ false);
	FParticlePerfStatsManager::AddListener(Listener);

	UE_LOG(LogPulse, Display, TEXT("Niagara cost stage created (%s world, rendering %s, distance culling %s)."),
		Mode == EPulseNiagaraStageMode::PIEWorld ? TEXT("PIE") : TEXT("private"),
		FApp::CanEverRender() ? TEXT("available") : TEXT("unavailable"),
		IsCullingExercised() ? TEXT("live") : TEXT("paused"));
	return true;
#endif
}

void FPulseNiagaraCostStage::BuildEnvironment()
{
	// Only worth building where something will actually render it. With no RHI these components
	// cost a little to register and contribute nothing, so skip them rather than pretend.
	if (!FApp::CanEverRender())
	{
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	EnvironmentActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (EnvironmentActor == nullptr)
	{
		return;
	}

	USceneComponent* Root = NewObject<USceneComponent>(EnvironmentActor, TEXT("PulseEnvironmentRoot"), RF_Transient);
	EnvironmentActor->SetRootComponent(Root);
	Root->RegisterComponent();

	// Anchored to the locked view, not the world origin. In a test map the camera may be nowhere near
	// the origin, and lights left back there would light nothing the run ever spawns.
	EnvironmentActor->SetActorLocation(LockedViewLocation);

	// Mirrors APSOForgeEnvironmentRig: enough of a scene that the common passes — dynamic shadows,
	// local light shadows, fog, sky — are active, so translucent and lit Niagara renderers are not
	// measured against an empty black void they would never see in a real level.
	UDirectionalLightComponent* Sun = NewObject<UDirectionalLightComponent>(EnvironmentActor, TEXT("Sun"), RF_Transient);
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->SetupAttachment(Root);
	Sun->RegisterComponent();
	Sun->SetCastShadows(true);
	Sun->SetRelativeRotation(FRotator(-50.0f, 30.0f, 0.0f));

	USkyLightComponent* SkyLight = NewObject<USkyLightComponent>(EnvironmentActor, TEXT("SkyLight"), RF_Transient);
	SkyLight->SetMobility(EComponentMobility::Movable);
	SkyLight->SetupAttachment(Root);
	SkyLight->RegisterComponent();

	USkyAtmosphereComponent* Atmosphere = NewObject<USkyAtmosphereComponent>(EnvironmentActor, TEXT("SkyAtmosphere"), RF_Transient);
	Atmosphere->SetupAttachment(Root);
	Atmosphere->RegisterComponent();

	UPointLightComponent* PointLight = NewObject<UPointLightComponent>(EnvironmentActor, TEXT("PointLight"), RF_Transient);
	PointLight->SetMobility(EComponentMobility::Movable);
	PointLight->SetupAttachment(Root);
	PointLight->RegisterComponent();
	PointLight->SetCastShadows(true);
	PointLight->SetAttenuationRadius(5000.0f);
	PointLight->SetRelativeLocation(FVector(200.0f, -400.0f, 500.0f));

	UExponentialHeightFogComponent* Fog = NewObject<UExponentialHeightFogComponent>(EnvironmentActor, TEXT("Fog"), RF_Transient);
	Fog->SetupAttachment(Root);
	Fog->RegisterComponent();

	// Engine content, so it needs no plugin content and no cook dependency. Absent is survivable —
	// the floor only exists so shadows and depth-fade land on something.
	if (UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")))
	{
		// Sized to the FARTHEST configured distance, not to a fixed number. Systems measured at
		// 20,000 cm would otherwise hang in the void past the end of a default-sized floor, losing
		// the shadows and depth-fade the near-distance samples had — which is a difference in
		// rendering conditions across the distance axis, exactly the axis being compared.
		float FarthestDistance = 0.0f;
		for (const float Distance : Measurement.MeasureDistances)
		{
			FarthestDistance = FMath::Max(FarthestDistance, Distance);
		}

		// Engine's Plane is 100 units across, and the scale is a radius doubled, plus margin so the
		// horizon is never visible right behind the farthest population.
		const float CoverRadius = FarthestDistance + GridHalfExtent() + 2000.0f;
		const float PlaneScale = FMath::Max(1.0f, CoverRadius * 2.0f / 100.0f);

		UStaticMeshComponent* Floor = NewObject<UStaticMeshComponent>(EnvironmentActor, TEXT("Floor"), RF_Transient);
		Floor->SetStaticMesh(PlaneMesh);
		Floor->SetupAttachment(Root);
		Floor->RegisterComponent();

		// Centred under the locked view, and dropped below the lowest instance so the whole population
		// sits ON the floor rather than in or under it. With the view levelled above, every grid centre
		// is at eye height regardless of distance, so one Z works for every sample.
		// A short drop, not a large one: the bottom row should read as standing ON the platform. The
		// offset has to track GridHalfExtent rather than being a fixed eye height, because a tall grid
		// extends below the view and a fixed ground plane would cut through it.
		Floor->SetWorldLocation(FVector(
			LockedViewLocation.X,
			LockedViewLocation.Y,
			LockedViewLocation.Z - GridHalfExtent() - 100.0f));
		Floor->SetWorldScale3D(FVector(PlaneScale, PlaneScale, 1.0f));
	}
}

void FPulseNiagaraCostStage::Destroy()
{
	DespawnInstances();

#if WITH_PARTICLE_PERF_STATS
	if (Listener.IsValid())
	{
		FParticlePerfStatsManager::RemoveListener(Listener);
		Listener.Reset();
	}
#endif

	if (bQualityOverrideApplied)
	{
		FNiagaraPlatformSet::ClearNiagaraQualityLevelOverride();
		bQualityOverrideApplied = false;
	}

	if (bInputLocked)
	{
		if (APlayerController* Controller = ViewController.Get())
		{
			Controller->ResetIgnoreInputFlags();
		}
		bInputLocked = false;
	}

	if (EnvironmentActor != nullptr)
	{
		EnvironmentActor->Destroy();
		EnvironmentActor = nullptr;
	}
	if (StageActor != nullptr)
	{
		StageActor->Destroy();
		StageActor = nullptr;
	}

	// Only a world this stage created may be torn down. In PIE the world belongs to the session,
	// which the driver ends separately.
	if (World != nullptr)
	{
		if (Mode == EPulseNiagaraStageMode::PrivateWorld)
		{
			World->CleanupWorld();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		}
		World = nullptr;
	}
}

bool FPulseNiagaraCostStage::SetQualityLevel(int32 QualityLevel, FString& OutError)
{
	// SetNiagaraQualityLevelOverride invalidates the cached platform data and calls
	// RefreshScalability(), which walks every loaded system calling UpdateScalability(). That same
	// refresh is also wired as a console-variable sink, and sinks are pumped by an engine loop the
	// commandlet does not have — hence the explicit CallAllConsoleVariableSinks below, without which
	// systems can be left deactivated and measure as free.
	FNiagaraPlatformSet::SetNiagaraQualityLevelOverride(QualityLevel);
	IConsoleManager::Get().CallAllConsoleVariableSinks();
	bQualityOverrideApplied = true;

	const int32 ObservedQualityLevel = FNiagaraPlatformSet::GetQualityLevel();
	if (ObservedQualityLevel != QualityLevel)
	{
		OutError = FString::Printf(
			TEXT("Requested Niagara quality level %d but the engine reports %d. Measuring would silently produce identical results for every level."),
			QualityLevel, ObservedQualityLevel);
		return false;
	}
	return true;
}

FVector FPulseNiagaraCostStage::ResolveSpawnOrigin(float Distance) const
{
	if (Mode != EPulseNiagaraStageMode::PIEWorld)
	{
		return FVector::ZeroVector;
	}

	// The locked player view, not a fresh query: this is the same view Niagara measures its cull
	// distance against, and holding it fixed is what lets two distance samples be compared at all.
	// Straight down the forward axis, so the verdict is about distance rather than about which
	// corner an instance happened to land in.
	return LockedViewLocation + LockedViewRotation.Vector() * Distance;
}

FVector FPulseNiagaraCostStage::GridLocation(int32 Index) const
{
	// Square-ish, so a large instance count grows in both axes instead of becoming a long strip whose
	// far end sits at a completely different distance from the camera than its near end.
	const int32 Columns = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Measurement.InstancesPerSystem))));
	const int32 Rows = FMath::Max(1, FMath::DivideAndRoundUp(Measurement.InstancesPerSystem, Columns));

	const int32 Column = Index % Columns;
	const int32 Row = Index / Columns;

	// Local X stays zero and the stage actor faces the camera, so every instance sits at the SAME
	// distance from the view. That matters: Niagara culls per instance, so a grid laid out in depth
	// would have some instances inside the cull radius and some outside at the same nominal distance,
	// and the distance axis would measure the boundary rather than the effect.
	//
	// Centred on the view axis rather than growing from a corner, so the population stays in frame
	// instead of drifting off to one side as the instance count rises.
	return FVector(
		0.0,
		(Column - (Columns - 1) * 0.5) * Measurement.InstanceSpacing,
		(Row - (Rows - 1) * 0.5) * Measurement.InstanceSpacing);
}

float FPulseNiagaraCostStage::GridHalfExtent() const
{
	const int32 Columns = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Measurement.InstancesPerSystem))));
	const int32 Rows = FMath::Max(1, FMath::DivideAndRoundUp(Measurement.InstancesPerSystem, Columns));
	return FMath::Max(Columns - 1, Rows - 1) * Measurement.InstanceSpacing * 0.5f;
}

void FPulseNiagaraCostStage::SpawnInstances(UNiagaraSystem& System, float Distance)
{
	checkf(Components.Num() == 0, TEXT("SpawnInstances called with instances still on the stage"));

	// Positioned down the view axis and rotated to match it, so the grid built in local YZ becomes a
	// wall square-on to the camera: everything visible, everything equidistant.
	StageActor->SetActorLocationAndRotation(ResolveSpawnOrigin(Distance), LockedViewRotation);

	// A grid wider than the distance it sits at means the corner instances are much further away
	// than the centre ones, and a cull boundary can fall inside the population. Worth one line rather
	// than a distance column that quietly stops separating.
	if (Distance > GPulseNiagaraNoDistance && GridHalfExtent() > Distance * 0.5f)
	{
		UE_LOG(LogPulse, Warning,
			TEXT("Instance grid half-extent %.0f cm is large next to the %.0f cm measurement distance; corner instances sit noticeably further out. Reduce InstanceSpacing or InstancesPerSystem for near distances."),
			GridHalfExtent(), Distance);
	}

	for (int32 Index = 0; Index < Measurement.InstancesPerSystem; ++Index)
	{
		UNiagaraComponent* Component = NewObject<UNiagaraComponent>(StageActor, NAME_None, RF_Transient);
		Component->SetAsset(&System);

		// Auto-destroy would remove a one-shot system mid-window and turn a finished effect into a
		// shrinking population rather than a recorded fact; InstancesActiveAtEnd reports that instead.
		Component->SetAutoDestroy(false);
		Component->bAutoActivate = false;

		// Registration with the scalability manager is what performs distance and significance
		// culling, and it must be off in a private world. There, FNiagaraWorldManager can cache no
		// views — they come only from a local PlayerController with a real viewport or from an editor
		// level viewport — so the closest distance stays FLT_MAX and EVERY system whose effect type
		// sets bCullByDistance would cull on its first tick and measure as very nearly free.
		//
		// That is the worst failure this tool could have: it reads as "this effect is cheap", and it
		// strikes precisely the well-authored content, since a system needs an effect type to be
		// culled at all. Doing it per component rather than through
		// FNiagaraWorldManager::SetScalabilityCullingMode leaves no global state to restore, and
		// avoids GetScalabilityCullingMode() — an inline accessor over a static the Niagara module
		// does not export, so reading it fails to link from outside.
		//
		// Quality levels are unaffected either way: emitter enablement is decided on UNiagaraSystem
		// through the platform set, not by the component's manager registration.
		//
		// No preview LOD distance is set either. FNiagaraWorldManager recomputes the distance from
		// its cached views and calls Component->SetLODDistance itself every tick, so a preview value
		// would simply be overwritten.
		Component->SetAllowScalability(IsCullingExercised());

		Component->SetupAttachment(StageActor->GetRootComponent());
		Component->RegisterComponent();
		Component->SetRelativeLocation(GridLocation(Index));
		Component->Activate(/*bReset*/ true);

		Components.Add(Component);
	}
}

void FPulseNiagaraCostStage::DespawnInstances()
{
	for (TObjectPtr<UNiagaraComponent>& Component : Components)
	{
		if (Component != nullptr)
		{
			Component->Deactivate();
			Component->DestroyComponent();
		}
	}
	Components.Reset();
}

void FPulseNiagaraCostStage::PumpFrame()
{
	++GFrameCounter;
	++GFrameNumber;

	// OnBeginFrame is what drives FParticlePerfStatsManager::Tick — it is bound there in
	// FParticlePerfStatsManager::OnStartup. Without this broadcast the listener never accumulates
	// and every system measures as zero.
	FCoreDelegates::OnBeginFrame.Broadcast();

	World->Tick(LEVELTICK_All, Measurement.FixedDeltaSeconds);

	// Niagara defers render-state work to the end-of-frame flush and hooks
	// OnWorldPreSendAllEndOfFrameUpdates; with no viewport nothing else will call this.
	World->SendAllEndOfFrameUpdates();

	FTSTicker::GetCoreTicker().Tick(Measurement.FixedDeltaSeconds);
	FCoreDelegates::OnEndFrame.Broadcast();
}

double FPulseNiagaraCostStage::GetLiveAverageMs() const
{
	// Over every frame recorded so far, matching how the finished sample computes its average — a
	// live figure that used a different rule than the one about to be written would be worse than
	// none, because the reader would watch it settle on a number the report then contradicts.
	if (FrameMilliseconds.Num() == 0)
	{
		return 0.0;
	}

	double SumMilliseconds = 0.0;
	for (const double FrameCost : FrameMilliseconds)
	{
		SumMilliseconds += FrameCost;
	}
	return SumMilliseconds / FrameMilliseconds.Num();
}

bool FPulseNiagaraCostStage::HasTimedOut() const
{
	return FPlatformTime::Seconds() - MeasurementStartSeconds > Measurement.SecondsPerSystemTimeout;
}

void FPulseNiagaraCostStage::BeginMeasurement(UNiagaraSystem& System, int32 QualityLevel, float Distance)
{
	checkf(!bMeasuring, TEXT("BeginMeasurement called while a measurement was already running"));

	MeasuredSystem = &System;
	MeasuredQualityLevel = QualityLevel;
	MeasuredDistance = Distance;
	MeasurementStartSeconds = FPlatformTime::Seconds();
	SettleFramesRemaining = Measurement.SettleFrames;
	MeasureFramesRemaining = Measurement.MeasureFrames;
	PreviousTotalCycles = 0;
	FrameMilliseconds.Reset();
	FrameMilliseconds.Reserve(Measurement.MeasureFrames);
	PendingSkipReason.Reset();
	bStatsReset = false;
	bMeasuring = true;

	SpawnInstances(System, Distance);
	if (Components.Num() == 0)
	{
		PendingSkipReason = TEXT("No instances could be spawned.");
		SettleFramesRemaining = 0;
		MeasureFramesRemaining = 0;
	}
}

bool FPulseNiagaraCostStage::TickMeasurement()
{
#if !WITH_PER_SYSTEM_PARTICLE_PERF_STATS
	PendingSkipReason = TEXT("Per-system particle perf stats are compiled out of this build.");
	return false;
#else
	if (!bMeasuring || !PendingSkipReason.IsEmpty())
	{
		return false;
	}

	UNiagaraSystem* System = MeasuredSystem.Get();
	if (System == nullptr)
	{
		PendingSkipReason = TEXT("The system was garbage collected mid-measurement.");
		return false;
	}

	if (Mode == EPulseNiagaraStageMode::PrivateWorld)
	{
		PumpFrame();
	}

	if (HasTimedOut())
	{
		if (FrameMilliseconds.Num() == 0)
		{
			PendingSkipReason = FString::Printf(TEXT("Timed out after %.1fs before any frame was recorded."),
				Measurement.SecondsPerSystemTimeout);
		}
		return false;
	}

	// Settle. Spawn, allocation and first-tick costs land here and are discarded wholesale by the
	// ResetGT below rather than averaged into the result.
	if (SettleFramesRemaining > 0)
	{
		--SettleFramesRemaining;
		return true;
	}

	FAccumulatedParticlePerfStats* Stats = Listener->GetStats(System);
	if (Stats == nullptr)
	{
		PendingSkipReason = TEXT("No perf stats were recorded for this system; it never ticked.");
		return false;
	}

	if (!bStatsReset)
	{
		Stats->ResetGT();
		PreviousTotalCycles = 0;
		bStatsReset = true;

		// The reset lands between frames, so this frame's accumulated total is not yet meaningful.
		// Returning here costs one frame and avoids booking a bogus first sample.
		return MeasureFramesRemaining > 0;
	}

	// The engine exposes a running average and a max but no minimum, so the per-frame series is
	// rebuilt here by differencing the accumulated cycle total. It also keeps avg, min and max
	// consistent with one another, which a mix of engine-averaged and locally-derived values
	// would not be.
	const uint64 TotalCycles = Stats->GetGameThreadStats().GetTotalCycles();
	FrameMilliseconds.Add(PulseCyclesToMilliseconds(TotalCycles - PreviousTotalCycles));
	PreviousTotalCycles = TotalCycles;

	--MeasureFramesRemaining;
	return MeasureFramesRemaining > 0;
#endif
}

FPulseNiagaraCostSample FPulseNiagaraCostStage::EndMeasurement()
{
	FPulseNiagaraCostSample Sample;
	Sample.QualityLevel = MeasuredQualityLevel;
	Sample.Distance = MeasuredDistance;
	Sample.InstancesSpawned = Components.Num();

	for (const TObjectPtr<UNiagaraComponent>& Component : Components)
	{
		if (Component != nullptr && Component->IsActive())
		{
			Sample.InstancesActiveAtEnd++;
		}
	}

	DespawnInstances();
	bMeasuring = false;

	if (!PendingSkipReason.IsEmpty())
	{
		Sample.SkipReason = PendingSkipReason;
		return Sample;
	}
	if (FrameMilliseconds.Num() == 0)
	{
		Sample.SkipReason = TEXT("No frames were recorded.");
		return Sample;
	}

	// Frames where the system booked no cycles at all are counted, not averaged into min.
	//
	// Niagara ticks much of its work concurrently, and cycles from an async task that finishes after
	// the perf-stats manager has already rolled the frame land in the NEXT frame's bucket. The series
	// therefore contains genuine zero frames followed by double-weight ones. The sum over the window
	// is still exactly right, so the average is unaffected — but a minimum taken over the raw series
	// reports 0.000 and reads as "this effect is sometimes free", which it never is.
	//
	// Min and max are therefore taken over frames that did work, and the zeros are reported as
	// IdleFrames rather than hidden: a high idle count means the sampling did not line up with the
	// system's tick cadence, and the reader should trust the average over the spread.
	double SumMilliseconds = 0.0;
	double MinMilliseconds = TNumericLimits<double>::Max();
	double MaxMilliseconds = 0.0;
	int32 IdleFrames = 0;
	for (const double FrameCost : FrameMilliseconds)
	{
		SumMilliseconds += FrameCost;
		if (FrameCost <= 0.0)
		{
			IdleFrames++;
			continue;
		}
		MinMilliseconds = FMath::Min(MinMilliseconds, FrameCost);
		MaxMilliseconds = FMath::Max(MaxMilliseconds, FrameCost);
	}

	Sample.FramesRecorded = FrameMilliseconds.Num();
	Sample.IdleFrames = IdleFrames;
	Sample.TotalAvgMs = SumMilliseconds / FrameMilliseconds.Num();

	// Every frame idle means the system never did any work in the window at all. That is a finding,
	// not a zero-cost measurement, so it is reported as unmeasured.
	if (IdleFrames == FrameMilliseconds.Num())
	{
		Sample.SkipReason = TEXT("The system booked no work in any measured frame; it never ticked.");
		return Sample;
	}

	Sample.TotalMinMs = MinMilliseconds;
	Sample.TotalMaxMs = MaxMilliseconds;

	const double InstanceDivisor = FMath::Max(1, Sample.InstancesSpawned);
	Sample.PerInstanceAvgMs = Sample.TotalAvgMs / InstanceDivisor;
	Sample.PerInstanceMinMs = Sample.TotalMinMs / InstanceDivisor;
	Sample.PerInstanceMaxMs = Sample.TotalMaxMs / InstanceDivisor;
	Sample.bMeasured = true;

	return Sample;
}

#undef LOCTEXT_NAMESPACE
