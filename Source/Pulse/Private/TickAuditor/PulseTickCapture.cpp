// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseTickCapture.h"

#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Pulse.h"
#include "PulseTickAuditorSettings.h"

/**
 * One per tick group in use. Registered in that group, it runs the group's disabled ticks itself so
 * each can be timed, which keeps every tick in the part of the frame it was written for.
 */
struct FPulseTickProbe : public FTickFunction
{
	FPulseTickCapture* Owner = nullptr;

	virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent) override
	{
		if (Owner != nullptr)
		{
			Owner->RunGroup(TickGroup, DeltaTime, TickType);
		}
	}

	virtual FString DiagnosticMessage() override
	{
		return TEXT("PulseTickProbe");
	}
};

static double PulseCyclesToMs(uint64 Cycles)
{
	return static_cast<double>(Cycles) * FPlatformTime::GetSecondsPerCycle64() * 1000.0;
}

FPulseTickCapture::FPulseTickCapture() = default;

FPulseTickCapture::~FPulseTickCapture()
{
	// No broadcast from a destructor: whoever is listening may be what is being destroyed.
	EndCapture();
	FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
	FWorldDelegates::OnWorldCleanup.Remove(EndPlayHandle);
}

bool FPulseTickCapture::Start(UWorld* InWorld, int32 InFramesToCapture, FString& OutError)
{
	if (IsRunning())
	{
		OutError = TEXT("A capture is already running.");
		return false;
	}
	if (InWorld == nullptr || !InWorld->IsGameWorld())
	{
		OutError = TEXT("Start Play In Editor first: the tick auditor measures a running game.");
		return false;
	}

	World = InWorld;
	FramesToCapture = FMath::Max(InFramesToCapture, 1);
	FramesCompleted = 0;
	Targets.Reset();
	Instances.Reset();
	bFrameOpen = false;
	bArmed = true;

	WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddRaw(this, &FPulseTickCapture::OnWorldTickStart);

	// Cleanup rather than EndPIE: the level still exists when it fires, so the probes can be
	// unregistered from it properly instead of being left for a dead tick task manager.
	EndPlayHandle = FWorldDelegates::OnWorldCleanup.AddLambda([this](UWorld* CleanedWorld, bool, bool)
	{
		if (CleanedWorld == World.Get())
		{
			OnEndPlay(false);
		}
	});
	return true;
}

void FPulseTickCapture::Stop()
{
	if (!IsRunning())
	{
		return;
	}
	bArmed = false;
	EndCapture();
	OnFinished.Broadcast();
}

void FPulseTickCapture::OnEndPlay(bool bIsSimulating)
{
	if (IsRunning())
	{
		UE_LOG(LogPulse, Warning, TEXT("Pulse.Tick: the play session ended mid-capture; keeping the %d frame(s) already captured."), FramesCompleted);
		Stop();
	}
}

void FPulseTickCapture::OnWorldTickStart(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds)
{
	if (InWorld != World.Get())
	{
		return;
	}

	if (bArmed)
	{
		// Begin at a frame boundary: started from a button press mid-frame, some groups would already
		// have run and the first frame would be partial.
		bArmed = false;
		BeginCapture();
		return;
	}

	if (!bCapturing)
	{
		return;
	}

	if (bFrameOpen)
	{
		CloseFrame();
		FramesCompleted++;
	}

	if (FramesCompleted >= FramesToCapture)
	{
		EndCapture();
		OnFinished.Broadcast();
	}
}

void FPulseTickCapture::AddTarget(FTickFunction& TickFunction, AActor& Actor, UActorComponent* Component)
{
	// One instance record per actor, created on its first ticking function.
	int32 InstanceIndex = INDEX_NONE;
	if (Targets.Num() > 0 && Targets.Last().Actor.Get() == &Actor)
	{
		InstanceIndex = Targets.Last().InstanceIndex;
	}
	else
	{
		InstanceIndex = Instances.AddDefaulted();
		Instances[InstanceIndex].Class = Actor.GetClass();
	}

	FTarget& Target = Targets.AddDefaulted_GetRef();
	Target.Actor = &Actor;
	Target.Component = Component;
	Target.TickFunction = &TickFunction;
	Target.Group = TickFunction.TickGroup;
	Target.Interval = TickFunction.TickInterval;
	Target.InstanceIndex = InstanceIndex;
	if (Component != nullptr)
	{
		Target.ComponentClass = Component->GetClass()->GetName();
	}

	// Disabled at the tick function rather than through SetActorTickEnabled/SetComponentTickEnabled,
	// which are virtual and can carry game-side reactions to "tick turned off" that a measurement must
	// not trigger.
	TickFunction.SetTickFunctionEnable(false);
}

