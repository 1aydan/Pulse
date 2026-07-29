// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseTextureCollector.h"

#include "AssetRegistry/AssetData.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "PulseCollectorRegistry.h"
#include "PulseSettings.h"
#include "PulseTagUtils.h"

static TPulseAutoCollector<FPulseTextureCollector> GPulseTextureCollectorRegistrar;

// ---- Schema helpers -----------------------------------------------------------------------------

static FPulseMetricDesc PulseMakeTextureMetric(FName Name, EPulseMetricKind Kind, EPulseMetricUnit Unit, EPulseTier MinTier, const TCHAR* Description)
{
	FPulseMetricDesc Desc;
	Desc.Name = Name;
	Desc.Kind = Kind;
	Desc.Unit = Unit;
	Desc.MinTier = MinTier;
	Desc.Description = Description;
	return Desc;
}

static FPulseRuleDesc PulseMakeTextureRule(FName RuleId, EPulseSeverity DefaultSeverity, EPulseTier MinTier, const TCHAR* Description, const TCHAR* Recommendation)
{
	FPulseRuleDesc Desc;
	Desc.RuleId = RuleId;
	Desc.DefaultSeverity = DefaultSeverity;
	Desc.MinTier = MinTier;
	Desc.Description = Description;
	Desc.Recommendation = Recommendation;
	return Desc;
}

// ---- Identity -----------------------------------------------------------------------------------

FName FPulseTextureCollector::GetCollectorName() const
{
	return FName(TEXT("Pulse.Texture"));
}

FName FPulseTextureCollector::GetCategory() const
{
	return FName(TEXT("Texture"));
}

UClass* FPulseTextureCollector::GetSupportedClass() const
{
	return UTexture2D::StaticClass();
}

bool FPulseTextureCollector::SupportsSubclasses() const
{
	return true;
}

EPulseTier FPulseTextureCollector::GetMaxUsefulTier() const
{
	return EPulseTier::Deep;
}

EPulseLoadMode FPulseTextureCollector::GetDeepLoadMode() const
{
	return EPulseLoadMode::Driver;
}

// ---- Schema -------------------------------------------------------------------------------------

