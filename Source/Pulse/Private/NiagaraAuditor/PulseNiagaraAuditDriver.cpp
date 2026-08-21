// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseNiagaraAuditDriver.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "PlayInEditorDataTypes.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "NiagaraPlatformSet.h"
#include "NiagaraSystem.h"
#include "Pulse.h"
#include "PulseNiagaraAuditorSettings.h"
#include "PulseTagUtils.h"
#include "UObject/UObjectGlobals.h"

FPulseNiagaraAuditDriver::FPulseNiagaraAuditDriver(const FPulseNiagaraAuditorConfig& InConfig,
	const UPulseNiagaraAuditorSettings& InSettings, EPulseNiagaraStageMode InMode)
	: Config(InConfig)
	, Settings(InSettings)
{
	Stage = MakeUnique<FPulseNiagaraCostStage>(Config.Measurement, InMode);
}

FPulseNiagaraAuditDriver::~FPulseNiagaraAuditDriver() = default;

bool FPulseNiagaraAuditDriver::Initialize(FString& OutError)
{
	checkf(!bInitialized, TEXT("FPulseNiagaraAuditDriver::Initialize called twice"));

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// A commandlet starts with an unscanned registry, so every tag read would come back absent and
	// every system would look like it needed resaving. In the editor this is already done and
	// returns immediately.
	AssetRegistry.SearchAllAssets(/*bSynchronousSearch*/ true);

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
	for (const FString& Path : Config.PathFilter)
	{
		Filter.PackagePaths.Add(FName(*Path));
	}
	AssetRegistry.GetAssets(Filter, Assets);

	Assets.RemoveAll([this](const FAssetData& AssetData)
	{
		const FString PackagePath = AssetData.PackageName.ToString();
		for (const FString& Pattern : Settings.Scan.ExcludePathPatterns)
		{
			if (!Pattern.IsEmpty() && PackagePath.Contains(Pattern))
			{
				return true;
			}
		}
		return false;
	});

	// Sorted so the row order of a report depends on the content, not on registry iteration order.
	Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
	{
		return Left.GetObjectPathString() < Right.GetObjectPathString();
	});

	if (Config.MaxSystems > 0 && Assets.Num() > Config.MaxSystems)
	{
		Assets.SetNum(Config.MaxSystems);
	}

	// Distance is only meaningful where Niagara has a view to measure against. Outside the editor
	// the stage pauses culling entirely, so the axis collapses to a single distance-less point
	// rather than pretending to sweep something that has no effect.
	TArray<float> Distances;
	if (Stage->IsCullingExercised() && Config.Measurement.MeasureDistances.Num() > 0)
	{
		Distances = Config.Measurement.MeasureDistances;
		Distances.Sort();
	}
	else
	{
		Distances.Add(GPulseNiagaraNoDistance);
	}

	// Quality outer, distance inner: SetQualityLevel walks every loaded system, so changing it as
	// rarely as possible keeps that cost off the measurement path.
	for (const int32 QualityLevel : Config.Measurement.QualityLevelsToMeasure)
	{
		for (const float Distance : Distances)
		{
			AxisPoints.Add({ QualityLevel, Distance });
		}
	}

	Report.GeneratedAtUtc = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	Report.EngineVersion = FEngineVersion::Current().ToString();
	Report.MachineName = FPlatformProcess::ComputerName();
	Report.RHIName = GDynamicRHI != nullptr ? GDynamicRHI->GetName() : TEXT("None");
	Report.bRenderingAvailable = FApp::CanEverRender();
	Report.bScalabilityCullingExercised = Stage->IsCullingExercised();
	Report.bMeasuredInPlayWorld = Stage->IsCullingExercised();
	Report.Measurement = Config.Measurement;
	Report.FrameBudgetMs = Config.FrameBudgetMs;
	Report.QualityLevels = Config.Measurement.QualityLevelsToMeasure;
	Report.Distances = Distances;
	Report.NumSystemsDiscovered = Assets.Num();

	for (const int32 QualityLevel : Report.QualityLevels)
	{
		Report.QualityLevelNames.Add(FNiagaraPlatformSet::GetQualityLevelText(QualityLevel).ToString());
	}

	// The stage is NOT created here for PIE runs. RequestPlaySession is asynchronous — the play
	// world does not exist until a tick or two later — so stage creation moves into Step, which
	// waits for it. Private-world runs have nothing to wait for and start immediately.
	if (Stage->IsCullingExercised())
	{
		RequestPlaySession();
	}
	else if (!TryCreateStage())
	{
		OutError = FatalError;
		return false;
	}

	bInitialized = true;
	return true;
}

