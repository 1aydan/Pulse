// Copyright (c) 2026 Pulse contributors. MIT License.

#include "Pulse.h"

#include "PulseCollector.h"
#include "PulseCollectorRegistry.h"

DEFINE_LOG_CATEGORY(LogPulse);

FPulseModule::~FPulseModule() = default;

void FPulseModule::StartupModule()
{
	// Built-in collectors registered themselves as factories during static init; instantiate and
	// publish them now, once FName and the module system are fully up.
	for (const TFunction<TUniquePtr<IPulseCollector>()>& Factory : FPulseCollectorRegistry::GetBuiltInFactories())
	{
		TUniquePtr<IPulseCollector> Collector = Factory();
		FPulseCollectorRegistry::Register(Collector.Get());
		OwnedCollectors.Add(MoveTemp(Collector));
	}

	UE_LOG(LogPulse, Log, TEXT("Pulse module loaded with %d built-in collectors."), OwnedCollectors.Num());
}

void FPulseModule::ShutdownModule()
{
	for (const TUniquePtr<IPulseCollector>& Collector : OwnedCollectors)
	{
		FPulseCollectorRegistry::Unregister(Collector.Get());
	}
	OwnedCollectors.Empty();
}

IMPLEMENT_MODULE(FPulseModule, Pulse)