void FPulseTickCapture::BeginCapture()
{
	UWorld* CaptureWorld = World.Get();
	if (CaptureWorld == nullptr || CaptureWorld->PersistentLevel == nullptr)
	{
		return;
	}

	for (TActorIterator<AActor> It(CaptureWorld); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || Actor->IsActorBeingDestroyed())
		{
			continue;
		}

		FTickFunction& ActorTick = Actor->PrimaryActorTick;
		if (ActorTick.bCanEverTick && ActorTick.IsTickFunctionRegistered() && ActorTick.IsTickFunctionEnabled())
		{
			AddTarget(ActorTick, *Actor, nullptr);
		}

		TInlineComponentArray<UActorComponent*> Components(Actor);
		for (UActorComponent* Component : Components)
		{
			if (!IsValid(Component) || !Component->IsRegistered())
			{
				continue;
			}
			FTickFunction& ComponentTick = Component->PrimaryComponentTick;
			if (ComponentTick.bCanEverTick && ComponentTick.IsTickFunctionRegistered() && ComponentTick.IsTickFunctionEnabled())
			{
				AddTarget(ComponentTick, *Actor, Component);
			}
		}
	}

	TArray<ETickingGroup> Groups;
	for (const FTarget& Target : Targets)
	{
		Groups.AddUnique(Target.Group);
	}
	for (const ETickingGroup Group : Groups)
	{
		TUniquePtr<FPulseTickProbe> Probe = MakeUnique<FPulseTickProbe>();
		Probe->Owner = this;
		Probe->TickGroup = Group;
		Probe->bCanEverTick = true;
		Probe->bStartWithTickEnabled = true;
		Probe->RegisterTickFunction(CaptureWorld->PersistentLevel);
		Probe->SetTickFunctionEnable(true);
		Probes.Add(MoveTemp(Probe));
	}

	bCapturing = true;
	bFrameOpen = false;
	CaptureStartSeconds = FPlatformTime::Seconds();

	UE_LOG(LogPulse, Display, TEXT("Pulse.Tick: capturing %d tick function(s) on %d actor(s) for %d frame(s)."),
		Targets.Num(), Instances.Num(), FramesToCapture);
}

bool FPulseTickCapture::IsTargetAlive(const FTarget& Target) const
{
	const AActor* Actor = Target.Actor.Get();
	if (Actor == nullptr || Actor->IsActorBeingDestroyed())
	{
		return false;
	}
	if (Target.Component.IsExplicitlyNull())
	{
		return Target.TickFunction->IsTickFunctionRegistered();
	}
	const UActorComponent* Component = Target.Component.Get();
	return Component != nullptr && Component->IsRegistered() && Target.TickFunction->IsTickFunctionRegistered();
}

void FPulseTickCapture::RunGroup(ETickingGroup Group, float DeltaTime, ELevelTick TickType)
{
	if (!bCapturing)
	{
		return;
	}
	bFrameOpen = true;

	for (FTarget& Target : Targets)
	{
		if (Target.Group != Group || Target.bReleased)
		{
			continue;
		}
		if (!IsTargetAlive(Target))
		{
			Target.bReleased = true;
			continue;
		}

		// Re-enabled by game code: the engine is ticking it again, so driving it as well would tick it
		// twice. Let it go; it stays enabled afterwards, as the game asked.
		if (Target.TickFunction->IsTickFunctionEnabled())
		{
			Target.bReleased = true;
			continue;
		}

		// Intervals honoured the way the engine does: tick once the interval has elapsed, with the
		// whole elapsed time as the delta.
		float TickDelta = DeltaTime;
		if (Target.Interval > 0.0f)
		{
			Target.AccumulatedDelta += DeltaTime;
			if (Target.AccumulatedDelta < Target.Interval)
			{
				continue;
			}
			TickDelta = Target.AccumulatedDelta;
			Target.AccumulatedDelta = 0.0f;
		}

		const int32 InstanceIndex = Target.InstanceIndex;
		const uint64 StartCycles = FPlatformTime::Cycles64();
		Target.TickFunction->ExecuteTick(TickDelta, TickType, ENamedThreads::GameThread, FGraphEventRef());
		const uint64 Elapsed = FPlatformTime::Cycles64() - StartCycles;

		// Targets and Instances only change in BeginCapture, never during a tick, so the reference held
		// across ExecuteTick stays valid whatever the tick does.
		Target.TotalCycles += Elapsed;
		Target.Calls++;
		Instances[InstanceIndex].FrameCycles += Elapsed;
	}
}

