// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseCollectorRegistry.h"

#include "Features/IModularFeatures.h"
#include "PulseCollector.h"

FName FPulseCollectorRegistry::GetModularFeatureName()
{
	static const FName FeatureName(TEXT("PulseCollector"));
	return FeatureName;
}

void FPulseCollectorRegistry::Register(IPulseCollector* Collector)
{
	IModularFeatures::Get().RegisterModularFeature(GetModularFeatureName(), Collector);
}

void FPulseCollectorRegistry::Unregister(IPulseCollector* Collector)
{
	IModularFeatures::Get().UnregisterModularFeature(GetModularFeatureName(), Collector);
}

TArray<IPulseCollector*> FPulseCollectorRegistry::GetAllSorted()
{
	TArray<IPulseCollector*> Collectors =
		IModularFeatures::Get().GetModularFeatureImplementations<IPulseCollector>(GetModularFeatureName());

	// Lexical, not FName-index, comparison: FName order is name-table insertion order and varies
	// per process, which would make collector iteration order — and therefore report row order —
	// non-reproducible.
	Collectors.Sort([](const IPulseCollector& A, const IPulseCollector& B)
	{
		return A.GetCollectorName().ToString() < B.GetCollectorName().ToString();
	});

	return Collectors;
}

TArray<TFunction<TUniquePtr<IPulseCollector>()>>& FPulseCollectorRegistry::GetBuiltInFactories()
{
	static TArray<TFunction<TUniquePtr<IPulseCollector>()>> Factories;
	return Factories;
}
