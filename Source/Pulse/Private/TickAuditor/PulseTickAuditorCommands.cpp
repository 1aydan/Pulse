// Copyright (c) 2026 Pulse contributors. MIT License.

#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "Pulse.h"
#include "PulseTickAuditorSettings.h"
#include "PulseTickCapture.h"
#include "PulseTickReportWriter.h"
#include "PulseTickResult.h"

/**
 * Pulse.Tick.Capture [frames] and Pulse.Tick.Stop, for capturing from the PIE console or an
 * automation script without the panel. The report is written as soon as the capture finishes.
 */
namespace PulseTickCommands
{
	// A plain pointer, replaced on the next capture and otherwise left for process exit. A static
	// TUniquePtr would run the capture's destructor during static teardown, after the engine delegates
	// it unbinds from may already be gone.
	static FPulseTickCapture* GCapture = nullptr;

	static FString ResolveReportDir(const UPulseTickAuditorSettings& Settings)
	{
		FString ReportDir = Settings.ReportDirectory;
		if (FPaths::IsRelative(ReportDir))
		{
			ReportDir = FPaths::ProjectDir() / ReportDir;
		}
		return FPaths::ConvertRelativePathToFull(ReportDir);
	}

	static void WriteReport()
	{
		if (GCapture == nullptr || !GCapture->HasData())
		{
			UE_LOG(LogPulse, Warning, TEXT("Pulse.Tick: the capture ended before a whole frame was recorded; nothing written."));
			return;
		}

		const UPulseTickAuditorSettings& Settings = *GetDefault<UPulseTickAuditorSettings>();
		const FPulseTickReport Report = GCapture->BuildReport(Settings);
		FPulseTickReportWriter Writer(ResolveReportDir(Settings), Settings.MaxHistoricalRuns);
		FString Error;
		if (!Writer.Write(Report, Error))
		{
			UE_LOG(LogPulse, Error, TEXT("Pulse.Tick: report write failed: %s"), *Error);
			return;
		}

		UE_LOG(LogPulse, Display, TEXT("Pulse.Tick: %d frame(s), %d tick function(s), %.3f ms/frame of ticking. Report: %s"),
			Report.FramesCaptured, Report.NumTickFunctions, Report.TotalMsPerFrame, *Writer.GetRunDir());
		for (int32 Index = 0; Index < FMath::Min(5, Report.Rows.Num()); ++Index)
		{
			const FPulseTickClassRow& Row = Report.Rows[Index];
			UE_LOG(LogPulse, Display, TEXT("  %s x%d: %.4f ms/frame"), *Row.ClassName, Row.NumInstances, Row.TotalMsPerFrame);
		}
	}

	static FAutoConsoleCommand CaptureCommand(
		TEXT("Pulse.Tick.Capture"),
		TEXT("Capture what every actor and component tick costs in the running PIE session. Optional argument: frames (default from Pulse - Tick Auditor settings)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
			const UPulseTickAuditorSettings& Settings = *GetDefault<UPulseTickAuditorSettings>();
			const int32 Frames = Args.Num() > 0 && Args[0].IsNumeric() ? FCString::Atoi(*Args[0]) : Settings.CaptureFrames;

			if (GCapture != nullptr && GCapture->IsRunning())
			{
				UE_LOG(LogPulse, Warning, TEXT("Pulse.Tick: a capture is already running. Pulse.Tick.Stop ends it."));
				return;
			}

			delete GCapture;
			GCapture = new FPulseTickCapture();
			GCapture->OnFinished.AddLambda([]() { WriteReport(); });

			FString Error;
			if (!GCapture->Start(PlayWorld, Frames, Error))
			{
				UE_LOG(LogPulse, Error, TEXT("Pulse.Tick: %s"), *Error);
				delete GCapture;
				GCapture = nullptr;
			}
		}));

	static FAutoConsoleCommand StopCommand(
		TEXT("Pulse.Tick.Stop"),
		TEXT("End a running Pulse.Tick.Capture early and write what was captured."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GCapture != nullptr)
			{
				GCapture->Stop();
			}
		}));
}
