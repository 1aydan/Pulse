// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseSkeletalMeshCollector.h"

#include "AssetRegistry/AssetData.h"
#include "Engine/SkeletalMesh.h"
#include "PulseCollectorRegistry.h"
#include "PulseSettings.h"
#include "PulseTagUtils.h"

// Metric and rule names as TCHAR* rather than static FName: file-scope FName construction runs
// before main, when the name table may not be up. FName conversion happens inside the methods,
// which only run after StartupModule.
static const TCHAR* GPulseSkelMetricBones = TEXT("Bones");
static const TCHAR* GPulseSkelMetricNumLODs = TEXT("NumLODs");
static const TCHAR* GPulseSkelMetricMorphTargets = TEXT("MorphTargets");
static const TCHAR* GPulseSkelMetricTriangles = TEXT("Triangles");
static const TCHAR* GPulseSkelMetricVertices = TEXT("Vertices");
static const TCHAR* GPulseSkelMetricNaniteEnabled = TEXT("NaniteEnabled");
static const TCHAR* GPulseSkelMetricEstCompressedBytes = TEXT("EstCompressedBytes");
static const TCHAR* GPulseSkelMetricHasPhysicsAsset = TEXT("HasPhysicsAsset");

static const TCHAR* GPulseSkelRuleExcessiveBones = TEXT("SkeletalMesh.ExcessiveBones");
static const TCHAR* GPulseSkelRuleMissingLODs = TEXT("SkeletalMesh.MissingLODs");
static const TCHAR* GPulseSkelRuleExcessiveTriangles = TEXT("SkeletalMesh.ExcessiveTriangles");
static const TCHAR* GPulseSkelRuleExcessiveMorphTargets = TEXT("SkeletalMesh.ExcessiveMorphTargets");
static const TCHAR* GPulseSkelRuleMissingPhysicsAsset = TEXT("SkeletalMesh.MissingPhysicsAsset");

static void PulseAddSkeletalMeshIssue(FPulseAssetResult& InOutResult, const TCHAR* RuleId, EPulseSeverity Severity, FString Message)
{
	FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
	Issue.RuleId = FName(RuleId);
	Issue.Severity = Severity;
	Issue.Message = MoveTemp(Message);
	// Recommendation and DetectedAtTier are filled by the driver's post-process.
}

FName FPulseSkeletalMeshCollector::GetCollectorName() const
{
	return FName(TEXT("Pulse.SkeletalMesh"));
}

FName FPulseSkeletalMeshCollector::GetCategory() const
{
	return FName(TEXT("SkeletalMesh"));
}

UClass* FPulseSkeletalMeshCollector::GetSupportedClass() const
{
	return USkeletalMesh::StaticClass();
}

bool FPulseSkeletalMeshCollector::SupportsSubclasses() const
{
	return true;
}

EPulseTier FPulseSkeletalMeshCollector::GetMaxUsefulTier() const
{
	// Everything a rule consumes is a registry tag; reporting Fast here is what keeps skeletal
	// meshes out of the -deep load loop entirely.
	return EPulseTier::Fast;
}

