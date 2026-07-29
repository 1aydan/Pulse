// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseStaticMeshCollector.h"

#include "AssetRegistry/AssetData.h"
#include "Engine/StaticMesh.h"
#include "PulseCollectorRegistry.h"
#include "PulseSettings.h"
#include "PulseTagUtils.h"

// Metric names, spelled once. Kept as TCHAR* rather than FName so nothing constructs names during
// static initialisation — the registrar below already defers collector construction for that reason.
static const TCHAR* GPulseSMTriangles             = TEXT("Triangles");
static const TCHAR* GPulseSMVertices              = TEXT("Vertices");
static const TCHAR* GPulseSMNumLODs               = TEXT("NumLODs");
static const TCHAR* GPulseSMMinLOD                = TEXT("MinLOD");
static const TCHAR* GPulseSMMaterialSlots         = TEXT("MaterialSlots");
static const TCHAR* GPulseSMUVChannels            = TEXT("UVChannels");
static const TCHAR* GPulseSMNaniteEnabled         = TEXT("NaniteEnabled");
static const TCHAR* GPulseSMNaniteTriangles       = TEXT("NaniteTriangles");
static const TCHAR* GPulseSMCollisionPrims        = TEXT("CollisionPrims");
static const TCHAR* GPulseSMSectionsWithCollision = TEXT("SectionsWithCollision");
static const TCHAR* GPulseSMCollisionComplexity   = TEXT("CollisionComplexity");
static const TCHAR* GPulseSMEstCompressedBytes    = TEXT("EstCompressedBytes");
static const TCHAR* GPulseSMDistanceFieldBytes    = TEXT("DistanceFieldBytes");

// Rule ids. "<Category>.<Condition>", stable forever.
static const TCHAR* GPulseSMRuleExcessiveTriangles      = TEXT("StaticMesh.ExcessiveTriangles");
static const TCHAR* GPulseSMRuleMissingLODs             = TEXT("StaticMesh.MissingLODs");
static const TCHAR* GPulseSMRuleTooManyMaterialSlots    = TEXT("StaticMesh.TooManyMaterialSlots");
static const TCHAR* GPulseSMRuleTooManyUVChannels       = TEXT("StaticMesh.TooManyUVChannels");
static const TCHAR* GPulseSMRuleComplexAsSimple         = TEXT("StaticMesh.ComplexAsSimpleCollision");
static const TCHAR* GPulseSMRuleExcessiveCollisionPrims = TEXT("StaticMesh.ExcessiveCollisionPrims");
static const TCHAR* GPulseSMRuleLargeDistanceField      = TEXT("StaticMesh.LargeDistanceField");

static void PulseAddStaticMeshIssue(FPulseAssetResult& InOutResult, const TCHAR* RuleId, EPulseSeverity Severity, FString Message)
{
	FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
	Issue.RuleId = FName(RuleId);
	Issue.Severity = Severity;
	Issue.Message = MoveTemp(Message);
	// Recommendation and DetectedAtTier are the driver's to fill; Score is FPulseScoring's.
}

FName FPulseStaticMeshCollector::GetCollectorName() const
{
	return FName(TEXT("Pulse.StaticMesh"));
}

FName FPulseStaticMeshCollector::GetCategory() const
{
	return FName(TEXT("StaticMesh"));
}

UClass* FPulseStaticMeshCollector::GetSupportedClass() const
{
	return UStaticMesh::StaticClass();
}

bool FPulseStaticMeshCollector::SupportsSubclasses() const
{
	return true;
}

EPulseTier FPulseStaticMeshCollector::GetMaxUsefulTier() const
{
	// Everything the rules need is a registry tag. Reporting Fast here is what guarantees this
	// collector never triggers a package load, even when the run itself is -deep.
	return EPulseTier::Fast;
}

