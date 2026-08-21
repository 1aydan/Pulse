// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "PulseNiagaraAuditorCommandlet.generated.h"

/**
 * Measures what every Niagara system in a directory actually costs, by spawning it and ticking it.
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseNiagaraAuditor -AllowCommandletRendering
 *       [-path=/Game/VFX] [-quality=0,3] [-instances=N] [-settle=N] [-frames=N]
 *       [-maxsystems=N] [-reportdir=<dir>] [-includegpu] [-failoverbudget]
 *
 * Separate from the asset audit on purpose. That audit is static, deterministic and byte-diffable;
 * this one spawns things and reports measured milliseconds, which are none of those. Keeping them apart is
 * what stops a measurement run from contaminating a report CI gates on — the same reason its
 * settings live in UPulseNiagaraAuditorSettings rather than in UPulseSettings.
 *
 * Exit codes: 0 = ran, 1 = a system was over budget and -failoverbudget was given, 2 = bad
 * invocation or the stage could not be created. Main() is orchestration only — parsing lives in
 * FPulseNiagaraAuditorConfig, measurement in FPulseNiagaraCostStage, output in
 * FPulseNiagaraCostReportWriter.
 */
UCLASS()
class UPulseNiagaraAuditorCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UPulseNiagaraAuditorCommandlet(const FObjectInitializer& ObjectInitializer);

	virtual int32 Main(const FString& Params) override;
};
