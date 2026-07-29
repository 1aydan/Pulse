// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "PulseTypes.h"
#include "PulseSettings.generated.h"

/** Where to look and how hard to work. */
USTRUCT()
struct FPulseScanSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, Category = "Scan", meta = (ContentDir))
	TArray<FDirectoryPath> ScanPaths;

	/** Package paths containing any of these substrings are skipped entirely. */
	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	TArray<FString> ExcludePathPatterns;

	/** Packages loaded between garbage collections at Deep tier. */
	UPROPERTY(EditAnywhere, Config, Category = "Scan", meta = (ClampMin = 1))
	int32 GCPackageInterval = 100;

	/** Materials pre-warmed per shader-stats batch. Larger = more parallel compile, more peak memory. */
	UPROPERTY(EditAnywhere, Config, Category = "Scan", meta = (ClampMin = 1))
	int32 ShaderStatsBatchSize = 64;

	/** Relative paths resolve against the project directory. */
	UPROPERTY(EditAnywhere, Config, Category = "Report")
	FString ReportDirectory = TEXT("Saved/Pulse");

	/** Historical PulseAudit_<timestamp>.json snapshots kept; oldest pruned. 0 = keep all. */
	UPROPERTY(EditAnywhere, Config, Category = "Report", meta = (ClampMin = 0))
	int32 MaxHistoricalSnapshots = 50;
};

/**
 * The whole scoring formula, exposed. Per asset: S = 100 * exp(-(sum of severity weights) / K).
 * Per category: (100 - mean deficit) * (1 - Gamma * share of assets with issues). Overall: the
 * category-weight blend over non-empty categories.
 */
USTRUCT()
struct FPulseScoringSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, Category = "Weights", meta = (ClampMin = 0.0))
	float WeightInfo = 0.0f;

	UPROPERTY(EditAnywhere, Config, Category = "Weights", meta = (ClampMin = 0.0))
	float WeightLow = 3.0f;

	UPROPERTY(EditAnywhere, Config, Category = "Weights", meta = (ClampMin = 0.0))
	float WeightMedium = 8.0f;

	UPROPERTY(EditAnywhere, Config, Category = "Weights", meta = (ClampMin = 0.0))
	float WeightHigh = 20.0f;

	UPROPERTY(EditAnywhere, Config, Category = "Weights", meta = (ClampMin = 0.0))
	float WeightCritical = 45.0f;

	/** Per-asset decay scale K. Larger = more forgiving. One Critical (45) at K=60 scores 47.2. */
	UPROPERTY(EditAnywhere, Config, Category = "Scoring", meta = (ClampMin = 1.0))
	float AssetDecayScale = 60.0f;

	/** Breadth weight Gamma. 0 = category score is the plain mean; 1 = a fully-affected category halves. */
	UPROPERTY(EditAnywhere, Config, Category = "Scoring", meta = (ClampMin = 0.0, ClampMax = 1.0))
	float CategoryBreadthWeight = 0.5f;

	/** Category weights for the overall blend. Missing categories default to 1.0; empty ones are excluded. */
	UPROPERTY(EditAnywhere, Config, Category = "Scoring")
	TMap<FName, float> CategoryWeights;
};

USTRUCT()
struct FPulseTextureThresholds
{
	GENERATED_BODY()

	/**
	 * Fast tier compares against the SOURCE dimensions — the registry's Dimensions tag comes from
	 * GetImportedSize(), and the cooked size is unknowable without loading. Metric names say
	 * SourceWidth/SourceHeight for the same reason.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Texture", meta = (ClampMin = 1))
	int32 MaxSourceDimension = 2048;

	UPROPERTY(EditAnywhere, Config, Category = "Texture", meta = (ClampMin = 1))
	int32 CriticalSourceDimension = 4096;

	/**
	 * When true, an oversize source drops to Low severity if MaxTextureSize clamps it or the
	 * LODGroup is exempt — because the registry cannot tell us the cooked size, and flagging a
	 * legitimately clamped 8K source at High is exactly how an audit tool gets ignored.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Texture")
	bool bRespectMaxTextureSize = true;

	UPROPERTY(EditAnywhere, Config, Category = "Texture")
	TArray<FName> ExemptLODGroups;

	/** Deep tier: CalcTextureMemorySizeEnum(TMC_AllMips). */
	UPROPERTY(EditAnywhere, Config, Category = "Texture", meta = (ClampMin = 0))
	int64 MaxMemoryBytes = 8388608;

	UPROPERTY(EditAnywhere, Config, Category = "Texture", meta = (ClampMin = 0))
	int64 CriticalMemoryBytes = 33554432;

	UPROPERTY(EditAnywhere, Config, Category = "Texture")
	bool bFlagNonPowerOfTwo = true;

	UPROPERTY(EditAnywhere, Config, Category = "Texture")
	bool bFlagMissingMips = true;

