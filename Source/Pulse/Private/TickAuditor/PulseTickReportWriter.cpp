// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseTickReportWriter.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "PulseCsvWriter.h"
#include "PulseTickResult.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

static const TCHAR* GPulseTickRunPrefix = TEXT("TickCapture_");

/** Four decimals, as in the Niagara auditor: a cheap tick is a few thousandths of a millisecond. */
static FString PulseTickMs(double Milliseconds)
{
	return FString::Printf(TEXT("%.4f"), Milliseconds);
}

static FString PulseTickBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

static FString PulseTickPercent(double Milliseconds, float FrameBudgetMs)
{
	return FrameBudgetMs > 0.0f ? FString::Printf(TEXT("%.2f"), Milliseconds / FrameBudgetMs * 100.0) : FString();
}

FPulseTickReportWriter::FPulseTickReportWriter(const FString& InReportDir, int32 InMaxHistoricalRuns)
	: ReportDir(InReportDir)
	, MaxHistoricalRuns(InMaxHistoricalRuns)
{
}

bool FPulseTickReportWriter::Write(const FPulseTickReport& Report, FString& OutError)
{
	RunDir = ReportDir / (GPulseTickRunPrefix + Report.GeneratedAtUtc);
	IFileManager::Get().MakeDirectory(*RunDir, /*Tree*/ true);
	if (!IFileManager::Get().DirectoryExists(*RunDir))
	{
		OutError = FString::Printf(TEXT("Failed to create report directory %s"), *RunDir);
		return false;
	}
	if (!WriteClassesCsv(Report, OutError) || !WriteComponentsCsv(Report, OutError) || !WriteRunJson(Report, OutError))
	{
		return false;
	}
	PruneHistoricalRuns();
	return true;
}

bool FPulseTickReportWriter::WriteClassesCsv(const FPulseTickReport& Report, FString& OutError) const
{
	FPulseCsvWriter Csv;
	Csv.WriteRow({
		TEXT("Class"), TEXT("AssetPath"), TEXT("NativeParent"), TEXT("Instances"),
		TEXT("TotalMsPerFrame"), TEXT("TotalPercentOfFrame"), TEXT("PerInstanceMsPerFrame"), TEXT("PerInstancePercentOfFrame"),
		TEXT("ActorMsPerFrame"), TEXT("WorstInstanceFrameMs"),
		TEXT("ActorTicks"), TEXT("ComponentTicks"), TEXT("MinActorTickInterval"), TEXT("HasEventTick"),
		TEXT("TicksEveryFrame"), TEXT("IdleActorTick"), TEXT("OverClassBudget"), TEXT("OverInstanceBudget"),
		TEXT("TopComponents")
	});

	for (const FPulseTickClassRow& Row : Report.Rows)
	{
		TArray<FString> Top;
		for (int32 Index = 0; Index < FMath::Min(3, Row.Components.Num()); ++Index)
		{
			Top.Add(FString::Printf(TEXT("%s x%d %s"), *Row.Components[Index].ClassName, Row.Components[Index].Count, *PulseTickMs(Row.Components[Index].MsPerFrame)));
		}

		Csv.WriteRow({
			Row.ClassName,
			Row.AssetPath,
			Row.NativeParent,
			FString::FromInt(Row.NumInstances),
			PulseTickMs(Row.TotalMsPerFrame),
			PulseTickPercent(Row.TotalMsPerFrame, Report.FrameBudgetMs),
			PulseTickMs(Row.PerInstanceMsPerFrame),
			PulseTickPercent(Row.PerInstanceMsPerFrame, Report.FrameBudgetMs),
			PulseTickMs(Row.ActorMsPerFrame),
			PulseTickMs(Row.WorstInstanceFrameMs),
			FString::FromInt(Row.NumActorTicks),
			FString::FromInt(Row.NumComponentTicks),
			Row.NumActorTicks > 0 ? FString::Printf(TEXT("%.3f"), Row.MinActorTickInterval) : FString(),
			PulseTickBool(Row.bHasEventTick),
			PulseTickBool(Row.bTicksEveryFrame),
			PulseTickBool(Row.bIdleActorTick),
			PulseTickBool(Row.bOverClassBudget),
			PulseTickBool(Row.bOverInstanceBudget),
			FString::Join(Top, TEXT("; "))
		});
	}
	return Csv.SaveToFile(RunDir / TEXT("Classes.csv"), OutError);
}