void FPulseSkeletalMeshCollector::GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const
{
	FPulseMetricDesc& Bones = OutMetrics.AddDefaulted_GetRef();
	Bones.Name = FName(GPulseSkelMetricBones);
	Bones.Kind = EPulseMetricKind::Int;
	Bones.Unit = EPulseMetricUnit::Bones;
	Bones.MinTier = EPulseTier::Fast;
	Bones.Description = TEXT("Reference-skeleton raw bone count.");

	FPulseMetricDesc& NumLODs = OutMetrics.AddDefaulted_GetRef();
	NumLODs.Name = FName(GPulseSkelMetricNumLODs);
	NumLODs.Kind = EPulseMetricKind::Int;
	NumLODs.Unit = EPulseMetricUnit::Count;
	NumLODs.MinTier = EPulseTier::Fast;
	NumLODs.Description = TEXT("Number of skeletal mesh LODs.");

	FPulseMetricDesc& MorphTargets = OutMetrics.AddDefaulted_GetRef();
	MorphTargets.Name = FName(GPulseSkelMetricMorphTargets);
	MorphTargets.Kind = EPulseMetricKind::Int;
	MorphTargets.Unit = EPulseMetricUnit::Count;
	MorphTargets.MinTier = EPulseTier::Fast;
	MorphTargets.Description = TEXT("Number of morph targets carried by the mesh.");

	FPulseMetricDesc& Triangles = OutMetrics.AddDefaulted_GetRef();
	Triangles.Name = FName(GPulseSkelMetricTriangles);
	Triangles.Kind = EPulseMetricKind::Int;
	Triangles.Unit = EPulseMetricUnit::Triangles;
	Triangles.MinTier = EPulseTier::Fast;
	Triangles.Description = TEXT("LOD0 triangle count.");

	FPulseMetricDesc& Vertices = OutMetrics.AddDefaulted_GetRef();
	Vertices.Name = FName(GPulseSkelMetricVertices);
	Vertices.Kind = EPulseMetricKind::Int;
	Vertices.Unit = EPulseMetricUnit::Vertices;
	Vertices.MinTier = EPulseTier::Fast;
	Vertices.Description = TEXT("LOD0 render vertex count.");

	FPulseMetricDesc& NaniteEnabled = OutMetrics.AddDefaulted_GetRef();
	NaniteEnabled.Name = FName(GPulseSkelMetricNaniteEnabled);
	NaniteEnabled.Kind = EPulseMetricKind::Bool;
	NaniteEnabled.Unit = EPulseMetricUnit::None;
	NaniteEnabled.MinTier = EPulseTier::Fast;
	NaniteEnabled.Description = TEXT("True when Nanite is enabled for this skeletal mesh. Absent on registries written before the tag existed.");

	FPulseMetricDesc& EstCompressedBytes = OutMetrics.AddDefaulted_GetRef();
	EstCompressedBytes.Name = FName(GPulseSkelMetricEstCompressedBytes);
	EstCompressedBytes.Kind = EPulseMetricKind::Int;
	EstCompressedBytes.Unit = EPulseMetricUnit::Bytes;
	EstCompressedBytes.MinTier = EPulseTier::Fast;
	EstCompressedBytes.Description = TEXT("Engine-estimated total compressed size from the EstTotalCompressedSize tag.");

	FPulseMetricDesc& HasPhysicsAsset = OutMetrics.AddDefaulted_GetRef();
	HasPhysicsAsset.Name = FName(GPulseSkelMetricHasPhysicsAsset);
	HasPhysicsAsset.Kind = EPulseMetricKind::Bool;
	HasPhysicsAsset.Unit = EPulseMetricUnit::None;
	HasPhysicsAsset.MinTier = EPulseTier::Fast;
	HasPhysicsAsset.Description = TEXT("True when a physics asset is assigned to the mesh.");
}