	UPROPERTY(EditAnywhere, Config, Category = "Texture")
	bool bFlagNeverStream = true;

	/** At or above this source dimension, virtual texturing is expected. 0 disables the rule. */
	UPROPERTY(EditAnywhere, Config, Category = "Texture", meta = (ClampMin = 0))
	int32 VirtualTextureExpectedAtDim = 4096;
};

USTRUCT()
struct FPulseStaticMeshThresholds
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, Category = "StaticMesh", meta = (ClampMin = 1))
	int32 MaxTrianglesNonNanite = 30000;

	UPROPERTY(EditAnywhere, Config, Category = "StaticMesh", meta = (ClampMin = 1))
	int32 CriticalTrianglesNonNanite = 150000;

	/** Below this triangle count, LODs are not expected and the MissingLODs rule stays quiet. */
	UPROPERTY(EditAnywhere, Config, Category = "StaticMesh", meta = (ClampMin = 0))
	int32 LODRequiredAboveTriangles = 2000;

	UPROPERTY(EditAnywhere, Config, Category = "StaticMesh", meta = (ClampMin = 1))
	int32 MinLODsForComplexMesh = 3;

	UPROPERTY(EditAnywhere, Config, Category = "StaticMesh", meta = (ClampMin = 1))
	int32 MaxMaterialSlots = 6;

	UPROPERTY(EditAnywhere, Config, Category = "StaticMesh", meta = (ClampMin = 1))
	int32 MaxUVChannels = 3;

	/** Deep: complex-as-simple collision on a rendered mesh is a per-trace cost. */
	UPROPERTY(EditAnywhere, Config, Category = "StaticMesh")
	bool bFlagComplexAsSimple = true;

	UPROPERTY(EditAnywhere, Config, Category = "StaticMesh", meta = (ClampMin = 1))
	int32 MaxCollisionPrimitives = 16;

	UPROPERTY(EditAnywhere, Config, Category = "StaticMesh", meta = (ClampMin = 0))
	int64 MaxDistanceFieldBytes = 4194304;
};

USTRUCT()
struct FPulseSkeletalMeshThresholds
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, Category = "SkeletalMesh", meta = (ClampMin = 1))
	int32 MaxBones = 150;

	UPROPERTY(EditAnywhere, Config, Category = "SkeletalMesh", meta = (ClampMin = 1))
	int32 CriticalBones = 256;

	UPROPERTY(EditAnywhere, Config, Category = "SkeletalMesh", meta = (ClampMin = 1))
	int32 MaxTriangles = 60000;

	UPROPERTY(EditAnywhere, Config, Category = "SkeletalMesh", meta = (ClampMin = 1))
	int32 MinLODs = 3;

	UPROPERTY(EditAnywhere, Config, Category = "SkeletalMesh", meta = (ClampMin = 1))
	int32 MaxMorphTargets = 64;

	UPROPERTY(EditAnywhere, Config, Category = "SkeletalMesh")
	bool bRequirePhysicsAsset = true;
};

USTRUCT()
struct FPulseMaterialThresholds
{
	GENERATED_BODY()

	/** ShaderStats tier only. */
	UPROPERTY(EditAnywhere, Config, Category = "Material", meta = (ClampMin = 1))
	int32 MaxPixelShaderInstructions = 250;

	UPROPERTY(EditAnywhere, Config, Category = "Material", meta = (ClampMin = 1))
	int32 CriticalPixelShaderInstructions = 500;

	UPROPERTY(EditAnywhere, Config, Category = "Material", meta = (ClampMin = 1))
	int32 MaxVertexShaderInstructions = 150;

	UPROPERTY(EditAnywhere, Config, Category = "Material", meta = (ClampMin = 1))
	int32 MaxSamplers = 13;

	/** Deep tier, from GetAllStaticSwitchParameterInfo(): 2^N permutations compile per usage. */
	UPROPERTY(EditAnywhere, Config, Category = "Material", meta = (ClampMin = 1))
	int32 MaxStaticSwitches = 8;

	UPROPERTY(EditAnywhere, Config, Category = "Material")
	bool bFlagTranslucentTwoSided = true;

	/** Masked blend mode defeats early-Z; flag it so it is at least a deliberate choice. */
	UPROPERTY(EditAnywhere, Config, Category = "Material")
	bool bFlagMaskedBlendMode = false;
};

USTRUCT()
struct FPulseBlueprintThresholds
{
	GENERATED_BODY()

	/** Deep: the biggest single Blueprint perf lever there is. */
	UPROPERTY(EditAnywhere, Config, Category = "Blueprint")
	bool bFlagTickEnabledByDefault = true;

	/** Ticking slower than this is considered intentional and not flagged. 0 = flag any tick. */
	UPROPERTY(EditAnywhere, Config, Category = "Blueprint", meta = (ClampMin = 0.0))
	float AcceptableTickInterval = 0.0f;