void FPulseNiagaraAuditDriver::RequestPlaySession()
{
	FRequestPlaySessionParams Params;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;

	// PlayInEditor, never SimulateInEditor: simulate skips spawning the player controller, and the
	// player controller is the entire reason for running in PIE — it is what gives Niagara a view to
	// cull against.
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	if (const TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport())
	{
		Params.DestinationSlateViewport = ActiveViewport;
	}

	// Start where the user was looking. Without this PIE uses a PlayerStart, or the origin when the
	// map has none — and a bare test map usually has none, which would put every measurement at a
	// spot nobody chose.
	if (GCurrentLevelEditingViewportClient != nullptr)
	{
		Params.StartLocation = GCurrentLevelEditingViewportClient->GetViewLocation();
		Params.StartRotation = GCurrentLevelEditingViewportClient->GetViewRotation();
	}

	GEditor->RequestPlaySession(Params);
	bPlaySessionRequested = true;
	PlaySessionWaitStartSeconds = FPlatformTime::Seconds();

	UE_LOG(LogPulse, Display, TEXT("Starting a PIE session to measure in; the player controller will be put into spectator state."));
}

bool FPulseNiagaraAuditDriver::TryCreateStage()
{
	if (bStageCreated)
	{
		return true;
	}

	FString Error;
	if (!Stage->Create(Error))
	{
		// For a PIE run this is expected until the world exists, so it is only fatal once waiting has
		// clearly failed — otherwise a slow map load would abort the run it was about to enable.
		if (Stage->IsCullingExercised())
		{
			const double WaitedSeconds = FPlatformTime::Seconds() - PlaySessionWaitStartSeconds;
			if (WaitedSeconds < 30.0)
			{
				return false;
			}
			FatalError = FString::Printf(TEXT("Timed out after %.0fs waiting for the PIE session: %s"), WaitedSeconds, *Error);
			return false;
		}

		FatalError = Error;
		return false;
	}

	bStageCreated = true;
	return true;
}

bool FPulseNiagaraAuditDriver::IsComplete() const
{
	if (bCancelled || !FatalError.IsEmpty())
	{
		return true;
	}

	// Not complete while the stage is still coming up, or an empty-looking asset list would finish
	// the run before PIE had even started.
	if (!bInitialized || !bStageCreated)
	{
		return false;
	}
	return CurrentAssetIndex >= Assets.Num() && !Stage->IsMeasuring();
}

void FPulseNiagaraAuditDriver::Cancel()
{
	bCancelled = true;
}

FPulseNiagaraAuditProgress FPulseNiagaraAuditDriver::GetProgress() const
{
	FPulseNiagaraAuditProgress Progress;
	Progress.SamplesCompleted = SamplesCompleted;
	Progress.SamplesTotal = Assets.Num() * AxisPoints.Num();
	Progress.CurrentSystemName = CurrentRow.AssetName;

	if (AxisPoints.IsValidIndex(CurrentAxisIndex))
	{
		const int32 QualityIndex = Report.QualityLevels.Find(AxisPoints[CurrentAxisIndex].QualityLevel);
		Progress.CurrentQualityName = Report.QualityLevelNames.IsValidIndex(QualityIndex)
			? Report.QualityLevelNames[QualityIndex]
			: FString::FromInt(AxisPoints[CurrentAxisIndex].QualityLevel);
		Progress.CurrentDistance = AxisPoints[CurrentAxisIndex].Distance;
	}

	Progress.bMeasuring = Stage->IsMeasuring();
	Progress.LiveAvgMs = Stage->GetLiveAverageMs();
	Progress.LiveFramesRecorded = Stage->GetLiveFramesRecorded();
	Progress.LiveInstanceCount = Stage->GetLiveInstanceCount();
	Progress.MeasureFrames = Config.Measurement.MeasureFrames;
	return Progress;
}

