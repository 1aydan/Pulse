// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Templates/UniquePtr.h"

class FWorkspaceItem;
class IPulseCollector;

PULSE_API DECLARE_LOG_CATEGORY_EXTERN(LogPulse, Log, All);

class FPulseModule : public IModuleInterface
{
public:
	// Out-of-line so TUniquePtr<IPulseCollector> can destroy a forward-declared type.
	virtual ~FPulseModule() override;

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** Built-in collectors, instantiated from the static factory list at startup. Owned here. */
	TArray<TUniquePtr<IPulseCollector>> OwnedCollectors;

	/** The Tools menu group hosting the audit tab. Removed on shutdown. */
	TSharedPtr<FWorkspaceItem> MenuGroup;
};