void FPulseStaticMeshCollector::GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const
{
	auto AddMetric = [&OutMetrics](const TCHAR* Name, EPulseMetricKind Kind, EPulseMetricUnit Unit, const TCHAR* Description)
	{
		FPulseMetricDesc& Desc = OutMetrics.AddDefaulted_GetRef();
		Desc.Name = FName(Name);
		Desc.Kind = Kind;
		Desc.Unit = Unit;
		Desc.MinTier = EPulseTier::Fast;
		Desc.Description = Description;
	};

	AddMetric(GPulseSMTriangles, EPulseMetricKind::Int, EPulseMetricUnit::Triangles,
		TEXT("LOD0 render triangle count, from the Triangles registry tag."));
	AddMetric(GPulseSMVertices, EPulseMetricKind::Int, EPulseMetricUnit::Vertices,
		TEXT("LOD0 render vertex count, from the Vertices registry tag."));
	AddMetric(GPulseSMNumLODs, EPulseMetricKind::Int, EPulseMetricUnit::Count,
		TEXT("Number of render LODs, from the LODs registry tag."));
	AddMetric(GPulseSMMinLOD, EPulseMetricKind::Int, EPulseMetricUnit::Count,
		TEXT("Default minimum LOD index used at runtime. Per-platform overrides are not captured."));
	AddMetric(GPulseSMMaterialSlots, EPulseMetricKind::Int, EPulseMetricUnit::Count,
		TEXT("Static material slot count, from the Materials registry tag."));
	AddMetric(GPulseSMUVChannels, EPulseMetricKind::Int, EPulseMetricUnit::Count,
		TEXT("LOD0 UV channel count, from the UVChannels registry tag."));
	AddMetric(GPulseSMNaniteEnabled, EPulseMetricKind::Bool, EPulseMetricUnit::None,
		TEXT("Whether Nanite is enabled for this mesh."));
	AddMetric(GPulseSMNaniteTriangles, EPulseMetricKind::Int, EPulseMetricUnit::Triangles,
		TEXT("Nanite source triangle count. Zero for non-Nanite meshes."));
	AddMetric(GPulseSMCollisionPrims, EPulseMetricKind::Int, EPulseMetricUnit::Count,
		TEXT("Simple collision primitive count on the body setup (boxes, spheres, capsules, convex hulls)."));
	AddMetric(GPulseSMSectionsWithCollision, EPulseMetricKind::Int, EPulseMetricUnit::Count,
		TEXT("Mesh sections with collision enabled."));
	AddMetric(GPulseSMCollisionComplexity, EPulseMetricKind::Text, EPulseMetricUnit::None,
		TEXT("Collision trace flag name, e.g. CTF_UseDefault or CTF_UseComplexAsSimple."));
	AddMetric(GPulseSMEstCompressedBytes, EPulseMetricKind::Int, EPulseMetricUnit::Bytes,
		TEXT("Estimated total compressed render data size, from the EstTotalCompressedSize registry tag."));
	AddMetric(GPulseSMDistanceFieldBytes, EPulseMetricKind::Int, EPulseMetricUnit::Bytes,
		TEXT("Distance field resource size. Absent when distance fields are disabled; absence is not counted as a missing tag."));
}