void FPulseTickCapture::CloseFrame()
{
	for (FInstance& Instance : Instances)
	{
		Instance.WorstFrameMs = FMath::Max(Instance.WorstFrameMs, PulseCyclesToMs(Instance.FrameCycles));
		Instance.FrameCycles = 0;
	}
	bFrameOpen = false;
}

void FPulseTickCapture::EndCapture()
{
	for (TUniquePtr<FPulseTickProbe>& Probe : Probes)
	{
		Probe->Owner = nullptr;
		if (Probe->IsTickFunctionRegistered())
		{
			Probe->UnRegisterTickFunction();
		}
	}
	Probes.Reset();

	if (bCapturing)
	{
		int32 NumRestored = 0;
		for (FTarget& Target : Targets)
		{
			if (!Target.bReleased && IsTargetAlive(Target))
			{
				Target.TickFunction->SetTickFunctionEnable(true);
				NumRestored++;
			}
			Target.bReleased = true;
		}
		bCapturing = false;
		CaptureEndSeconds = FPlatformTime::Seconds();
		UE_LOG(LogPulse, Display, TEXT("Pulse.Tick: capture ended after %d frame(s); %d tick function(s) handed back to the engine."),
			FramesCompleted, NumRestored);
	}

	FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
	WorldTickStartHandle.Reset();
	FWorldDelegates::OnWorldCleanup.Remove(EndPlayHandle);
	EndPlayHandle.Reset();
}

