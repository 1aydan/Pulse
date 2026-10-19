// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "PulseSceneAuditorSettings.generated.h"

/** Where a finding fires. Each is a count or a size; 0 disables the finding. */
USTRUCT()
struct FPulseSceneThresholds
{
	GENERATED_BODY()

	/**
	 * A non-Nanite static mesh placed as at least this many separate components. Each is its own scene
	 * primitive — culled, sorted and updated individually — where an instanced or Nanite version is one.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Thresholds", meta = (ClampMin = 0))
	int32 RepeatedMeshMinCount = 50;

	/** A placed non-Nanite mesh whose LOD0 has at least this many triangles. */
	UPROPERTY(EditAnywhere, Config, Category = "Thresholds", meta = (ClampMin = 0))
	int32 HighPolyTriangles = 100000;

	/** Primitive components in one map's persistent level. */
	UPROPERTY(EditAnywhere, Config, Category = "Thresholds", meta = (ClampMin = 0))
	int32 MaxPrimitives = 10000;

	/** Movable lights casting shadows. Each re-renders shadow depth for what it touches every frame. */
	UPROPERTY(EditAnywhere, Config, Category = "Thresholds", meta = (ClampMin = 0))
	int32 MaxMovableShadowLights = 4;

	/** A movable shadow-casting local light with an attenuation radius at least this large, in centimetres. */
	UPROPERTY(EditAnywhere, Config, Category = "Thresholds", meta = (ClampMin = 0.0))
	float LargeShadowLightRadius = 3000.0f;

	/** Movable primitives casting dynamic shadows. */
	UPROPERTY(EditAnywhere, Config, Category = "Thresholds", meta = (ClampMin = 0))
	int32 MaxMovableShadowCasters = 200;

	/** A prop counts as small when its bounds radius is under this, in centimetres. */
	UPROPERTY(EditAnywhere, Config, Category = "Thresholds", meta = (ClampMin = 0.0))
	float SmallPropRadius = 100.0f;

	/** Small non-Nanite props with no draw distance at all, so they render as far as the eye can see. */
	UPROPERTY(EditAnywhere, Config, Category = "Thresholds", meta = (ClampMin = 0))
	int32 MaxSmallPropsWithoutCullDistance = 300;

	/** Primitives using a translucent material. Translucency is sorted and drawn per primitive, without early-z. */
	UPROPERTY(EditAnywhere, Config, Category = "Thresholds", meta = (ClampMin = 0))
	int32 MaxTranslucentPrimitives = 200;
};

/**
 * Project configuration for the Pulse scene auditor, stored in Config/DefaultPulse.ini under
 * [/Script/Pulse.PulseSceneAuditorSettings].
 */
UCLASS(Config = Pulse, DefaultConfig, meta = (DisplayName = "Pulse - Scene Auditor"))
class PULSE_API UPulseSceneAuditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPulseSceneAuditorSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/** Maps under these paths are audited by "Audit maps". */
	UPROPERTY(EditAnywhere, Config, Category = "Scan", meta = (ContentDir))
	TArray<FDirectoryPath> ScanPaths;

	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	TArray<FString> ExcludePathPatterns;

	/** Audit only Maps to Cook when the project lists any: the maps that ship. */
	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	bool bOnlyMapsToCook = true;

	UPROPERTY(EditAnywhere, Config, Category = "Thresholds")
	FPulseSceneThresholds Thresholds;

	UPROPERTY(EditAnywhere, Config, Category = "Report")
	FString ReportDirectory = TEXT("Saved/Pulse/SceneAuditor");

	UPROPERTY(EditAnywhere, Config, Category = "Report", meta = (ClampMin = 0))
	int32 MaxHistoricalRuns = 20;
};
