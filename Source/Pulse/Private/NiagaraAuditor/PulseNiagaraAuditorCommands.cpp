// Copyright (c) 2026 Pulse contributors. MIT License.

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Pulse.h"
#include "PulseNiagaraAuditDriver.h"
#include "PulseNiagaraAuditorConfig.h"
#include "PulseNiagaraAuditorSettings.h"
#include "PulseNiagaraCostReportWriter.h"
#include "PulseNiagaraCostResult.h"
#include "PulseNiagaraCostStage.h"
#include "UI/SPulseNiagaraAuditorOverlay.h"
#include "Templates/UniquePtr.h"

/**
 * Console entry points, so a run can be started from the editor console or an -ExecCmds line without
 * opening the panel — the same convenience PSOForge.Start provides.
 *
 * These drive the identical FPulseNiagaraAuditDriver the panel and the commandlet use, in a PIE
 * session, and write the same report on completion. Progress goes to the log and the overlay.
 */
namespace PulseNiagaraAuditorCommands
{
	/** The one in-flight run. A second start is refused rather than queued: two runs would spawn two
	    populations into the same world and attribute each other's cost. */
	static TUniquePtr<FPulseNiagaraAuditDriver> GDriver;
	static FTSTicker::FDelegateHandle GTickerHandle;
	static int32 GLastLoggedPercent = -1;

	static void Finish()
	{
		if (!GDriver.IsValid())
		{
			return;
		}

		const FPulseNiagaraCostReport& Report = GDriver->FinalizeReport();

		const UPulseNiagaraAuditorSettings& Settings = *GetDefault<UPulseNiagaraAuditorSettings>();
		FPulseNiagaraAuditorConfig Config;
		Config.ApplySettingsDefaults(Settings);

		FPulseNiagaraCostReportWriter Writer(Config.ReportDir, Config.Budget, Settings.Scan.MaxHistoricalRuns);
		FString Error;
		if (Writer.Write(Report, Error))
		{
			UE_LOG(LogPulse, Display, TEXT("Niagara audit complete: %d measured, %d skipped, of %d discovered. Report: %s"),
				Report.NumSystemsMeasured, Report.NumSystemsSkipped, Report.NumSystemsDiscovered, *Writer.GetRunDir());
		}
		else
		{
			UE_LOG(LogPulse, Error, TEXT("Niagara audit report write failed: %s"), *Error);
		}

		PulseNiagaraAuditorOverlay::Hide();
		GDriver.Reset();
		GLastLoggedPercent = -1;
	}

	static void Stop()
	{
		if (GTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GTickerHandle);
			GTickerHandle.Reset();
		}
	}

	static bool Tick(float DeltaTime)
	{
		if (!GDriver.IsValid())
		{
			GTickerHandle.Reset();
			return false;
		}

		const bool bMoreWork = GDriver->Step(/*TimeBudgetSeconds*/ 0.008);

		if (!GDriver->GetError().IsEmpty())
		{
			UE_LOG(LogPulse, Error, TEXT("%s"), *GDriver->GetError());
			Finish();
			GTickerHandle.Reset();
			return false;
		}

		// Logged at whole percentages only: this ticks every frame for minutes, and a line per frame
		// would bury whatever else the console is being used for.
		const FPulseNiagaraAuditProgress Progress = GDriver->GetProgress();
		if (Progress.SamplesTotal > 0)
		{
			const int32 Percent = (Progress.SamplesCompleted * 100) / Progress.SamplesTotal;
			if (Percent != GLastLoggedPercent)
			{
				GLastLoggedPercent = Percent;
				UE_LOG(LogPulse, Display, TEXT("Niagara audit %d%% (%d/%d) - %s"),
					Percent, Progress.SamplesCompleted, Progress.SamplesTotal, *Progress.CurrentSystemName);
			}
		}

		if (bMoreWork)
		{
			return true;
		}

		Finish();
		GTickerHandle.Reset();
		return false;
	}

	static void Cancel();

	/** Pulled by the overlay each frame; an empty struct once the run is gone. */
	static FPulseNiagaraAuditProgress GetProgressForOverlay()
	{
		return GDriver.IsValid() ? GDriver->GetProgress() : FPulseNiagaraAuditProgress();
	}

	static void Start(const TArray<FString>& Args)
	{
		if (GDriver.IsValid())
		{
			UE_LOG(LogPulse, Warning, TEXT("A Niagara audit is already running. Use Pulse.Niagara.Cancel first."));
			return;
		}

		const UPulseNiagaraAuditorSettings& Settings = *GetDefault<UPulseNiagaraAuditorSettings>();
		FPulseNiagaraAuditorConfig Config;
		Config.ApplySettingsDefaults(Settings);

		// A single optional argument narrows the scan, so a quick check of one folder does not mean
		// editing project settings first.
		if (Args.Num() > 0 && !Args[0].IsEmpty())
		{
			Config.PathFilter.Reset();
			Config.PathFilter.Add(Args[0]);
		}

		GDriver = MakeUnique<FPulseNiagaraAuditDriver>(Config, Settings, EPulseNiagaraStageMode::PIEWorld);

		FString Error;
		if (!GDriver->Initialize(Error))
		{
			UE_LOG(LogPulse, Error, TEXT("Could not start the Niagara audit: %s"), *Error);
			GDriver.Reset();
			return;
		}

		GLastLoggedPercent = -1;
		UE_LOG(LogPulse, Display, TEXT("Niagara audit started: %s"), *Config.ToString());

		PulseNiagaraAuditorOverlay::Show(
			FPulseNiagaraGetProgress::CreateStatic(&GetProgressForOverlay),
			FSimpleDelegate::CreateStatic(&Cancel));

		GTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
	}

	static void Cancel()
	{
		if (!GDriver.IsValid())
		{
			UE_LOG(LogPulse, Display, TEXT("No Niagara audit is running."));
			return;
		}

		GDriver->Cancel();
		Stop();
		Finish();
		UE_LOG(LogPulse, Display, TEXT("Niagara audit cancelled; the partial report was written."));
	}

	static void Status()
	{
		if (!GDriver.IsValid())
		{
			UE_LOG(LogPulse, Display, TEXT("No Niagara audit is running."));
			return;
		}

		const FPulseNiagaraAuditProgress Progress = GDriver->GetProgress();
		UE_LOG(LogPulse, Display, TEXT("Niagara audit: %d/%d samples, currently %s (%s)."),
			Progress.SamplesCompleted, Progress.SamplesTotal, *Progress.CurrentSystemName, *Progress.CurrentQualityName);
	}
}

static FAutoConsoleCommand GPulseNiagaraAuditCommand(
	TEXT("Pulse.Niagara.Start"),
	TEXT("Measure the cost of every Niagara system under the configured scan paths, in the current level. ")
	TEXT("Optional argument: a package path to scan instead, e.g. Pulse.Niagara.Start /Game/VFX"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&PulseNiagaraAuditorCommands::Start));

static FAutoConsoleCommand GPulseNiagaraCancelCommand(
	TEXT("Pulse.Niagara.Cancel"),
	TEXT("Cancel the running Niagara audit and write the partial report."),
	FConsoleCommandDelegate::CreateStatic(&PulseNiagaraAuditorCommands::Cancel));

static FAutoConsoleCommand GPulseNiagaraStatusCommand(
	TEXT("Pulse.Niagara.Status"),
	TEXT("Log the progress of the running Niagara audit."),
	FConsoleCommandDelegate::CreateStatic(&PulseNiagaraAuditorCommands::Status));
