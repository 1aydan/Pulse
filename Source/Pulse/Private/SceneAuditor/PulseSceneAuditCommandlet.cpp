// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseSceneAuditCommandlet.h"

#include "Misc/Parse.h"
#include "Pulse.h"
#include "PulseSceneAnalyzer.h"
#include "PulseSceneAuditorSettings.h"
#include "PulseSceneReportWriter.h"

UPulseSceneAuditCommandlet::UPulseSceneAuditCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = false;
}

int32 UPulseSceneAuditCommandlet::Main(const FString& Params)
{
	FPulseSceneConfig Config;
	FString Error;
	if (!FPulseSceneConfig::Parse(Params, Config, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Bad invocation: %s"), *Error);
		return 2;
	}

	// Optional gate, off by default like every other Pulse gate: reporting is not failing.
	bool bHasGate = false;
	EPulseSeverity Gate = EPulseSeverity::High;
	FString GateText;
	if (FParse::Value(*Params, TEXT("failon="), GateText))
	{
		const UEnum* SeverityEnum = StaticEnum<EPulseSeverity>();
		const int64 Value = SeverityEnum->GetValueByNameString(GateText);
		if (Value == INDEX_NONE)
		{
			UE_LOG(LogPulse, Error, TEXT("Bad invocation: -failon= expects Info, Low, Medium, High or Critical, got '%s'."), *GateText);
			return 2;
		}
		Gate = static_cast<EPulseSeverity>(Value);
		bHasGate = true;
	}

	Config.ApplySettingsDefaults(*GetDefault<UPulseSceneAuditorSettings>());
	UE_LOG(LogPulse, Display, TEXT("Pulse scene audit starting: %s"), *Config.ToString());

	FPulseSceneReport Report;
	FPulseSceneAnalyzer Analyzer(Config);
	if (!Analyzer.Run(Report, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Scene audit failed: %s"), *Error);
		return 2;
	}

	FPulseSceneReportWriter Writer(Config.ReportDir, Config.MaxHistoricalRuns);
	if (!Writer.Write(Report, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Report write failed: %s"), *Error);
		return 1;
	}

	int32 NumGated = 0;
	int32 NumFindings = 0;
	for (const FPulseSceneMap& Map : Report.Maps)
	{
		NumFindings += Map.Findings.Num();
		NumGated += bHasGate ? Map.CountFindings(Gate) : 0;
	}
	UE_LOG(LogPulse, Display, TEXT("Pulse scene audit complete. %d map(s), %d finding(s). Reports written to %s"),
		Report.Maps.Num(), NumFindings, *Writer.GetRunDir());

	if (bHasGate && NumGated > 0)
	{
		UE_LOG(LogPulse, Error, TEXT("%d finding(s) at or above %s."), NumGated, *GateText);
		return 1;
	}
	return 0;
}
