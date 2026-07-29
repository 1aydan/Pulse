// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseMaterialCollector.h"
#include "PulseCollectorRegistry.h"
#include "PulseSettings.h"
#include "PulseTagUtils.h"

#include "AssetRegistry/AssetData.h"
#include "Engine/EngineTypes.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
// MaterialInterface.h only forward-declares FMaterialParameterInfo; TArray needs the full type.
#include "Materials/MaterialParameters.h"

static TPulseAutoCollector<FPulseMaterialCollector> GPulseMaterialCollectorRegistrar;

// Metric names, single-sourced so schema, Collect*, and Evaluate can never drift apart.
static const TCHAR* GPulseMatDomain = TEXT("Domain");
static const TCHAR* GPulseMatBlendMode = TEXT("BlendMode");
static const TCHAR* GPulseMatShadingModel = TEXT("ShadingModel");
static const TCHAR* GPulseMatParentPath = TEXT("ParentPath");
static const TCHAR* GPulseMatHasSceneColor = TEXT("HasSceneColor");
static const TCHAR* GPulseMatStaticSwitchCount = TEXT("StaticSwitchCount");
static const TCHAR* GPulseMatTwoSided = TEXT("TwoSided");
static const TCHAR* GPulseMatEffectiveBlendMode = TEXT("EffectiveBlendMode");
static const TCHAR* GPulseMatPixelShaderInstructions = TEXT("PixelShaderInstructions");
static const TCHAR* GPulseMatVertexShaderInstructions = TEXT("VertexShaderInstructions");
static const TCHAR* GPulseMatSamplers = TEXT("Samplers");
static const TCHAR* GPulseMatPixelTextureSamples = TEXT("PixelTextureSamples");
static const TCHAR* GPulseMatVirtualTextureSamples = TEXT("VirtualTextureSamples");

// Rule ids. Stable forever — see FPulseIssue::RuleId.
static const TCHAR* GPulseRuleExcessivePixelInstructions = TEXT("Material.ExcessivePixelInstructions");
static const TCHAR* GPulseRuleExcessiveVertexInstructions = TEXT("Material.ExcessiveVertexInstructions");
static const TCHAR* GPulseRuleExcessiveSamplers = TEXT("Material.ExcessiveSamplers");
static const TCHAR* GPulseRuleExcessiveStaticSwitches = TEXT("Material.ExcessiveStaticSwitches");
static const TCHAR* GPulseRuleTranslucentTwoSided = TEXT("Material.TranslucentTwoSided");
static const TCHAR* GPulseRuleMaskedBlendMode = TEXT("Material.MaskedBlendMode");

// EBlendMode enumerator name in the exact form the EffectiveBlendMode metric stores, so the
// Evaluate comparison and the metric text can never disagree on prefix or casing.
static FString PulseBlendModeName(EBlendMode BlendMode)
{
	return StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(BlendMode));
}

FName FPulseMaterialCollector::GetCollectorName() const
{
	return FName(TEXT("Pulse.Material"));
}

FName FPulseMaterialCollector::GetCategory() const
{
	return FName(TEXT("Material"));
}

UClass* FPulseMaterialCollector::GetSupportedClass() const
{
	return UMaterialInterface::StaticClass();
}

bool FPulseMaterialCollector::SupportsSubclasses() const
{
	// UMaterialInterface is abstract; the real assets are UMaterial and UMaterialInstanceConstant.
	return true;
}

EPulseTier FPulseMaterialCollector::GetMaxUsefulTier() const
{
	return EPulseTier::ShaderStats;
}

EPulseLoadMode FPulseMaterialCollector::GetDeepLoadMode() const
{
	return EPulseLoadMode::Driver;
}