FPulseTickReport FPulseTickCapture::BuildReport(const UPulseTickAuditorSettings& Settings) const
{
	FPulseTickReport Report;
	Report.GeneratedAtUtc = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	Report.EngineVersion = FEngineVersion::Current().ToString();
	Report.MachineName = FPlatformProcess::ComputerName();
	if (const UWorld* CaptureWorld = World.Get())
	{
		Report.WorldName = UWorld::RemovePIEPrefix(CaptureWorld->GetOutermost()->GetName());
	}
	Report.FramesCaptured = FramesCompleted;
	Report.CapturedSeconds = (bCapturing ? FPlatformTime::Seconds() : CaptureEndSeconds) - CaptureStartSeconds;
	Report.NumTickFunctions = Targets.Num();
	Report.FrameBudgetMs = Settings.GetFrameBudgetMs();
	Report.MaxFrameBudgetPercentPerClass = Settings.MaxFrameBudgetPercentPerClass;
	Report.MaxFrameBudgetPercentPerInstance = Settings.MaxFrameBudgetPercentPerInstance;
	Report.IdleTickMicroseconds = Settings.IdleTickMicroseconds;

	if (FramesCompleted <= 0)
	{
		return Report;
	}
	const double Frames = static_cast<double>(FramesCompleted);

	struct FClassAccumulator
	{
		FPulseTickClassRow Row;
		TMap<FString, FPulseTickComponentCost> Components;
		uint64 ActorCycles = 0;
		int32 ActorCalls = 0;
		TSet<int32> InstanceIndices;
	};
	TMap<UClass*, FClassAccumulator> ByClass;

	for (const FTarget& Target : Targets)
	{
		UClass* Class = Instances[Target.InstanceIndex].Class.Get();
		if (Class == nullptr)
		{
			continue;
		}
		FClassAccumulator& Accumulator = ByClass.FindOrAdd(Class);
		Accumulator.InstanceIndices.Add(Target.InstanceIndex);

		const double MsPerFrame = PulseCyclesToMs(Target.TotalCycles) / Frames;
		Accumulator.Row.TotalMsPerFrame += MsPerFrame;
		Accumulator.Row.bTicksEveryFrame |= Target.Interval <= 0.0f;
		Report.NumTargetsLost += (Target.bReleased && Target.Calls == 0) ? 1 : 0;

		if (Target.ComponentClass.IsEmpty())
		{
			Accumulator.Row.NumActorTicks++;
			Accumulator.Row.ActorMsPerFrame += MsPerFrame;
			Accumulator.ActorCycles += Target.TotalCycles;
			Accumulator.ActorCalls += Target.Calls;
			Accumulator.Row.MinActorTickInterval = Accumulator.Row.NumActorTicks == 1
				? Target.Interval
				: FMath::Min(Accumulator.Row.MinActorTickInterval, Target.Interval);
		}
		else
		{
			Accumulator.Row.NumComponentTicks++;
			FPulseTickComponentCost& Cost = Accumulator.Components.FindOrAdd(Target.ComponentClass);
			Cost.MinTickInterval = Cost.Count == 0 ? Target.Interval : FMath::Min(Cost.MinTickInterval, Target.Interval);
			Cost.ClassName = Target.ComponentClass;
			Cost.Count++;
			Cost.MsPerFrame += MsPerFrame;
		}
	}

	const double ClassBudgetMs = Report.FrameBudgetMs * Report.MaxFrameBudgetPercentPerClass / 100.0;
	const double InstanceBudgetMs = Report.FrameBudgetMs * Report.MaxFrameBudgetPercentPerInstance / 100.0;

	for (TPair<UClass*, FClassAccumulator>& Pair : ByClass)
	{
		UClass* Class = Pair.Key;
		FClassAccumulator& Accumulator = Pair.Value;
		FPulseTickClassRow& Row = Accumulator.Row;

		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
		{
			Row.ClassName = Blueprint->GetName();
			Row.AssetPath = Blueprint->GetPathName();
		}
		else
		{
			Row.ClassName = Class->GetName();
		}
		for (const UClass* Ancestor = Class; Ancestor != nullptr; Ancestor = Ancestor->GetSuperClass())
		{
			if (Ancestor->HasAnyClassFlags(CLASS_Native))
			{
				Row.NativeParent = Ancestor->GetName();
				break;
			}
		}

		Row.bHasEventTick = Class->IsFunctionImplementedInScript(FName(TEXT("ReceiveTick")));
		Row.NumInstances = Accumulator.InstanceIndices.Num();
		Row.PerInstanceMsPerFrame = Row.NumInstances > 0 ? Row.TotalMsPerFrame / Row.NumInstances : 0.0;
		for (const int32 InstanceIndex : Accumulator.InstanceIndices)
		{
			Row.WorstInstanceFrameMs = FMath::Max(Row.WorstInstanceFrameMs, Instances[InstanceIndex].WorstFrameMs);
		}

		Accumulator.Components.GenerateValueArray(Row.Components);
		Row.Components.Sort([](const FPulseTickComponentCost& A, const FPulseTickComponentCost& B)
		{
			return A.MsPerFrame != B.MsPerFrame ? A.MsPerFrame > B.MsPerFrame : A.ClassName < B.ClassName;
		});

		// Idle: the actor tick is enabled, runs, and does next to nothing per call, with no Event Tick
		// to explain it. Usually a Blueprint that kept the default "Start with Tick Enabled".
		if (Row.NumActorTicks > 0 && Accumulator.ActorCalls > 0 && !Row.bHasEventTick)
		{
			const double MicrosecondsPerCall = PulseCyclesToMs(Accumulator.ActorCycles) * 1000.0 / Accumulator.ActorCalls;
			Row.bIdleActorTick = MicrosecondsPerCall < Report.IdleTickMicroseconds;
		}

		Row.bOverClassBudget = ClassBudgetMs > 0.0 && Row.TotalMsPerFrame > ClassBudgetMs;
		Row.bOverInstanceBudget = InstanceBudgetMs > 0.0 && Row.PerInstanceMsPerFrame > InstanceBudgetMs;

		Report.TotalMsPerFrame += Row.TotalMsPerFrame;
		Report.Rows.Add(MoveTemp(Row));
	}

	Report.Rows.Sort([](const FPulseTickClassRow& A, const FPulseTickClassRow& B)
	{
		return A.TotalMsPerFrame != B.TotalMsPerFrame ? A.TotalMsPerFrame > B.TotalMsPerFrame : A.ClassName < B.ClassName;
	});
	return Report;
}