void FPulseStaticMeshCollector::GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const
{
	auto AddRule = [&OutRules](const TCHAR* RuleId, EPulseSeverity Severity, const TCHAR* Description, const TCHAR* Recommendation)
	{
		FPulseRuleDesc& Desc = OutRules.AddDefaulted_GetRef();
		Desc.RuleId = FName(RuleId);
		Desc.DefaultSeverity = Severity;
		Desc.MinTier = EPulseTier::Fast;
		Desc.Description = Description;
		Desc.Recommendation = Recommendation;
	};

	AddRule(GPulseSMRuleExcessiveTriangles, EPulseSeverity::Medium,
		TEXT("Non-Nanite mesh whose LOD0 triangle count exceeds the budget. Escalates to High above the critical threshold. Nanite meshes are exempt."),
		TEXT("Reduce the LOD0 triangle count in your DCC tool or via the Static Mesh editor reduction settings, or enable Nanite — Nanite meshes are exempt from this budget."));
	AddRule(GPulseSMRuleMissingLODs, EPulseSeverity::Medium,
		TEXT("Non-Nanite mesh complex enough to need LODs but authored with too few."),
		TEXT("Generate LODs in the Static Mesh editor (LOD Settings > Number of LODs, or assign an LOD Group), or enable Nanite so discrete LODs are unnecessary."));
	AddRule(GPulseSMRuleTooManyMaterialSlots, EPulseSeverity::Medium,
		TEXT("More material slots than the per-mesh budget. Every slot is an additional mesh section and draw call per instance."),
		TEXT("Consolidate materials by atlasing textures and merging slots in your DCC tool; every slot removed saves a draw call on every instance."));
	AddRule(GPulseSMRuleTooManyUVChannels, EPulseSeverity::Low,
		TEXT("More UV channels than the budget. Every channel adds vertex buffer size and bandwidth on all LODs."),
		TEXT("Delete unused UV channels in the source asset or the Static Mesh editor; keep only what materials and lightmaps actually sample."));
	AddRule(GPulseSMRuleComplexAsSimple, EPulseSeverity::Medium,
		TEXT("Collision complexity is Use Complex Collision As Simple, so every physics query against this mesh tests render triangles."),
		TEXT("Author simple collision (boxes, spheres, convex hulls) in the Static Mesh editor and set Collision Complexity back to Project Default or Use Simple And Complex."));
	AddRule(GPulseSMRuleExcessiveCollisionPrims, EPulseSeverity::Low,
		TEXT("More simple collision primitives than the per-body budget; each primitive is tested on every query against the body."),
		TEXT("Rebuild collision with fewer primitives — auto convex with a lower hull count, or a handful of hand-placed boxes and spheres in the Static Mesh editor."));
	AddRule(GPulseSMRuleLargeDistanceField, EPulseSeverity::Low,
		TEXT("Mesh distance field resource exceeds the byte budget."),
		TEXT("Lower Distance Field Resolution Scale in the Static Mesh editor Build Settings, or disable Generate Mesh Distance Field if this mesh never contributes to distance field AO or shadows."));
}

void FPulseStaticMeshCollector::CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FAssetData& Asset = Context.AssetData;

	// Every helper call reads one registry tag. Absence means a stale registry (UStaticMesh's tag
	// writer early-outs while compiling), so the helper skips the metric and bumps the trust
	// counter instead of recording a lying zero.
	PulseAddIntTagMetric(InOutResult, Asset, TEXT("Triangles"), GPulseSMTriangles, EPulseMetricUnit::Triangles);
	PulseAddIntTagMetric(InOutResult, Asset, TEXT("Vertices"), GPulseSMVertices, EPulseMetricUnit::Vertices);
	PulseAddIntTagMetric(InOutResult, Asset, TEXT("LODs"), GPulseSMNumLODs, EPulseMetricUnit::Count);

	// The MinLOD tag is FPerPlatformInt::ToString() — "0", or "0, Switch=2" with overrides. The int
	// parser reads the leading default value, which is exactly the number we want.
	PulseAddIntTagMetric(InOutResult, Asset, TEXT("MinLOD"), GPulseSMMinLOD, EPulseMetricUnit::Count);

	PulseAddIntTagMetric(InOutResult, Asset, TEXT("Materials"), GPulseSMMaterialSlots, EPulseMetricUnit::Count);
	PulseAddIntTagMetric(InOutResult, Asset, TEXT("UVChannels"), GPulseSMUVChannels, EPulseMetricUnit::Count);
	PulseAddBoolTagMetric(InOutResult, Asset, TEXT("NaniteEnabled"), GPulseSMNaniteEnabled);

	// Written for every mesh (zero when Nanite is off), so it is an expected tag like the rest.
	PulseAddIntTagMetric(InOutResult, Asset, TEXT("NaniteTriangles"), GPulseSMNaniteTriangles, EPulseMetricUnit::Triangles);

	PulseAddIntTagMetric(InOutResult, Asset, TEXT("CollisionPrims"), GPulseSMCollisionPrims, EPulseMetricUnit::Count);
	PulseAddIntTagMetric(InOutResult, Asset, TEXT("SectionsWithCollision"), GPulseSMSectionsWithCollision, EPulseMetricUnit::Count);
	PulseAddTextTagMetric(InOutResult, Asset, TEXT("CollisionComplexity"), GPulseSMCollisionComplexity, /*bExpected*/ true);
	PulseAddIntTagMetric(InOutResult, Asset, TEXT("EstTotalCompressedSize"), GPulseSMEstCompressedBytes, EPulseMetricUnit::Bytes);

	// Distance fields are legitimately disabled on many projects and meshes, so an absent
	// DistanceFieldSize tag is not evidence of a stale registry. Record only when present, and do
	// not count absence against the trust rollup.
	int64 DistanceFieldBytes = 0;
	if (PulseGetTagInt(Asset, TEXT("DistanceFieldSize"), DistanceFieldBytes))
	{
		InOutResult.Metrics.Add(FPulseMetric::MakeInt(GPulseSMDistanceFieldBytes, DistanceFieldBytes, EPulseMetricUnit::Bytes, EPulseTier::Fast));
	}
}