void FPulseMaterialCollector::GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const
{
	OutMetrics.Add({ FName(GPulseMatDomain), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Material domain of a master material (Surface, DeferredDecal, UI, ...), from the MaterialDomain tag. Masters only.") });
	OutMetrics.Add({ FName(GPulseMatBlendMode), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Authored blend mode of a master material, from the BlendMode tag. Masters only; instances may override it — see EffectiveBlendMode.") });
	OutMetrics.Add({ FName(GPulseMatShadingModel), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Authored shading model of a master material, from the ShadingModel tag. Masters only.") });
	OutMetrics.Add({ FName(GPulseMatParentPath), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Object path of an instance's parent material, from the Parent tag. Instances only.") });
	OutMetrics.Add({ FName(GPulseMatHasSceneColor), EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("True when the material graph samples SceneColor, which forces a scene copy. Optional tag: absent on assets saved before the tag existed.") });
	OutMetrics.Add({ FName(GPulseMatStaticSwitchCount), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Deep,
		TEXT("Static switch parameters visible on this material, parent chain included. Each one doubles the shader permutation space per usage.") });
	OutMetrics.Add({ FName(GPulseMatTwoSided), EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Deep,
		TEXT("Effective two-sided flag, resolved through the parent chain.") });
	OutMetrics.Add({ FName(GPulseMatEffectiveBlendMode), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Deep,
		TEXT("Blend mode after instance overrides, resolved through the parent chain. This, not the authored tag, is what renders.") });
	OutMetrics.Add({ FName(GPulseMatPixelShaderInstructions), EPulseMetricKind::Int, EPulseMetricUnit::Instructions, EPulseTier::ShaderStats,
		TEXT("Instruction count of the most expensive pixel shader in the compiled shader map.") });
	OutMetrics.Add({ FName(GPulseMatVertexShaderInstructions), EPulseMetricKind::Int, EPulseMetricUnit::Instructions, EPulseTier::ShaderStats,
		TEXT("Instruction count of the most expensive vertex shader in the compiled shader map.") });
	OutMetrics.Add({ FName(GPulseMatSamplers), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::ShaderStats,
		TEXT("Texture samplers the material requires. The hard API limit is 16 on most platforms.") });
	OutMetrics.Add({ FName(GPulseMatPixelTextureSamples), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::ShaderStats,
		TEXT("Texture sample operations in the pixel shader.") });
	OutMetrics.Add({ FName(GPulseMatVirtualTextureSamples), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::ShaderStats,
		TEXT("Virtual texture sample operations in the material.") });
}

void FPulseMaterialCollector::GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const
{
	OutRules.Add({ FName(GPulseRuleExcessivePixelInstructions), EPulseSeverity::Medium, EPulseTier::ShaderStats,
		TEXT("Pixel shader instruction count exceeds MaxPixelShaderInstructions. Escalates to High past CriticalPixelShaderInstructions."),
		TEXT("Reduce pixel cost: channel-pack textures to cut sample math, replace expression chains with lookup textures, move per-pixel work to the vertex shader via customized UVs, and delete unused feature branches.") });
	OutRules.Add({ FName(GPulseRuleExcessiveVertexInstructions), EPulseSeverity::Medium, EPulseTier::ShaderStats,
		TEXT("Vertex shader instruction count exceeds MaxVertexShaderInstructions."),
		TEXT("Simplify World Position Offset and other vertex-path expressions, or bake per-vertex animation into a texture with the Vertex Animation Tools.") });
	OutRules.Add({ FName(GPulseRuleExcessiveSamplers), EPulseSeverity::Medium, EPulseTier::ShaderStats,
		TEXT("Sampler count exceeds MaxSamplers, approaching the hard API limit of 16 on most platforms."),
		TEXT("Set texture samples to a Shared sampler source (Wrap/Clamp) in the Material Editor, channel-pack textures to reduce distinct samples, or remove unused texture inputs.") });
	OutRules.Add({ FName(GPulseRuleExcessiveStaticSwitches), EPulseSeverity::Medium, EPulseTier::Deep,
		TEXT("Static switch parameter count exceeds MaxStaticSwitches. Each switch doubles the shader permutation space per usage."),
		TEXT("Replace rarely-toggled static switches with separate master materials, a Quality Switch node, or runtime scalar/lerp parameters — each removed switch halves the permutations compiled per usage.") });
	OutRules.Add({ FName(GPulseRuleTranslucentTwoSided), EPulseSeverity::Medium, EPulseTier::Deep,
		TEXT("Material resolves to Translucent blend mode with Two Sided enabled: both faces of every translucent layer are shaded, doubling overdraw."),
		TEXT("Disable Two Sided on the material (or override it back off in the instance), or split the mesh so only the faces that genuinely need both sides use a two-sided material.") });
	OutRules.Add({ FName(GPulseRuleMaskedBlendMode), EPulseSeverity::Info, EPulseTier::Deep,
		TEXT("Material resolves to Masked blend mode, which can defeat early-Z rejection. Informational: masking is often the right choice."),
		TEXT("Confirm masking is required. For fade-out effects use dithered opacity on an Opaque material; for assets that never actually clip, switch to Opaque.") });
}

void FPulseMaterialCollector::CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	// Masters and instances carry disjoint tag sets, so expecting the union would misreport every
	// asset as missing half its tags. Anything that is not exactly UMaterial is treated as an
	// instance: UMaterialInstanceConstant and its subclasses all inherit the Parent UPROPERTY.
	const bool bIsMaster = Context.AssetData.AssetClassPath == UMaterial::StaticClass()->GetClassPathName();
	if (bIsMaster)
	{
		PulseAddTextTagMetric(InOutResult, Context.AssetData, FName(TEXT("MaterialDomain")), FName(GPulseMatDomain));
		PulseAddTextTagMetric(InOutResult, Context.AssetData, FName(TEXT("BlendMode")), FName(GPulseMatBlendMode));
		PulseAddTextTagMetric(InOutResult, Context.AssetData, FName(TEXT("ShadingModel")), FName(GPulseMatShadingModel));
	}
	else
	{
		// The Parent tag is stored in export form (Material'/Game/X.X'); normalize to a plain
		// object path so ParentPath is joinable against other report columns.
		FString ParentExportForm;
		if (PulseGetTagString(Context.AssetData, FName(TEXT("Parent")), ParentExportForm))
		{
			InOutResult.Metrics.Add(FPulseMetric::MakeText(FName(GPulseMatParentPath), PulseNormalizeExportedClassPath(ParentExportForm), EPulseTier::Fast));
		}
		else
		{
			InOutResult.NumMissingExpectedTags++;
		}
	}

	// Optional on both families: the tag only exists on assets saved since it was introduced, so
	// absence is normal history, not registry staleness — no missing-tag count.
	bool bHasSceneColor = false;
	if (PulseGetTagBool(Context.AssetData, FName(TEXT("HasSceneColor")), bHasSceneColor))
	{
		InOutResult.Metrics.Add(FPulseMetric::MakeBool(FName(GPulseMatHasSceneColor), bHasSceneColor, EPulseTier::Fast));
	}
}

void FPulseMaterialCollector::CollectDeep(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(Context.LoadedObject);
	if (MaterialInterface == nullptr)
	{
		return;
	}

	TArray<FMaterialParameterInfo> SwitchInfos;
	TArray<FGuid> SwitchGuids;
	MaterialInterface->GetAllStaticSwitchParameterInfo(SwitchInfos, SwitchGuids);
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(GPulseMatStaticSwitchCount), SwitchInfos.Num(), EPulseMetricUnit::Count, EPulseTier::Deep));

	InOutResult.Metrics.Add(FPulseMetric::MakeBool(FName(GPulseMatTwoSided), MaterialInterface->IsTwoSided(), EPulseTier::Deep));

	// GetBlendMode resolves the parent chain for instances — this is what renders, not the tag.
	InOutResult.Metrics.Add(FPulseMetric::MakeText(FName(GPulseMatEffectiveBlendMode), PulseBlendModeName(MaterialInterface->GetBlendMode()), EPulseTier::Deep));
}

void FPulseMaterialCollector::CollectShaderStats(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(Context.LoadedObject);
	if (MaterialInterface == nullptr)
	{
		return;
	}

	// Cheap by construction: PulseShaderStatsBatch pre-warmed the shader map, so the internal
	// FinishCompilation() returns immediately.
	const FMaterialStatistics Statistics = UMaterialEditingLibrary::GetStatistics(MaterialInterface);

	// An incomplete or failed shader map reports all-zero statistics. Zeros must never become
	// measurements — emit nothing and count the asset against the trust rollup instead.
	const bool bAllZero =
		Statistics.NumPixelShaderInstructions == 0 &&
		Statistics.NumVertexShaderInstructions == 0 &&
		Statistics.NumSamplers == 0 &&
		Statistics.NumPixelTextureSamples == 0 &&
		Statistics.NumVirtualTextureSamples == 0;
	if (bAllZero)
	{
		InOutResult.NumMissingExpectedTags++;
		return;
	}

	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(GPulseMatPixelShaderInstructions), Statistics.NumPixelShaderInstructions, EPulseMetricUnit::Instructions, EPulseTier::ShaderStats));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(GPulseMatVertexShaderInstructions), Statistics.NumVertexShaderInstructions, EPulseMetricUnit::Instructions, EPulseTier::ShaderStats));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(GPulseMatSamplers), Statistics.NumSamplers, EPulseMetricUnit::Count, EPulseTier::ShaderStats));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(GPulseMatPixelTextureSamples), Statistics.NumPixelTextureSamples, EPulseMetricUnit::Count, EPulseTier::ShaderStats));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(FName(GPulseMatVirtualTextureSamples), Statistics.NumVirtualTextureSamples, EPulseMetricUnit::Count, EPulseTier::ShaderStats));
}

