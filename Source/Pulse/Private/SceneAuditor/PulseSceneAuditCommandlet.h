// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "PulseSceneAuditCommandlet.generated.h"

/**
 * Audits what each map places and lights: repeated non-Nanite meshes, stacked duplicates, stationary
 * light overlap, shadow-casting movable lights, small props with no draw distance.
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=Pulse.PulseSceneAudit -unattended -nullrhi
 *       [-maps=/Game/Maps/A,/Game/Maps/B] [-path=/Game/Maps] [-allmaps] [-reportdir=<dir>] [-failon=High]
 *
 * With no -maps=, audits Maps to Cook when the project lists any, otherwise every map under the scan
 * paths; -allmaps ignores Maps to Cook. Each map is loaded, inspected and released; nothing is saved.
 *
 * Exit codes: 0 = ran, 1 = a finding at or above -failon= severity (or the report could not be
 * written), 2 = bad invocation or no maps.
 */
UCLASS()
class UPulseSceneAuditCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UPulseSceneAuditCommandlet(const FObjectInitializer& ObjectInitializer);

	virtual int32 Main(const FString& Params) override;
};