bool FPulseTickReportWriter::WriteComponentsCsv(const FPulseTickReport& Report, FString& OutError) const
{
	FPulseCsvWriter Csv;
	Csv.WriteRow({ TEXT("ActorClass"), TEXT("Component"), TEXT("Count"), TEXT("MsPerFrame"), TEXT("PercentOfFrame"), TEXT("MinTickInterval") });
	for (const FPulseTickClassRow& Row : Report.Rows)
	{
		for (const FPulseTickComponentCost& Component : Row.Components)
		{
			Csv.WriteRow({
				Row.ClassName,
				Component.ClassName,
				FString::FromInt(Component.Count),
				PulseTickMs(Component.MsPerFrame),
				PulseTickPercent(Component.MsPerFrame, Report.FrameBudgetMs),
				FString::Printf(TEXT("%.3f"), Component.MinTickInterval)
			});
		}
	}
	return Csv.SaveToFile(RunDir / TEXT("Components.csv"), OutError);
}

bool FPulseTickReportWriter::WriteRunJson(const FPulseTickReport& Report, FString& OutError) const
{
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("generatedAtUtc"), Report.GeneratedAtUtc);
	Writer->WriteValue(TEXT("engineVersion"), Report.EngineVersion);
	Writer->WriteValue(TEXT("machineName"), Report.MachineName);
	Writer->WriteValue(TEXT("world"), Report.WorldName);
	Writer->WriteValue(TEXT("framesCaptured"), Report.FramesCaptured);
	Writer->WriteValue(TEXT("capturedSeconds"), Report.CapturedSeconds);
	Writer->WriteValue(TEXT("tickFunctions"), Report.NumTickFunctions);
	Writer->WriteValue(TEXT("tickFunctionsLost"), Report.NumTargetsLost);
	Writer->WriteValue(TEXT("totalMsPerFrame"), Report.TotalMsPerFrame);
	Writer->WriteValue(TEXT("frameBudgetMs"), Report.FrameBudgetMs);
	Writer->WriteValue(TEXT("maxFrameBudgetPercentPerClass"), Report.MaxFrameBudgetPercentPerClass);
	Writer->WriteValue(TEXT("maxFrameBudgetPercentPerInstance"), Report.MaxFrameBudgetPercentPerInstance);
	Writer->WriteValue(TEXT("idleTickMicroseconds"), Report.IdleTickMicroseconds);
	Writer->WriteValue(TEXT("measurementNote"),
		TEXT("Game-thread time of every actor and component tick that was enabled when the capture began, timed by running each through its own ExecuteTick from a probe in its tick group. ")
		TEXT("Tick groups are kept; prerequisites within a group are not. Work the engine would run on worker threads (parallel animation, async ticks) was timed on the game thread, so those classes read higher than in game. ")
		TEXT("Actors spawned during the capture ticked normally and are not included."));
	Writer->WriteObjectEnd();
	Writer->Close();

	const FString Path = RunDir / TEXT("Run.json");
	if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write %s"), *Path);
		return false;
	}
	return true;
}

void FPulseTickReportWriter::PruneHistoricalRuns() const
{
	if (MaxHistoricalRuns <= 0)
	{
		return;
	}
	TArray<FString> RunFolders;
	IFileManager::Get().FindFiles(RunFolders, *(ReportDir / (FString(GPulseTickRunPrefix) + TEXT("*"))), /*Files*/ false, /*Directories*/ true);
	if (RunFolders.Num() <= MaxHistoricalRuns)
	{
		return;
	}
	RunFolders.Sort();
	for (int32 Index = 0; Index < RunFolders.Num() - MaxHistoricalRuns; ++Index)
	{
		IFileManager::Get().DeleteDirectory(*(ReportDir / RunFolders[Index]), /*RequireExists*/ false, /*Tree*/ true);
	}
}