void FPulseStaticMeshCollector::Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FPulseStaticMeshThresholds& Thresholds = Context.Settings.StaticMesh;

	int64 Triangles = 0;
	const bool bHasTriangles = InOutResult.GetIntMetric(FName(GPulseSMTriangles), Triangles);

	bool bNaniteEnabled = false;
	const bool bHasNaniteFlag = InOutResult.GetBoolMetric(FName(GPulseSMNaniteEnabled), bNaniteEnabled);

	// StaticMesh.ExcessiveTriangles — Nanite meshes are exempt, and an absent NaniteEnabled metric
	// means unknown, not "off": the rule needs a definite non-Nanite answer before it may fire.
	if (bHasTriangles && bHasNaniteFlag && !bNaniteEnabled)
	{
		if (Triangles > Thresholds.CriticalTrianglesNonNanite)
		{
			PulseAddStaticMeshIssue(InOutResult, GPulseSMRuleExcessiveTriangles, EPulseSeverity::High,
				FString::Printf(TEXT("%lld LOD0 triangles on a non-Nanite mesh exceeds CriticalTrianglesNonNanite %d."),
					Triangles, Thresholds.CriticalTrianglesNonNanite));
		}
		else if (Triangles > Thresholds.MaxTrianglesNonNanite)
		{
			PulseAddStaticMeshIssue(InOutResult, GPulseSMRuleExcessiveTriangles, EPulseSeverity::Medium,
				FString::Printf(TEXT("%lld LOD0 triangles on a non-Nanite mesh exceeds MaxTrianglesNonNanite %d."),
					Triangles, Thresholds.MaxTrianglesNonNanite));
		}
	}

	// StaticMesh.MissingLODs
	int64 NumLODs = 0;
	if (bHasTriangles && bHasNaniteFlag && !bNaniteEnabled && InOutResult.GetIntMetric(FName(GPulseSMNumLODs), NumLODs))
	{
		if (Triangles > Thresholds.LODRequiredAboveTriangles && NumLODs < Thresholds.MinLODsForComplexMesh)
		{
			PulseAddStaticMeshIssue(InOutResult, GPulseSMRuleMissingLODs, EPulseSeverity::Medium,
				FString::Printf(TEXT("%lld LOD0 triangles but only %lld LODs; meshes above %d triangles are expected to have at least %d."),
					Triangles, NumLODs, Thresholds.LODRequiredAboveTriangles, Thresholds.MinLODsForComplexMesh));
		}
	}

	// StaticMesh.TooManyMaterialSlots
	int64 MaterialSlots = 0;
	if (InOutResult.GetIntMetric(FName(GPulseSMMaterialSlots), MaterialSlots) && MaterialSlots > Thresholds.MaxMaterialSlots)
	{
		PulseAddStaticMeshIssue(InOutResult, GPulseSMRuleTooManyMaterialSlots, EPulseSeverity::Medium,
			FString::Printf(TEXT("%lld material slots exceeds MaxMaterialSlots %d."),
				MaterialSlots, Thresholds.MaxMaterialSlots));
	}

	// StaticMesh.TooManyUVChannels
	int64 UVChannels = 0;
	if (InOutResult.GetIntMetric(FName(GPulseSMUVChannels), UVChannels) && UVChannels > Thresholds.MaxUVChannels)
	{
		PulseAddStaticMeshIssue(InOutResult, GPulseSMRuleTooManyUVChannels, EPulseSeverity::Low,
			FString::Printf(TEXT("%lld UV channels exceeds MaxUVChannels %d."),
				UVChannels, Thresholds.MaxUVChannels));
	}

	// StaticMesh.ComplexAsSimpleCollision — the tag value is LexToString(ECollisionTraceFlag),
	// which stringifies the raw enum identifier, so "CTF_UseComplexAsSimple" is the verbatim
	// carried value. CTF_UseDefault could still resolve to complex-as-simple through the project
	// default, but the registry cannot see project physics settings, so only the explicit per-mesh
	// choice fires.
	if (Thresholds.bFlagComplexAsSimple)
	{
		if (const FPulseMetric* Complexity = InOutResult.FindMetric(FName(GPulseSMCollisionComplexity)))
		{
			const FString* ComplexityText = Complexity->Value.TryGet<FString>();
			if (ComplexityText != nullptr && ComplexityText->Equals(TEXT("CTF_UseComplexAsSimple"), ESearchCase::CaseSensitive))
			{
				PulseAddStaticMeshIssue(InOutResult, GPulseSMRuleComplexAsSimple, EPulseSeverity::Medium,
					TEXT("Collision complexity is CTF_UseComplexAsSimple: every physics query against this mesh tests render triangles instead of simple primitives."));
			}
		}
	}

	// StaticMesh.ExcessiveCollisionPrims
	int64 CollisionPrims = 0;
	if (InOutResult.GetIntMetric(FName(GPulseSMCollisionPrims), CollisionPrims) && CollisionPrims > Thresholds.MaxCollisionPrimitives)
	{
		PulseAddStaticMeshIssue(InOutResult, GPulseSMRuleExcessiveCollisionPrims, EPulseSeverity::Low,
			FString::Printf(TEXT("%lld collision primitives exceeds MaxCollisionPrimitives %d."),
				CollisionPrims, Thresholds.MaxCollisionPrimitives));
	}

	// StaticMesh.LargeDistanceField
	int64 DistanceFieldBytes = 0;
	if (InOutResult.GetIntMetric(FName(GPulseSMDistanceFieldBytes), DistanceFieldBytes) && DistanceFieldBytes > Thresholds.MaxDistanceFieldBytes)
	{
		PulseAddStaticMeshIssue(InOutResult, GPulseSMRuleLargeDistanceField, EPulseSeverity::Low,
			FString::Printf(TEXT("%lld distance field bytes exceeds MaxDistanceFieldBytes %lld."),
				DistanceFieldBytes, Thresholds.MaxDistanceFieldBytes));
	}
}

// Self-registration: FPulseModule::StartupModule drains the factory list, so adding this collector
// touched no existing file.
static TPulseAutoCollector<FPulseStaticMeshCollector> GPulseStaticMeshCollectorRegistrar;
