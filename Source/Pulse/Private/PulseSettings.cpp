// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseSettings.h"

#include "Misc/SecureHash.h"
#include "PulseCollector.h"
#include "PulseCollectorRegistry.h"
#include "UObject/UnrealType.h"

UPulseSettings::UPulseSettings()
{
	FDirectoryPath GameRoot;
	GameRoot.Path = TEXT("/Game");
	Scan.ScanPaths.Add(GameRoot);

	// Substring matches against the package path. External actor/object packages are audited by
	// the level collector through their owning world, never as standalone rows.
	Scan.ExcludePathPatterns.Add(TEXT("/Developers/"));
	Scan.ExcludePathPatterns.Add(TEXT("/__ExternalActors__/"));
	Scan.ExcludePathPatterns.Add(TEXT("/__ExternalObjects__/"));
	Scan.ExcludePathPatterns.Add(TEXT("/Collections/"));

	// Levels weigh a little more: one bad map hurts more than one bad prop. Unlisted categories
	// default to 1.0; empty categories are excluded from the overall blend entirely.
	Scoring.CategoryWeights.Add(FName(TEXT("Level")), 1.2f);
	Scoring.CategoryWeights.Add(FName(TEXT("StaticMesh")), 1.0f);
	Scoring.CategoryWeights.Add(FName(TEXT("Texture")), 1.0f);
	Scoring.CategoryWeights.Add(FName(TEXT("Material")), 1.0f);
	Scoring.CategoryWeights.Add(FName(TEXT("SkeletalMesh")), 0.8f);
	Scoring.CategoryWeights.Add(FName(TEXT("Blueprint")), 0.8f);
	Scoring.CategoryWeights.Add(FName(TEXT("Niagara")), 0.8f);
}

TArray<FString> UPulseSettings::GetRegisteredCategoryOptions() const
{
	// Asked by the details panel at edit time, so the list reflects whatever is actually registered
	// — including collectors added by other plugins, which a hardcoded checkbox per category could
	// never offer.
	TArray<FString> Options;
	for (const IPulseCollector* Collector : FPulseCollectorRegistry::GetAllSorted())
	{
		Options.AddUnique(Collector->GetCategory().ToString());
	}
	return Options;
}

bool UPulseSettings::IsCategoryEnabled(FName Category) const
{
	return !DisabledCategories.Contains(Category);
}

FString UPulseSettings::ComputeSettingsHash() const
{
	// Reflection export walks properties in declaration order, so the blob — and therefore the
	// hash — is deterministic for a given plugin version and settings state.
	FString Blob;
	for (TFieldIterator<FProperty> It(GetClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		Blob += It->GetName();
		Blob += TEXT("=");
		It->ExportTextItem_InContainer(Blob, this, /*DefaultValue*/ nullptr, /*Parent*/ nullptr, PPF_None);
		Blob += TEXT(";");
	}
	return FMD5::HashAnsiString(*Blob);
}
