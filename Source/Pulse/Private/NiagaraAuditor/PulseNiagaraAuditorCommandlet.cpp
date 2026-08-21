// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseNiagaraAuditorCommandlet.h"

#include "Misc/App.h"
#include "Pulse.h"
#include "PulseNiagaraAuditDriver.h"
#include "PulseNiagaraAuditorConfig.h"
#include "PulseNiagaraAuditorSettings.h"
#include "PulseNiagaraCostReportWriter.h"
#include "PulseNiagaraCostResult.h"

UPulseNiagaraAuditorCommandlet::UPulseNiagaraAuditorCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// IsClient true, unlike the asset audit's commandlet. FEngineLoop copies this straight onto
	// GIsClient, and FRendererModule::AllocateScene only builds a real FScene when
	// GIsClient && FApp::CanEverRender() && !GUsingNullRHI — otherwise every world silently gets an
	// FNULLSceneInterface, no render proxies are created, and the render-state work that is part of
	// a Niagara system's real game-thread cost never happens. Measuring that would understate every
	// system. Necessary but not sufficient: -AllowCommandletRendering is still required, and without
	// it the null scene is still what you get.
	IsClient = true;
	IsEditor = true;
	IsServer = false;
	LogToConsole = false;
}

int32 UPulseNiagaraAuditorCommandlet::Main(const FString& Params)
{
	FPulseNiagaraAuditorConfig Config;
	FString Error;
	if (!FPulseNiagaraAuditorConfig::Parse(Params, Config, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Bad invocation: %s"), *Error);
		return 2;
	}

	const UPulseNiagaraAuditorSettings& Settings = *GetDefault<UPulseNiagaraAuditorSettings>();
	Config.ApplySettingsDefaults(Settings);

	UE_LOG(LogPulse, Display, TEXT("Pulse Niagara auditor starting: %s"), *Config.ToString());

	if (!FApp::CanEverRender())
	{
		// Not fatal: game-thread cost is what this tool reports and it measures without an RHI. It is
		// logged loudly because it changes how far the numbers can be trusted, and Run.json records
		// it so a CSV read later cannot lose that context.
		UE_LOG(LogPulse, Warning,
			TEXT("Running without rendering. Pass -AllowCommandletRendering for a closer approximation of a real frame; game-thread costs are still measured."));
	}

	// Distance culling cannot run here at all — see FPulseNiagaraCostStage::Create. Saying so once,
	// up front, is cheaper than a reader discovering it in Run.json after drawing a conclusion.
	UE_LOG(LogPulse, Display,
		TEXT("Distance culling is paused for commandlet runs (no viewport, so Niagara can cache no views). Costs are uncculled; use the editor panel to measure culling by distance."));

	// PrivateWorld: the commandlet owns its world and drives its own frames.
	FPulseNiagaraAuditDriver Driver(Config, Settings, EPulseNiagaraStageMode::PrivateWorld);
	if (!Driver.Initialize(Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Could not start the audit: %s"), *Error);
		return 2;
	}

	// Unbounded budget: the commandlet has no frame to yield to, so one Step drains everything.
	while (Driver.Step(/*TimeBudgetSeconds*/ 0.0))
	{
	}

	if (!Driver.GetError().IsEmpty())
	{
		UE_LOG(LogPulse, Error, TEXT("%s"), *Driver.GetError());
		return 2;
	}

	const FPulseNiagaraCostReport& Report = Driver.FinalizeReport();

	FPulseNiagaraCostReportWriter Writer(Config.ReportDir, Config.Budget, Settings.Scan.MaxHistoricalRuns);
	if (!Writer.Write(Report, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Report write failed: %s"), *Error);
		return 1;
	}

	int32 NumOverBudget = 0;
	for (const FPulseNiagaraCostRow& Row : Report.Rows)
	{
		for (const FPulseNiagaraCostSample& Sample : Row.Samples)
		{
			if (Sample.bMeasured && Config.FrameBudgetMs > 0.0f)
			{
				const double PercentOfFrame = Sample.PerInstanceAvgMs / Config.FrameBudgetMs * 100.0;
				if (PercentOfFrame > Config.Budget.MaxFrameBudgetPercentPerInstance)
				{
					NumOverBudget++;
				}
			}
		}
	}

	UE_LOG(LogPulse, Display, TEXT("Pulse Niagara audit complete. %d discovered, %d measured, %d skipped, %d sample(s) over budget."),
		Report.NumSystemsDiscovered, Report.NumSystemsMeasured, Report.NumSystemsSkipped, NumOverBudget);
	UE_LOG(LogPulse, Display, TEXT("Reports written to %s"), *Writer.GetRunDir());

	if (Config.bFailOverBudget && NumOverBudget > 0)
	{
		UE_LOG(LogPulse, Error, TEXT("%d sample(s) exceeded %.2f%% of the frame budget per instance."),
			NumOverBudget, Config.Budget.MaxFrameBudgetPercentPerInstance);
		return 1;
	}

	return 0;
}
