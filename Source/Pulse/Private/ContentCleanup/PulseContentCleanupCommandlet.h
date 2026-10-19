// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "PulseContentCleanupCommandlet.generated.h"

/**
 * Lists content nothing that ships can load, redirectors still in the project, and imports that exist
 * more than once.
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseContentCleanup -unattended -nullrhi
 *       [-path=/Game/Art] [-reportdir=<dir>] [-allmaps] [-nosearch]
 *
 * -allmaps treats every map as shipping even when Maps to Cook is set; -nosearch skips the search of
 * .ini and C++ files for asset paths. Read-only: nothing is loaded or deleted.
 *
 * Exit codes: 0 = ran, 1 = the report could not be written, 2 = bad invocation or no roots found.
 */
UCLASS()
class UPulseContentCleanupCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UPulseContentCleanupCommandlet(const FObjectInitializer& ObjectInitializer);

	virtual int32 Main(const FString& Params) override;
};
