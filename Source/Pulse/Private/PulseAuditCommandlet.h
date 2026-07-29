// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "PulseAuditCommandlet.generated.h"

/**
 * Read-only performance audit over the project's assets.
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseAudit
 *       [-deep] [-shaderstats] [-category=A,B] [-path=/Game/X] [-minseverity=Medium]
 *       [-maxassets=N] [-reportdir=<dir>] [-format=csv,json] [-failunder=85] [-gcfreq=N]
 *
 * Exit codes: 0 = audit ran (and passed -failunder if given), 1 = score below -failunder,
 * 2 = bad invocation. Main() is orchestration only — parsing lives in FPulseRunConfig, the scan
 * in FPulseScanDriver, scoring in FPulseScoring, and output in FPulseReportWriter.
 */
UCLASS()
class UPulseAuditCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UPulseAuditCommandlet(const FObjectInitializer& ObjectInitializer);

	virtual int32 Main(const FString& Params) override;
};