void FPulseSkeletalMeshCollector::GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const
{
	FPulseRuleDesc& ExcessiveBones = OutRules.AddDefaulted_GetRef();
	ExcessiveBones.RuleId = FName(GPulseSkelRuleExcessiveBones);
	ExcessiveBones.DefaultSeverity = EPulseSeverity::Medium;
	ExcessiveBones.MinTier = EPulseTier::Fast;
	ExcessiveBones.Description = TEXT("Reference-skeleton bone count exceeds MaxBones; escalates to High above CriticalBones.");
	ExcessiveBones.Recommendation = TEXT("Remove or merge helper/twist bones that are never animated, or retarget to a smaller skeleton. For per-LOD savings use Remove Bones in the Skeletal Mesh editor's LOD settings.");

	FPulseRuleDesc& MissingLODs = OutRules.AddDefaulted_GetRef();
	MissingLODs.RuleId = FName(GPulseSkelRuleMissingLODs);
	MissingLODs.DefaultSeverity = EPulseSeverity::Medium;
	MissingLODs.MinTier = EPulseTier::Fast;
	MissingLODs.Description = TEXT("Fewer LODs than MinLODs.");
	MissingLODs.Recommendation = TEXT("Generate additional LODs in the Skeletal Mesh editor (LOD Settings > Number of LODs) or assign a shared LOD Settings asset, so distant instances skin fewer vertices.");

	FPulseRuleDesc& ExcessiveTriangles = OutRules.AddDefaulted_GetRef();
	ExcessiveTriangles.RuleId = FName(GPulseSkelRuleExcessiveTriangles);
	ExcessiveTriangles.DefaultSeverity = EPulseSeverity::Medium;
	ExcessiveTriangles.MinTier = EPulseTier::Fast;
	ExcessiveTriangles.Description = TEXT("LOD0 triangle count exceeds MaxTriangles on a non-Nanite mesh.");
	ExcessiveTriangles.Recommendation = TEXT("Reduce the LOD0 triangle count in the DCC or via the Skeletal Mesh editor's reduction settings, or enable Nanite on the mesh.");

	FPulseRuleDesc& ExcessiveMorphTargets = OutRules.AddDefaulted_GetRef();
	ExcessiveMorphTargets.RuleId = FName(GPulseSkelRuleExcessiveMorphTargets);
	ExcessiveMorphTargets.DefaultSeverity = EPulseSeverity::Low;
	ExcessiveMorphTargets.MinTier = EPulseTier::Fast;
	ExcessiveMorphTargets.Description = TEXT("Morph target count exceeds MaxMorphTargets.");
	ExcessiveMorphTargets.Recommendation = TEXT("Strip morph targets that are never driven by animation or code — remove them from the DCC export or deselect them on re-import. Each one costs memory and GPU skinning time.");

	FPulseRuleDesc& MissingPhysicsAsset = OutRules.AddDefaulted_GetRef();
	MissingPhysicsAsset.RuleId = FName(GPulseSkelRuleMissingPhysicsAsset);
	MissingPhysicsAsset.DefaultSeverity = EPulseSeverity::Low;
	MissingPhysicsAsset.MinTier = EPulseTier::Fast;
	MissingPhysicsAsset.Description = TEXT("No physics asset is assigned and bRequirePhysicsAsset is set.");
	MissingPhysicsAsset.Recommendation = TEXT("Create and assign one: right-click the mesh in the Content Browser > Create > Physics Asset, then set it in the Skeletal Mesh editor's Asset Details. Without it the mesh cannot ragdoll or take per-bone hits.");
}

void FPulseSkeletalMeshCollector::CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FAssetData& AssetData = Context.AssetData;

	PulseAddIntTagMetric(InOutResult, AssetData, FName(TEXT("Bones")), FName(GPulseSkelMetricBones), EPulseMetricUnit::Bones);
	PulseAddIntTagMetric(InOutResult, AssetData, FName(TEXT("LODs")), FName(GPulseSkelMetricNumLODs), EPulseMetricUnit::Count);
	PulseAddIntTagMetric(InOutResult, AssetData, FName(TEXT("MorphTargets")), FName(GPulseSkelMetricMorphTargets), EPulseMetricUnit::Count);
	PulseAddIntTagMetric(InOutResult, AssetData, FName(TEXT("Triangles")), FName(GPulseSkelMetricTriangles), EPulseMetricUnit::Triangles);
	PulseAddIntTagMetric(InOutResult, AssetData, FName(TEXT("Vertices")), FName(GPulseSkelMetricVertices), EPulseMetricUnit::Vertices);

	// NaniteEnabled is read manually rather than through PulseAddBoolTagMetric: the engine writes
	// it only under WITH_EDITORONLY_DATA, so registries written by older saves can lack it without
	// being stale. Absence must not count against the trust rollup, and the triangle rule simply
	// treats an absent flag as not-exempt.
	bool bNaniteEnabled = false;
	if (PulseGetTagBool(AssetData, FName(TEXT("NaniteEnabled")), bNaniteEnabled))
	{
		InOutResult.Metrics.Add(FPulseMetric::MakeBool(FName(GPulseSkelMetricNaniteEnabled), bNaniteEnabled, EPulseTier::Fast));
	}

	// The engine currently writes 0 here in both 5.7 and 5.8 — the Nanite-skinning size estimate
	// behind it is `#if ... && 0` in SkeletalMesh.cpp. Recorded anyway so reports pick the number
	// up the day the engine fills it in; no rule consumes it.
	PulseAddIntTagMetric(InOutResult, AssetData, FName(TEXT("EstTotalCompressedSize")), FName(GPulseSkelMetricEstCompressedBytes), EPulseMetricUnit::Bytes);

	// PhysicsAsset is an AssetRegistrySearchable UPROPERTY on USkeletalMesh, so the tag should
	// always be present — "None" when unassigned, an object-path export when assigned. Absence
	// means a stale registry, which is a trust problem, not a missing physics asset: emit no
	// metric so MissingPhysicsAsset cannot fire on bad data.
	FString PhysicsAssetPath;
	if (PulseGetTagString(AssetData, FName(TEXT("PhysicsAsset")), PhysicsAssetPath))
	{
		const bool bHasPhysicsAsset = !PhysicsAssetPath.IsEmpty() && PhysicsAssetPath != TEXT("None");
		InOutResult.Metrics.Add(FPulseMetric::MakeBool(FName(GPulseSkelMetricHasPhysicsAsset), bHasPhysicsAsset, EPulseTier::Fast));
	}
	else
	{
		InOutResult.NumMissingExpectedTags++;
	}
}