bool FPulseNiagaraAuditDriver::BeginNextSystem()
{
	// Flush the row the previous system built before moving on.
	if (CurrentAssetIndex >= 0 && CurrentAssetIndex < Assets.Num())
	{
		if (bRowMeasuredAnySample)
		{
			Report.NumSystemsMeasured++;
		}
		else
		{
			Report.NumSystemsSkipped++;
		}
		Report.Rows.Add(MoveTemp(CurrentRow));
		CurrentRow = FPulseNiagaraCostRow();

		CurrentSystem.Reset();
		if (++SystemsSinceCollect >= Config.Measurement.GCSystemInterval)
		{
			CollectGarbage(RF_NoFlags);
			SystemsSinceCollect = 0;
		}
	}

	++CurrentAssetIndex;
	CurrentAxisIndex = 0;
	bRowMeasuredAnySample = false;

	if (CurrentAssetIndex >= Assets.Num())
	{
		return false;
	}

	const FAssetData& AssetData = Assets[CurrentAssetIndex];
	CurrentRow.AssetPath = AssetData.GetObjectPathString();
	CurrentRow.AssetName = AssetData.AssetName.ToString();

	int64 NumEmitters = 0;
	if (PulseGetTagInt(AssetData, FName(TEXT("NumEmitters")), NumEmitters))
	{
		CurrentRow.NumEmitters = static_cast<int32>(NumEmitters);
	}
	bool bHasGPUEmitter = false;
	if (PulseGetTagBool(AssetData, FName(TEXT("HasGPUEmitter")), bHasGPUEmitter))
	{
		CurrentRow.bHasGPUEmitter = bHasGPUEmitter;
	}
	FString EffectType;
	if (PulseGetTagString(AssetData, FName(TEXT("EffectType")), EffectType))
	{
		CurrentRow.EffectType = EffectType;
	}

	auto SkipWholeRow = [this](const FString& Reason)
	{
		for (const FPulseNiagaraAxisPoint& Point : AxisPoints)
		{
			FPulseNiagaraCostSample& Sample = CurrentRow.Samples.AddDefaulted_GetRef();
			Sample.QualityLevel = Point.QualityLevel;
			Sample.Distance = Point.Distance;
			Sample.SkipReason = Reason;
			SamplesCompleted++;
		}
		CurrentAxisIndex = AxisPoints.Num();
	};

	// Decided from the registry tag, before any load. A GPU system measured without a scene render
	// never dispatches its simulation and would report as nearly free — a wrong number rather than a
	// missing one. In the editor world the viewport does render, so the skip does not apply.
	if (Config.Measurement.bSkipGPUSystems && CurrentRow.bHasGPUEmitter && !Stage->IsCullingExercised())
	{
		SkipWholeRow(TEXT("Has a GPU emitter; GPU simulation does not dispatch in a headless run, so any cost measured here would understate it. Pass -includegpu to measure anyway, or use the editor panel."));
		return true;
	}

	UNiagaraSystem* System = Cast<UNiagaraSystem>(AssetData.GetAsset());
	if (System == nullptr)
	{
		SkipWholeRow(TEXT("Failed to load."));
		return true;
	}

	// WITH_EDITORONLY_DATA, not WITH_EDITOR: that is the guard the declaration itself sits under.
	// A freshly started commandlet has nothing compiled, and an uncompiled system measures as free.
	// This wait is what makes the first run agree with the second.
#if WITH_EDITORONLY_DATA
	System->WaitForCompilationComplete(/*bIncludingGPUShaders*/ false, /*bShowProgress*/ false);
#endif

	CurrentSystem.Reset(System);
	return true;
}