void FPulseTextureCollector::GetMetricSchema(TArray<FPulseMetricDesc>& OutMetrics) const
{
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("SourceWidth"), EPulseMetricKind::Int, EPulseMetricUnit::Pixels, EPulseTier::Fast,
		TEXT("Imported source width. NOT the runtime size: MaxTextureSize and LODBias apply at cook.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("SourceHeight"), EPulseMetricKind::Int, EPulseMetricUnit::Pixels, EPulseTier::Fast,
		TEXT("Imported source height. NOT the runtime size: MaxTextureSize and LODBias apply at cook.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("Format"), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Platform pixel format name at save time, e.g. DXT1, BC7.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("CompressionSettings"), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Texture compression setting, e.g. TC_Default, TC_Normalmap.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("LODGroup"), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Texture group, e.g. TEXTUREGROUP_World. Drives per-group size and streaming policy.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("MipGenSettings"), EPulseMetricKind::Text, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("Mip generation mode, e.g. TMGS_FromTextureGroup, TMGS_NoMipmaps.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("MaxTextureSize"), EPulseMetricKind::Int, EPulseMetricUnit::Pixels, EPulseTier::Fast,
		TEXT("Per-asset cooked size clamp. 0 means unclamped.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("VirtualTextureStreaming"), EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("True when the texture streams as a virtual texture.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("NeverStream"), EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("True when mip streaming is disabled; the full chain stays resident.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("HasAlphaChannel"), EPulseMetricKind::Bool, EPulseMetricUnit::None, EPulseTier::Fast,
		TEXT("True when the platform format carries alpha. Informational; only UTexture2D writes this tag.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("MemoryBytes"), EPulseMetricKind::Int, EPulseMetricUnit::Bytes, EPulseTier::Deep,
		TEXT("Full mip chain memory, CalcTextureMemorySizeEnum(TMC_AllMips). The real cooked cost.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("CookedWidth"), EPulseMetricKind::Int, EPulseMetricUnit::Pixels, EPulseTier::Deep,
		TEXT("Platform-data width after clamps and LOD bias — what actually ships.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("CookedHeight"), EPulseMetricKind::Int, EPulseMetricUnit::Pixels, EPulseTier::Deep,
		TEXT("Platform-data height after clamps and LOD bias — what actually ships.")));
	OutMetrics.Add(PulseMakeTextureMetric(TEXT("NumMips"), EPulseMetricKind::Int, EPulseMetricUnit::Count, EPulseTier::Deep,
		TEXT("Mip levels in the platform data.")));
}

void FPulseTextureCollector::GetRuleSchema(TArray<FPulseRuleDesc>& OutRules) const
{
	OutRules.Add(PulseMakeTextureRule(TEXT("Texture.OversizeSource"), EPulseSeverity::Medium, EPulseTier::Fast,
		TEXT("Source dimension exceeds MaxSourceDimension (High at CriticalSourceDimension). Demoted to Low when MaxTextureSize clamps the cook or the LODGroup is exempt, because the registry cannot see the cooked size."),
		TEXT("Set MaxTextureSize in the Texture Editor to clamp the cooked size, downscale the source, or add the texture's LODGroup to ExemptLODGroups in Pulse settings if the size is intentional.")));
	OutRules.Add(PulseMakeTextureRule(TEXT("Texture.MissingMips"), EPulseSeverity::Medium, EPulseTier::Fast,
		TEXT("MipGenSettings is TMGS_NoMipmaps on a source larger than 256 pixels. Without mips, minified samples thrash the texture cache and shimmer."),
		TEXT("Set MipGenSettings to TMGS_FromTextureGroup in the Texture Editor. Keep TMGS_NoMipmaps only for UI or pixel-exact lookup textures.")));
	OutRules.Add(PulseMakeTextureRule(TEXT("Texture.NonPowerOfTwo"), EPulseSeverity::Low, EPulseTier::Fast,
		TEXT("A source dimension is not a power of two and the texture is not virtual. NPOT sizes waste block-compression padding and can restrict mip/streaming behavior."),
		TEXT("Resize the source to a power of two, set the texture's Power Of Two Mode to pad, or enable VirtualTextureStreaming.")));
	OutRules.Add(PulseMakeTextureRule(TEXT("Texture.NeverStream"), EPulseSeverity::Medium, EPulseTier::Fast,
		TEXT("NeverStream is set on a non-virtual texture: the full mip chain loads with the package and never yields to streaming-pool pressure."),
		TEXT("Clear NeverStream in the Texture Editor unless the texture genuinely must be full-resolution from the first frame (e.g. a startup UI atlas).")));
	OutRules.Add(PulseMakeTextureRule(TEXT("Texture.VirtualTextureExpected"), EPulseSeverity::Low, EPulseTier::Fast,
		TEXT("Source dimension is at or above VirtualTextureExpectedAtDim but VirtualTextureStreaming is off."),
		TEXT("Enable VirtualTextureStreaming on the texture (requires 'Enable virtual texture support' in Project Settings > Rendering), or clamp with MaxTextureSize if full resolution is not needed.")));
	OutRules.Add(PulseMakeTextureRule(TEXT("Texture.ExcessiveMemory"), EPulseSeverity::Medium, EPulseTier::Deep,
		TEXT("Full-mip-chain memory exceeds MaxMemoryBytes (High at CriticalMemoryBytes). Measured from the loaded texture, so clamps and compression are already accounted for."),
		TEXT("Reduce dimensions or MaxTextureSize, choose a tighter CompressionSettings (e.g. TC_Default over TC_HDR, BC7 only where needed), or assign a more aggressive LODGroup.")));
}

// ---- Collect ------------------------------------------------------------------------------------

void FPulseTextureCollector::CollectFast(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	// Dimensions arrives as "WxH" from UTexture2D — the value comes from GetImportedSize(), i.e.
	// the SOURCE size, which is why these metrics are named SourceWidth/SourceHeight and never
	// Width/Height: an 8K source clamped by MaxTextureSize is fine at runtime, and a metric name
	// implying runtime cost would make the report lie. Some texture subclasses write "WxHxD";
	// the first two axes are the ones the size rules consume.
	FString DimensionsText;
	bool bParsedDimensions = false;
	int64 SourceWidth = 0;
	int64 SourceHeight = 0;
	if (PulseGetTagString(Context.AssetData, TEXT("Dimensions"), DimensionsText))
	{
		TArray<FString> Parts;
		DimensionsText.ParseIntoArray(Parts, TEXT("x"), /*InCullEmpty*/ true);
		if (Parts.Num() >= 2 && LexTryParseString(SourceWidth, *Parts[0]) && LexTryParseString(SourceHeight, *Parts[1]))
		{
			bParsedDimensions = true;
		}
	}
	if (bParsedDimensions)
	{
		InOutResult.Metrics.Add(FPulseMetric::MakeInt(TEXT("SourceWidth"), SourceWidth, EPulseMetricUnit::Pixels, EPulseTier::Fast));
		InOutResult.Metrics.Add(FPulseMetric::MakeInt(TEXT("SourceHeight"), SourceHeight, EPulseMetricUnit::Pixels, EPulseTier::Fast));
	}
	else
	{
		// Absent OR malformed both mean the registry cannot be trusted for this asset's size.
		// Counted once — it is one tag — and no metric is emitted, so the size rules stay quiet
		// instead of judging a zero.
		InOutResult.NumMissingExpectedTags++;
	}

	PulseAddTextTagMetric(InOutResult, Context.AssetData, TEXT("Format"), TEXT("Format"));
	PulseAddTextTagMetric(InOutResult, Context.AssetData, TEXT("CompressionSettings"), TEXT("CompressionSettings"));
	PulseAddTextTagMetric(InOutResult, Context.AssetData, TEXT("LODGroup"), TEXT("LODGroup"));
	PulseAddTextTagMetric(InOutResult, Context.AssetData, TEXT("MipGenSettings"), TEXT("MipGenSettings"));
	PulseAddIntTagMetric(InOutResult, Context.AssetData, TEXT("MaxTextureSize"), TEXT("MaxTextureSize"), EPulseMetricUnit::Pixels);
	PulseAddBoolTagMetric(InOutResult, Context.AssetData, TEXT("VirtualTextureStreaming"), TEXT("VirtualTextureStreaming"));
	PulseAddBoolTagMetric(InOutResult, Context.AssetData, TEXT("NeverStream"), TEXT("NeverStream"));

	// HasAlphaChannel is written by UTexture2D::GetAssetRegistryTags itself, not by the UTexture
	// base, so subclasses with their own tag path may legitimately lack it. Absence is therefore
	// not a trust problem and must not feed the missing-tag counter.
	bool bHasAlpha = false;
	if (PulseGetTagBool(Context.AssetData, TEXT("HasAlphaChannel"), bHasAlpha))
	{
		InOutResult.Metrics.Add(FPulseMetric::MakeBool(TEXT("HasAlphaChannel"), bHasAlpha, EPulseTier::Fast));
	}
}

void FPulseTextureCollector::CollectDeep(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	// The driver's class filter matched, but LoadedObject is whatever actually loaded — a failed
	// load or an unexpected subclass resolution must degrade to Fast-only, not crash.
	const UTexture2D* Texture = Cast<UTexture2D>(Context.LoadedObject);
	if (Texture == nullptr)
	{
		return;
	}

	// TMC_AllMips is the honest cooked cost: resident-mips would depend on the streaming state of
	// the editor process running the audit, which is noise.
	const int64 MemoryBytes = static_cast<int64>(Texture->CalcTextureMemorySizeEnum(TMC_AllMips));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(TEXT("MemoryBytes"), MemoryBytes, EPulseMetricUnit::Bytes, EPulseTier::Deep));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(TEXT("CookedWidth"), Texture->GetSizeX(), EPulseMetricUnit::Pixels, EPulseTier::Deep));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(TEXT("CookedHeight"), Texture->GetSizeY(), EPulseMetricUnit::Pixels, EPulseTier::Deep));
	InOutResult.Metrics.Add(FPulseMetric::MakeInt(TEXT("NumMips"), Texture->GetNumMips(), EPulseMetricUnit::Count, EPulseTier::Deep));
}

// ---- Evaluate -----------------------------------------------------------------------------------

void FPulseTextureCollector::Evaluate(const FPulseCollectContext& Context, FPulseAssetResult& InOutResult) const
{
	const FPulseTextureThresholds& Thresholds = Context.Settings.Texture;

	int64 SourceWidth = 0;
	int64 SourceHeight = 0;
	const bool bHasDimensions = InOutResult.GetIntMetric(TEXT("SourceWidth"), SourceWidth)
		&& InOutResult.GetIntMetric(TEXT("SourceHeight"), SourceHeight);
	const int64 MaxDimension = FMath::Max(SourceWidth, SourceHeight);

	// Absent is not false: a rule conditioned on !VirtualTextureStreaming must not fire when the
	// tag never arrived, or a stale registry turns into a page of streaming complaints.
	bool bVirtualTexture = false;
	const bool bKnowVirtualTexture = InOutResult.GetBoolMetric(TEXT("VirtualTextureStreaming"), bVirtualTexture);

	FString LODGroupText;
	if (const FPulseMetric* LODGroupMetric = InOutResult.FindMetric(TEXT("LODGroup")))
	{
		if (const FString* Text = LODGroupMetric->Value.TryGet<FString>())
		{
			LODGroupText = *Text;
		}
	}

	// -- Texture.OversizeSource -------------------------------------------------------------------
	if (bHasDimensions && MaxDimension > Thresholds.MaxSourceDimension)
	{
		EPulseSeverity Severity = (MaxDimension >= Thresholds.CriticalSourceDimension)
			? EPulseSeverity::High
			: EPulseSeverity::Medium;

		FString Message = FString::Printf(TEXT("%lldx%lld source exceeds MaxSourceDimension %d"),
			SourceWidth, SourceHeight, Thresholds.MaxSourceDimension);
		if (Severity == EPulseSeverity::High)
		{
			Message += FString::Printf(TEXT(" and reaches CriticalSourceDimension %d"), Thresholds.CriticalSourceDimension);
		}

		// Demotion, not suppression: the registry only sees the SOURCE size, and flagging a
		// legitimately clamped 8K source at High is exactly how an audit tool gets ignored. The
		// asset still appears, at Low, so a wrong clamp remains discoverable.
		int64 MaxTextureSize = 0;
		const bool bClampedByMaxTextureSize = Thresholds.bRespectMaxTextureSize
			&& InOutResult.GetIntMetric(TEXT("MaxTextureSize"), MaxTextureSize)
			&& MaxTextureSize > 0
			&& MaxTextureSize <= Thresholds.MaxSourceDimension;
		const bool bExemptLODGroup = !LODGroupText.IsEmpty()
			&& Thresholds.ExemptLODGroups.Contains(FName(*LODGroupText));

		if (bClampedByMaxTextureSize)
		{
			Severity = EPulseSeverity::Low;
			Message += FString::Printf(TEXT("; demoted to Low because MaxTextureSize %lld clamps the cooked size — the registry sees only the source, not what ships"), MaxTextureSize);
		}
		else if (bExemptLODGroup)
		{
			Severity = EPulseSeverity::Low;
			Message += FString::Printf(TEXT("; demoted to Low because LODGroup %s is exempt in Pulse settings"), *LODGroupText);
		}
		Message += TEXT(".");

		FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
		Issue.RuleId = FName(TEXT("Texture.OversizeSource"));
		Issue.Severity = Severity;
		Issue.Message = MoveTemp(Message);
		Issue.DetectedAtTier = EPulseTier::Fast;
	}

	// -- Texture.MissingMips ----------------------------------------------------------------------
	// The 256 floor is deliberate and unconfigurable: below it, a full mip chain saves almost
	// nothing and small UI/lookup textures without mips are routine, not a finding.
	if (Thresholds.bFlagMissingMips && bHasDimensions && MaxDimension > 256)
	{
		if (const FPulseMetric* MipGenMetric = InOutResult.FindMetric(TEXT("MipGenSettings")))
		{
			const FString* MipGenText = MipGenMetric->Value.TryGet<FString>();
			// Exact enum name string as written by UTexture::GetAssetRegistryTags via
			// StaticEnum<TextureMipGenSettings>()->GetNameStringByValue.
			if (MipGenText != nullptr && MipGenText->Equals(TEXT("TMGS_NoMipmaps"), ESearchCase::CaseSensitive))
			{
				FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
				Issue.RuleId = FName(TEXT("Texture.MissingMips"));
				Issue.Severity = EPulseSeverity::Medium;
				Issue.Message = FString::Printf(TEXT("MipGenSettings is TMGS_NoMipmaps on a %lldx%lld source; mips are expected above 256."),
					SourceWidth, SourceHeight);
				Issue.DetectedAtTier = EPulseTier::Fast;
			}
		}
	}

	// -- Texture.NonPowerOfTwo --------------------------------------------------------------------
	if (Thresholds.bFlagNonPowerOfTwo && bHasDimensions && bKnowVirtualTexture && !bVirtualTexture)
	{
		const bool bWidthPow2 = FMath::IsPowerOfTwo(SourceWidth);
		const bool bHeightPow2 = FMath::IsPowerOfTwo(SourceHeight);
		if (!bWidthPow2 || !bHeightPow2)
		{
			FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
			Issue.RuleId = FName(TEXT("Texture.NonPowerOfTwo"));
			Issue.Severity = EPulseSeverity::Low;
			Issue.Message = FString::Printf(TEXT("%lldx%lld source is not power-of-two (%s) and the texture is not virtual."),
				SourceWidth, SourceHeight,
				(!bWidthPow2 && !bHeightPow2) ? TEXT("both dimensions") : (!bWidthPow2 ? TEXT("width") : TEXT("height")));
			Issue.DetectedAtTier = EPulseTier::Fast;
		}
	}

	// -- Texture.NeverStream ----------------------------------------------------------------------
	bool bNeverStream = false;
	if (Thresholds.bFlagNeverStream
		&& InOutResult.GetBoolMetric(TEXT("NeverStream"), bNeverStream) && bNeverStream
		&& bKnowVirtualTexture && !bVirtualTexture)
	{
		FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
		Issue.RuleId = FName(TEXT("Texture.NeverStream"));
		Issue.Severity = EPulseSeverity::Medium;
		Issue.Message = TEXT("NeverStream is set on a non-virtual texture: the full mip chain loads with the package and never yields to streaming-pool pressure.");
		Issue.DetectedAtTier = EPulseTier::Fast;
	}

	// -- Texture.VirtualTextureExpected -----------------------------------------------------------
	if (Thresholds.VirtualTextureExpectedAtDim > 0
		&& bHasDimensions && MaxDimension >= Thresholds.VirtualTextureExpectedAtDim
		&& bKnowVirtualTexture && !bVirtualTexture)
	{
		FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
		Issue.RuleId = FName(TEXT("Texture.VirtualTextureExpected"));
		Issue.Severity = EPulseSeverity::Low;
		Issue.Message = FString::Printf(TEXT("%lldx%lld source is at or above VirtualTextureExpectedAtDim %d but VirtualTextureStreaming is off."),
			SourceWidth, SourceHeight, Thresholds.VirtualTextureExpectedAtDim);
		Issue.DetectedAtTier = EPulseTier::Fast;
	}

	// -- Texture.ExcessiveMemory (Deep) -----------------------------------------------------------
	// If the run never reached Deep the metric is absent and the driver reports the rule as
	// unevaluated — not as passed.
	int64 MemoryBytes = 0;
	if (InOutResult.GetIntMetric(TEXT("MemoryBytes"), MemoryBytes) && MemoryBytes > Thresholds.MaxMemoryBytes)
	{
		const bool bCritical = MemoryBytes >= Thresholds.CriticalMemoryBytes;

		FString Message = FString::Printf(TEXT("%lld bytes across all mips exceeds MaxMemoryBytes %lld"),
			MemoryBytes, Thresholds.MaxMemoryBytes);
		if (bCritical)
		{
			Message += FString::Printf(TEXT(" and reaches CriticalMemoryBytes %lld"), Thresholds.CriticalMemoryBytes);
		}
		Message += TEXT(".");

		FPulseIssue& Issue = InOutResult.Issues.AddDefaulted_GetRef();
		Issue.RuleId = FName(TEXT("Texture.ExcessiveMemory"));
		Issue.Severity = bCritical ? EPulseSeverity::High : EPulseSeverity::Medium;
		Issue.Message = MoveTemp(Message);
		Issue.DetectedAtTier = EPulseTier::Deep;
	}
}