void FPulseSkeletalMeshCollector::Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FPulseSkeletalMeshThresholds& Thresholds = Context.Settings.SkeletalMesh;

	int64 Bones = 0;
	if (InOutResult.GetIntMetric(FName(GPulseSkelMetricBones), Bones))
	{
		// One issue at the higher severity, never two.
		if (Bones > Thresholds.CriticalBones)
		{
			PulseAddSkeletalMeshIssue(InOutResult, GPulseSkelRuleExcessiveBones, EPulseSeverity::High,
				FString::Printf(TEXT("%lld bones exceeds CriticalBones %d."), Bones, Thresholds.CriticalBones));
		}
		else if (Bones > Thresholds.MaxBones)
		{
			PulseAddSkeletalMeshIssue(InOutResult, GPulseSkelRuleExcessiveBones, EPulseSeverity::Medium,
				FString::Printf(TEXT("%lld bones exceeds MaxBones %d."), Bones, Thresholds.MaxBones));
		}
	}

	int64 NumLODs = 0;
	if (InOutResult.GetIntMetric(FName(GPulseSkelMetricNumLODs), NumLODs) && NumLODs < Thresholds.MinLODs)
	{
		PulseAddSkeletalMeshIssue(InOutResult, GPulseSkelRuleMissingLODs, EPulseSeverity::Medium,
			FString::Printf(TEXT("%lld LODs is below MinLODs %d."), NumLODs, Thresholds.MinLODs));
	}

	int64 Triangles = 0;
	if (InOutResult.GetIntMetric(FName(GPulseSkelMetricTriangles), Triangles) && Triangles > Thresholds.MaxTriangles)
	{
		// Nanite exempts the mesh from the triangle budget. An absent NaniteEnabled metric leaves
		// bNaniteEnabled false, so meshes from pre-tag registries are held to the budget.
		bool bNaniteEnabled = false;
		InOutResult.GetBoolMetric(FName(GPulseSkelMetricNaniteEnabled), bNaniteEnabled);
		if (!bNaniteEnabled)
		{
			PulseAddSkeletalMeshIssue(InOutResult, GPulseSkelRuleExcessiveTriangles, EPulseSeverity::Medium,
				FString::Printf(TEXT("%lld LOD0 triangles exceeds MaxTriangles %d and Nanite is not enabled."), Triangles, Thresholds.MaxTriangles));
		}
	}

	int64 MorphTargets = 0;
	if (InOutResult.GetIntMetric(FName(GPulseSkelMetricMorphTargets), MorphTargets) && MorphTargets > Thresholds.MaxMorphTargets)
	{
		PulseAddSkeletalMeshIssue(InOutResult, GPulseSkelRuleExcessiveMorphTargets, EPulseSeverity::Low,
			FString::Printf(TEXT("%lld morph targets exceeds MaxMorphTargets %d."), MorphTargets, Thresholds.MaxMorphTargets));
	}

	bool bHasPhysicsAsset = false;
	if (Thresholds.bRequirePhysicsAsset
		&& InOutResult.GetBoolMetric(FName(GPulseSkelMetricHasPhysicsAsset), bHasPhysicsAsset)
		&& !bHasPhysicsAsset)
	{
		PulseAddSkeletalMeshIssue(InOutResult, GPulseSkelRuleMissingPhysicsAsset, EPulseSeverity::Low,
			TEXT("No physics asset is assigned."));
	}
}

static TPulseAutoCollector<FPulseSkeletalMeshCollector> GPulseSkeletalMeshCollectorRegistrar;