void FPulseNiagaraAuditDriver::FinishCurrentSample()
{
	FPulseNiagaraCostSample Sample = Stage->EndMeasurement();
	bRowMeasuredAnySample |= Sample.bMeasured;
	CurrentRow.Samples.Add(MoveTemp(Sample));

	SamplesCompleted++;
	++CurrentAxisIndex;
}

bool FPulseNiagaraAuditDriver::AdvanceOnce()
{
	// Nothing can run until the world exists. Reported as a measurement frame so a bounded Step
	// yields between attempts rather than spinning on a PIE session that is still loading.
	if (!bStageCreated)
	{
		TryCreateStage();
		return true;
	}

	if (Stage->IsMeasuring())
	{
		if (Stage->TickMeasurement())
		{
			return true;
		}
		FinishCurrentSample();
		return true;
	}

	// No measurement running: either start the next axis point, or move to the next system.
	if (CurrentAssetIndex < 0 || CurrentAxisIndex >= AxisPoints.Num())
	{
		BeginNextSystem();
		return false;
	}

	UNiagaraSystem* System = CurrentSystem.Get();
	if (System == nullptr)
	{
		BeginNextSystem();
		return false;
	}

	const FPulseNiagaraAxisPoint& Point = AxisPoints[CurrentAxisIndex];

	// Only re-apply on change: SetQualityLevel walks every loaded system calling UpdateScalability.
	if (CurrentAxisIndex == 0 || AxisPoints[CurrentAxisIndex - 1].QualityLevel != Point.QualityLevel)
	{
		FString Error;
		if (!Stage->SetQualityLevel(Point.QualityLevel, Error))
		{
			FatalError = Error;
			return false;
		}
	}

	Stage->BeginMeasurement(*System, Point.QualityLevel, Point.Distance);
	return false;
}

bool FPulseNiagaraAuditDriver::Step(double TimeBudgetSeconds)
{
	if (!bInitialized || IsComplete())
	{
		return false;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	const bool bUnbounded = TimeBudgetSeconds <= 0.0;

	for (;;)
	{
		const bool bAdvancedMeasurementFrame = AdvanceOnce();

		if (IsComplete())
		{
			break;
		}

		// In PIE a sample is only valid once per rendered frame, so one measurement
		// frame per Step is the hard ceiling however much budget is left. Setup work — loading,
		// spawning, quality changes — is not rate-limited that way and keeps going.
		if (bAdvancedMeasurementFrame && !Stage->AllowsMultipleFramesPerStep())
		{
			break;
		}
		if (!bUnbounded && FPlatformTime::Seconds() - StartSeconds >= TimeBudgetSeconds)
		{
			break;
		}
	}

	return !IsComplete();
}

const FPulseNiagaraCostReport& FPulseNiagaraAuditDriver::FinalizeReport()
{
	if (bFinalized)
	{
		return Report;
	}
	bFinalized = true;

	// A cancelled run still has a partly built row in flight; keeping it makes the partial report
	// honest about what was measured rather than dropping the system silently.
	if (!CurrentRow.AssetPath.IsEmpty())
	{
		if (bRowMeasuredAnySample)
		{
			Report.NumSystemsMeasured++;
		}
		else
		{
			Report.NumSystemsSkipped++;
		}
		Report.Rows.Add(MoveTemp(CurrentRow));
		CurrentRow = FPulseNiagaraCostRow();
	}

	CurrentSystem.Reset();

	// Before any report writing: the stage roots a world and holds a stats listener, neither of which
	// belongs alive past the run.
	Stage->Destroy();

	// End PIE only if this driver started it. Ending a session the user launched themselves would be
	// a surprise, and leaving one this driver started would be a leak.
	if (bPlaySessionRequested && GEditor != nullptr && GEditor->PlayWorld != nullptr)
	{
		GEditor->RequestEndPlayMap();
	}
	bPlaySessionRequested = false;

	return Report;
}