void FPulseMaterialCollector::Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FPulseMaterialThresholds& Thresholds = Context.Settings.Material;

	int64 PixelInstructions = 0;
	if (InOutResult.GetIntMetric(FName(GPulseMatPixelShaderInstructions), PixelInstructions))
	{
		// Escalation, not accumulation: one issue at the highest severity the value earns.
		if (PixelInstructions > Thresholds.CriticalPixelShaderInstructions)
		{
			FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
			Issue.RuleId = FName(GPulseRuleExcessivePixelInstructions);
			Issue.Severity = EPulseSeverity::High;
			Issue.Message = FString::Printf(TEXT("%lld pixel shader instructions exceeds CriticalPixelShaderInstructions %d."), PixelInstructions, Thresholds.CriticalPixelShaderInstructions);
			Issue.DetectedAtTier = EPulseTier::ShaderStats;
		}
		else if (PixelInstructions > Thresholds.MaxPixelShaderInstructions)
		{
			FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
			Issue.RuleId = FName(GPulseRuleExcessivePixelInstructions);
			Issue.Severity = EPulseSeverity::Medium;
			Issue.Message = FString::Printf(TEXT("%lld pixel shader instructions exceeds MaxPixelShaderInstructions %d."), PixelInstructions, Thresholds.MaxPixelShaderInstructions);
			Issue.DetectedAtTier = EPulseTier::ShaderStats;
		}
	}

	int64 VertexInstructions = 0;
	if (InOutResult.GetIntMetric(FName(GPulseMatVertexShaderInstructions), VertexInstructions) && VertexInstructions > Thresholds.MaxVertexShaderInstructions)
	{
		FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
		Issue.RuleId = FName(GPulseRuleExcessiveVertexInstructions);
		Issue.Severity = EPulseSeverity::Medium;
		Issue.Message = FString::Printf(TEXT("%lld vertex shader instructions exceeds MaxVertexShaderInstructions %d."), VertexInstructions, Thresholds.MaxVertexShaderInstructions);
		Issue.DetectedAtTier = EPulseTier::ShaderStats;
	}

	int64 Samplers = 0;
	if (InOutResult.GetIntMetric(FName(GPulseMatSamplers), Samplers) && Samplers > Thresholds.MaxSamplers)
	{
		FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
		Issue.RuleId = FName(GPulseRuleExcessiveSamplers);
		Issue.Severity = EPulseSeverity::Medium;
		Issue.Message = FString::Printf(TEXT("%lld samplers exceeds MaxSamplers %d (hard API limit is 16 on most platforms)."), Samplers, Thresholds.MaxSamplers);
		Issue.DetectedAtTier = EPulseTier::ShaderStats;
	}

	int64 StaticSwitchCount = 0;
	if (InOutResult.GetIntMetric(FName(GPulseMatStaticSwitchCount), StaticSwitchCount) && StaticSwitchCount > Thresholds.MaxStaticSwitches)
	{
		FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
		Issue.RuleId = FName(GPulseRuleExcessiveStaticSwitches);
		Issue.Severity = EPulseSeverity::Medium;
		Issue.Message = FString::Printf(TEXT("%lld static switch parameters exceeds MaxStaticSwitches %d."), StaticSwitchCount, Thresholds.MaxStaticSwitches);
		Issue.DetectedAtTier = EPulseTier::Deep;
	}

	// Blend-mode rules compare against the effective (parent-chain-resolved) mode, so a Deep run
	// is required — the Fast BlendMode tag reflects only what the master authored.
	const FPulseMetric* EffectiveBlendModeMetric = InOutResult.FindMetric(FName(GPulseMatEffectiveBlendMode));
	const FString* EffectiveBlendMode = EffectiveBlendModeMetric ? EffectiveBlendModeMetric->Value.TryGet<FString>() : nullptr;
	if (EffectiveBlendMode != nullptr)
	{
		bool bTwoSided = false;
		if (Thresholds.bFlagTranslucentTwoSided
			&& *EffectiveBlendMode == PulseBlendModeName(BLEND_Translucent)
			&& InOutResult.GetBoolMetric(FName(GPulseMatTwoSided), bTwoSided)
			&& bTwoSided)
		{
			FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
			Issue.RuleId = FName(GPulseRuleTranslucentTwoSided);
			Issue.Severity = EPulseSeverity::Medium;
			Issue.Message = FString::Printf(TEXT("Two Sided is enabled with effective blend mode %s."), **EffectiveBlendMode);
			Issue.DetectedAtTier = EPulseTier::Deep;
		}

		if (Thresholds.bFlagMaskedBlendMode && *EffectiveBlendMode == PulseBlendModeName(BLEND_Masked))
		{
			FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
			Issue.RuleId = FName(GPulseRuleMaskedBlendMode);
			Issue.Severity = EPulseSeverity::Info;
			Issue.Message = FString::Printf(TEXT("Effective blend mode is %s."), **EffectiveBlendMode);
			Issue.DetectedAtTier = EPulseTier::Deep;
		}
	}
}