	UPROPERTY(EditAnywhere, Config, Category = "Blueprint", meta = (ClampMin = 1))
	int32 MaxComponents = 20;

	UPROPERTY(EditAnywhere, Config, Category = "Blueprint", meta = (ClampMin = 1))
	int32 MaxReplicatedProperties = 24;

	/** Data-only Blueprints are near-free; skip them so the report is signal, not census. */
	UPROPERTY(EditAnywhere, Config, Category = "Blueprint")
	bool bSkipDataOnlyBlueprints = true;
};

USTRUCT()
struct FPulseNiagaraThresholds
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, Category = "Niagara", meta = (ClampMin = 1))
	int32 MaxEmitters = 8;

	UPROPERTY(EditAnywhere, Config, Category = "Niagara", meta = (ClampMin = 1))
	int32 MaxRenderers = 12;

	/** A GPU sim without fixed bounds is a genuine stall source, and it is a registry tag. */
	UPROPERTY(EditAnywhere, Config, Category = "Niagara")
	bool bFlagGPUMissingBounds = true;

	UPROPERTY(EditAnywhere, Config, Category = "Niagara")
	bool bRequireEffectType = true;

	/**
	 * Flags MaxPoolSize == 0 && PoolPrimeSize == 0. The engine has no bPoolingEnabled — the real
	 * switch is the per-spawn ENCPoolMethod — so this rule can only say "this system cannot be
	 * pooled", never "pooling is off". The rule text says exactly that.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Niagara")
	bool bFlagNonPoolableSystems = true;

	UPROPERTY(EditAnywhere, Config, Category = "Niagara", meta = (ClampMin = 0.0))
	float MaxWarmupTimeSeconds = 1.0f;
};

USTRUCT()
struct FPulseLevelThresholds
{
	GENERATED_BODY()

	/** Fast tier for World Partition maps via actor descriptors; Deep for everything else. */
	UPROPERTY(EditAnywhere, Config, Category = "Level", meta = (ClampMin = 1))
	int32 MaxActors = 5000;

	UPROPERTY(EditAnywhere, Config, Category = "Level", meta = (ClampMin = 1))
	int32 CriticalActors = 20000;

	/** Above this actor count, World Partition is expected. 0 disables the rule. */
	UPROPERTY(EditAnywhere, Config, Category = "Level", meta = (ClampMin = 0))
	int32 PartitionExpectedAboveActors = 2000;

	UPROPERTY(EditAnywhere, Config, Category = "Level")
	bool bFlagStreamingDisabled = true;
};

/**
 * Project configuration for Pulse audits. Stored in Config/DefaultPulse.ini under
 * [/Script/Pulse.PulseSettings], checked in so CI and the editor agree on what "healthy" means.
 *
 * NOTE Config = Pulse, not Config = Game: UObject::GetDefaultConfigFilename() builds the filename
 * as "Default" + ClassConfigName + ".ini", so only ClassConfigName == Pulse produces
 * DefaultPulse.ini. Unknown config branch names are fully supported by the config system.
 */
UCLASS(Config = Pulse, DefaultConfig, meta = (DisplayName = "Pulse"))
class PULSE_API UPulseSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPulseSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	UPROPERTY(EditAnywhere, Config, Category = "Scan")
	FPulseScanSettings Scan;

	UPROPERTY(EditAnywhere, Config, Category = "Scoring")
	FPulseScoringSettings Scoring;

	UPROPERTY(EditAnywhere, Config, Category = "Thresholds")
	FPulseTextureThresholds Texture;

	UPROPERTY(EditAnywhere, Config, Category = "Thresholds")
	FPulseStaticMeshThresholds StaticMesh;

	UPROPERTY(EditAnywhere, Config, Category = "Thresholds")
	FPulseSkeletalMeshThresholds SkeletalMesh;

	UPROPERTY(EditAnywhere, Config, Category = "Thresholds")
	FPulseMaterialThresholds Material;

	UPROPERTY(EditAnywhere, Config, Category = "Thresholds")
	FPulseBlueprintThresholds Blueprint;

	UPROPERTY(EditAnywhere, Config, Category = "Thresholds")
	FPulseNiagaraThresholds Niagara;

	UPROPERTY(EditAnywhere, Config, Category = "Thresholds")
	FPulseLevelThresholds Level;

	/** Rules suppressed entirely. Lets a project opt out of a rule without a code change or a fork. */
	UPROPERTY(EditAnywhere, Config, Category = "Rules")
	TArray<FName> DisabledRules;

	/** Per-rule severity overrides, applied by the driver after Evaluate. The main tuning hatch. */
	UPROPERTY(EditAnywhere, Config, Category = "Rules")
	TMap<FName, EPulseSeverity> RuleSeverityOverrides;

	/**
	 * MD5 over every config property, via reflection export in declaration order. Written into
	 * the report header so a settings edit is one visible header line, not unexplained row churn.
	 */
	FString ComputeSettingsHash() const;
};
