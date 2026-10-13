// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "PulseMemoryAuditorCommandlet.generated.h"

/**
 * Ranks every top-level asset in a project by the memory its hard references keep resident.
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseMemoryAuditor -unattended -nullrhi
 *       [-path=/Game/Maps,/Game/Core] [-inspect=/Game/Characters/BP_Player,...] [-reportdir=<dir>]
 *       [-nosoft] [-failoverbudget]
 *
 * Each -inspect= asset also gets an AssetReport_<Asset>_<timestamp> folder: its biggest cuts, ranked
 * by how much they save it.
 *
 * Registry-only: no package is loaded, so -nullrhi is fine and a whole project takes seconds.
 *
 * Exit codes: 0 = ran, 1 = the baseline or a top-level asset was over budget and -failoverbudget was
 * given (or the report could not be written), 2 = bad invocation or nothing to analyse. Main() is
 * orchestration only — parsing lives in FPulseMemoryAuditorConfig, analysis in FPulseMemoryAnalyzer,
 * output in FPulseMemoryReportWriter.
 */
UCLASS()
class UPulseMemoryAuditorCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UPulseMemoryAuditorCommandlet(const FObjectInitializer& ObjectInitializer);

	virtual int32 Main(const FString& Params) override;
};
