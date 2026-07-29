// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

class IPulseCollector;

/**
 * Facade over IModularFeatures so nobody has to remember the feature name, and so GetAllSorted()
 * can guarantee a stable order: IModularFeatures returns registration order, which depends on
 * module load order and is therefore not reproducible between machines.
 */
class PULSE_API FPulseCollectorRegistry
{
public:
	/** The IModularFeatures type name. External modules register under this. */
	static FName GetModularFeatureName();

	static void Register(IPulseCollector* Collector);
	static void Unregister(IPulseCollector* Collector);

	/** All registered collectors, sorted lexically by GetCollectorName(). */
	static TArray<IPulseCollector*> GetAllSorted();

	/**
	 * Built-in collector factories, populated at static-init by TPulseAutoCollector<> and drained
	 * by FPulseModule::StartupModule(). Meyers singleton: the list must exist before any
	 * translation unit's static initialisers run, and a file-scope TArray would not be guaranteed to.
	 */
	static TArray<TFunction<TUniquePtr<IPulseCollector>()>>& GetBuiltInFactories();
};

/**
 * Declare one of these at file scope in a collector's .cpp and it registers itself. Adding a
 * collector touches no existing file:
 *
 *     static TPulseAutoCollector<FPulseTextureCollector> GPulseTextureCollectorRegistrar;
 *
 * Construction is deferred to StartupModule rather than happening at static-init time, which keeps
 * the collectors' constructors out of the pre-main window where FName machinery is not up yet.
 */
template <typename TCollector>
struct TPulseAutoCollector
{
	TPulseAutoCollector()
	{
		FPulseCollectorRegistry::GetBuiltInFactories().Add([]() -> TUniquePtr<IPulseCollector>
		{
			return MakeUnique<TCollector>();
		});
	}
};
