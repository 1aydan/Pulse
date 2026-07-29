// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseAuditCommandlet.h"

#include "Pulse.h"
#include "PulseCollector.h"
#include "PulseCollectorRegistry.h"
#include "PulseRunConfig.h"
#include "PulseSettings.h"

UPulseAuditCommandlet::UPulseAuditCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = false;
}

int32 UPulseAuditCommandlet::Main(const FString& Params)
{
	FPulseRunConfig Config;
	FString Error;
	if (!FPulseRunConfig::Parse(Params, Config, Error))
	{
		UE_LOG(LogPulse, Error, TEXT("Bad invocation: %s"), *Error);
		return 2;
	}

	const UPulseSettings& Settings = *GetDefault<UPulseSettings>();
	Config.ApplySettingsDefaults(Settings);

	if (Config.bShaderStatsRequested && !Config.bDeepRequested)
	{
		UE_LOG(LogPulse, Display, TEXT("-shaderstats implies -deep; running at tier ShaderStats."));
	}

	UE_LOG(LogPulse, Display, TEXT("Pulse audit starting: %s"), *Config.ToString());
	UE_LOG(LogPulse, Display, TEXT("Settings hash: %s"), *Settings.ComputeSettingsHash());

	TArray<IPulseCollector*> Collectors = FPulseCollectorRegistry::GetAllSorted();
	if (!Config.CategoryFilter.IsEmpty())
	{
		Collectors.RemoveAll([&Config](const IPulseCollector* Collector)
		{
			return !Config.CategoryFilter.ContainsByPredicate([Collector](const FString& Category)
			{
				return Category.Equals(Collector->GetCategory().ToString(), ESearchCase::IgnoreCase);
			});
		});
	}

	if (Collectors.IsEmpty())
	{
		UE_LOG(LogPulse, Error, TEXT("No collectors match -category=. Registered collectors: %d."),
			FPulseCollectorRegistry::GetAllSorted().Num());
		return 2;
	}

	UE_LOG(LogPulse, Display, TEXT("Running %d collectors."), Collectors.Num());

	// M0 stops here: the scan driver, scoring, and report writers land in M1.
	return 0;
}
