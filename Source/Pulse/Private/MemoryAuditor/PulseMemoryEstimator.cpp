// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseMemoryEstimator.h"

#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "PixelFormat.h"
#include "PulseTagUtils.h"
#include "Sound/SoundWave.h"

/** The engine writes this tag name verbatim, units and all (see AnimSequence.cpp). */
static const FName GPulseAnimCompressedSizeTag(TEXT("Compressed Size (KB)"));

/** Finds a pixel format by the name the Format tag carries, which is GPixelFormats[].Name. */
static const FPixelFormatInfo* PulseFindPixelFormat(const FString& Name)
{
	if (Name.IsEmpty())
	{
		return nullptr;
	}
	for (int32 Index = 0; Index < PF_MAX; ++Index)
	{
		const FPixelFormatInfo& Info = GPixelFormats[Index];
		// BlockBytes guards PF_Unknown and formats this RHI build never filled in.
		if (Info.Name != nullptr && Info.BlockBytes > 0 && Name.Equals(Info.Name, ESearchCase::IgnoreCase))
		{
			return &Info;
		}
	}
	return nullptr;
}

/**
 * The format a texture would most likely build to on desktop, from its compression settings. Used
 * only when the Format tag is missing or names no real format — a texture saved before its platform
 * data was ever built writes "Unknown".
 */
static EPixelFormat PulseGuessPixelFormat(const FString& CompressionSettings, bool bHasAlpha)
{
	if (CompressionSettings == TEXT("TC_Normalmap"))                 { return PF_BC5; }
	if (CompressionSettings == TEXT("TC_Grayscale")
		|| CompressionSettings == TEXT("TC_Displacementmap")
		|| CompressionSettings == TEXT("TC_DistanceFieldFont"))       { return PF_G8; }
	if (CompressionSettings == TEXT("TC_VectorDisplacementmap")
		|| CompressionSettings == TEXT("TC_EditorIcon"))              { return PF_B8G8R8A8; }
	if (CompressionSettings == TEXT("TC_HDR"))                       { return PF_FloatRGBA; }
	if (CompressionSettings == TEXT("TC_Alpha"))                     { return PF_BC4; }
	if (CompressionSettings == TEXT("TC_HDR_Compressed"))            { return PF_BC6H; }
	if (CompressionSettings == TEXT("TC_BC7"))                       { return PF_BC7; }
	if (CompressionSettings == TEXT("TC_HalfFloat"))                 { return PF_R16F; }
	if (CompressionSettings == TEXT("TC_LQ"))                        { return PF_R5G6B5_UNORM; }
	if (CompressionSettings == TEXT("TC_SingleFloat"))               { return PF_R32_FLOAT; }
	if (CompressionSettings == TEXT("TC_HDR_F32"))                   { return PF_A32B32G32R32F; }

	// TC_Default, TC_Masks and anything unrecognised: the DXT pair, chosen by alpha.
	return bHasAlpha ? PF_DXT5 : PF_DXT1;
}

/** Bytes for mips [FirstMip, NumMips) of a 2D texture in the given format. */
static int64 PulseMipRangeBytes(int32 Width, int32 Height, int32 FirstMip, int32 NumMips, const FPixelFormatInfo& Format)
{
	int64 Bytes = 0;
	for (int32 Mip = FirstMip; Mip < NumMips; ++Mip)
	{
		const int64 MipWidth = FMath::Max(Width >> Mip, 1);
		const int64 MipHeight = FMath::Max(Height >> Mip, 1);
		const int64 BlocksX = FMath::DivideAndRoundUp<int64>(MipWidth, FMath::Max(Format.BlockSizeX, 1));
		const int64 BlocksY = FMath::DivideAndRoundUp<int64>(MipHeight, FMath::Max(Format.BlockSizeY, 1));
		Bytes += BlocksX * BlocksY * Format.BlockBytes;
	}
	return Bytes;
}

/** 1 + 1/2 + 1/4 ...: each mesh LOD assumed to carry half the geometry of the one before. */
static double PulseLodChainFactor(int64 NumLODs)
{
	double Factor = 0.0;
	double Scale = 1.0;
	for (int64 Lod = 0; Lod < FMath::Max<int64>(NumLODs, 1); ++Lod)
	{
		Factor += Scale;
		Scale *= 0.5;
	}
	return Factor;
}

/** Vertex plus index buffer bytes for one mesh's LOD chain. */
static int64 PulseMeshBufferBytes(int64 Vertices, int64 Triangles, int64 NumLODs, int64 BytesPerVertex)
{
	// 16-bit indices until a LOD has more vertices than they can address.
	const int64 BytesPerIndex = Vertices > 65535 ? 4 : 2;
	const int64 Lod0Bytes = Vertices * BytesPerVertex + Triangles * 3 * BytesPerIndex;
	return static_cast<int64>(static_cast<double>(Lod0Bytes) * PulseLodChainFactor(NumLODs));
}

FString PulseFormatBytes(int64 Bytes)
{
	const double Value = static_cast<double>(Bytes);
	if (Bytes >= 1024LL * 1024 * 1024)
	{
		return FString::Printf(TEXT("%.2f GB"), Value / (1024.0 * 1024.0 * 1024.0));
	}
	if (Bytes >= 1024LL * 1024)
	{
		return FString::Printf(TEXT("%.1f MB"), Value / (1024.0 * 1024.0));
	}
	if (Bytes >= 1024)
	{
		return FString::Printf(TEXT("%.0f KB"), Value / 1024.0);
	}
	return FString::Printf(TEXT("%lld B"), Bytes);
}

FPulseMemoryEstimator::FPulseMemoryEstimator(const FPulseMemoryEstimationSettings& InSettings)
	: Settings(InSettings)
{
}

const FAssetData* FPulseMemoryEstimator::PickMainAsset(const TArray<FAssetData>& Assets)
{
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.IsUAsset())
		{
			return &Asset;
		}
	}
	return Assets.Num() > 0 ? &Assets[0] : nullptr;
}

FPulseMemoryEstimate FPulseMemoryEstimator::EstimatePackage(const TArray<FAssetData>& Assets, const FAssetPackageData* PackageData) const
{
	FPulseMemoryEstimate Estimate;
	const FAssetData* MainAsset = PickMainAsset(Assets);

	// GetClass resolves only classes already in memory. Every class with an estimator is native, and
	// native classes always are; a Blueprint-defined asset class resolves to null and falls through
	// to disk size, which is the right answer for it anyway.
	const UClass* Class = MainAsset != nullptr ? MainAsset->GetClass() : nullptr;
	if (Class != nullptr)
	{
		bool bEstimated = false;
		if (Class->IsChildOf(UTexture2D::StaticClass()))
		{
			bEstimated = EstimateTexture2D(*MainAsset, Estimate);
		}
		else if (Class->IsChildOf(UStaticMesh::StaticClass()))
		{
			bEstimated = EstimateStaticMesh(*MainAsset, Estimate);
		}
		else if (Class->IsChildOf(USkeletalMesh::StaticClass()))
		{
			bEstimated = EstimateSkeletalMesh(*MainAsset, Estimate);
		}
		else if (Class->IsChildOf(UAnimSequence::StaticClass()))
		{
			bEstimated = EstimateAnimSequence(*MainAsset, Estimate);
		}
		else if (Class->IsChildOf(USoundWave::StaticClass()))
		{
			bEstimated = EstimateSoundWave(*MainAsset, Estimate);
		}

		if (bEstimated)
		{
			Estimate.Source = EPulseMemorySizeSource::Tags;
			return Estimate;
		}
	}

	EstimateFromDiskSize(MainAsset, PackageData, Estimate);
	return Estimate;
}

void FPulseMemoryEstimator::EstimateFromDiskSize(const FAssetData* MainAsset, const FAssetPackageData* PackageData, FPulseMemoryEstimate& OutEstimate) const
{
	if (PackageData == nullptr || PackageData->DiskSize <= 0)
	{
		// Unknown, not zero-cost: the caller counts these separately so a gap in the data shows up
		// as a gap rather than as a cheap asset.
		OutEstimate.Source = EPulseMemorySizeSource::Unknown;
		OutEstimate.Note = TEXT("No size information in the registry");
		return;
	}

	OutEstimate.Source = EPulseMemorySizeSource::DiskSize;

	// Bucket by what the class becomes at runtime; everything without an estimator is CPU-side
	// unless it is plainly a GPU resource.
	const UClass* Class = MainAsset != nullptr ? MainAsset->GetClass() : nullptr;
	const FString ClassName = MainAsset != nullptr ? MainAsset->AssetClassPath.GetAssetName().ToString() : FString();

	if (ClassName == TEXT("MapBuildDataRegistry"))
	{
		// A map's _BuiltData is mostly lightmap and shadowmap textures. In the editor those carry
		// their uncompressed source, so the package can be several times its cooked size, and cooked
		// lightmaps stream like any other mipped texture. Counting the editor size as resident VRAM
		// put a single lighting build at the top of every ranking it appeared in.
		OutEstimate.Cost.StreamingBytes = PackageData->DiskSize;
		OutEstimate.Note = TEXT("Built lighting; editor size on disk, which includes uncompressed lightmap source");
		return;
	}

	if (Class != nullptr && Class->IsChildOf(UTexture::StaticClass()))
	{
		OutEstimate.Cost.VRAMBytes = PackageData->DiskSize;
	}
	else
	{
		OutEstimate.Cost.RAMBytes = PackageData->DiskSize;
	}
	OutEstimate.Note = TEXT("Editor package size on disk (includes editor-only data)");
}

bool FPulseMemoryEstimator::EstimateTexture2D(const FAssetData& Asset, FPulseMemoryEstimate& OutEstimate) const
{
	// Dimensions is the SOURCE size ("WxH"), the same caveat the texture collector documents. The
	// cooked size is derived below from the clamps the registry does carry.
	FString DimensionsText;
	if (!PulseGetTagString(Asset, TEXT("Dimensions"), DimensionsText))
	{
		return false;
	}
	TArray<FString> Parts;
	DimensionsText.ParseIntoArray(Parts, TEXT("x"), /*InCullEmpty*/ true);
	int32 Width = 0;
	int32 Height = 0;
	if (Parts.Num() < 2 || !LexTryParseString(Width, *Parts[0]) || !LexTryParseString(Height, *Parts[1]) || Width <= 0 || Height <= 0)
	{
		return false;
	}

	int64 MaxTextureSize = 0;
	PulseGetTagInt(Asset, TEXT("MaxTextureSize"), MaxTextureSize);
	if (MaxTextureSize > 0)
	{
		while (FMath::Max(Width, Height) > MaxTextureSize && FMath::Max(Width, Height) > 1)
		{
			Width = FMath::Max(Width / 2, 1);
			Height = FMath::Max(Height / 2, 1);
		}
	}

	// Per-texture LOD bias drops top mips from what the game ever loads. The LOD group's own bias and
	// MaxLODSize live in device profiles the registry does not see, so they are not applied.
	int64 LODBias = 0;
	PulseGetTagInt(Asset, TEXT("LODBias"), LODBias);
	for (int64 Step = 0; Step < LODBias && FMath::Max(Width, Height) > 1; ++Step)
	{
		Width = FMath::Max(Width / 2, 1);
		Height = FMath::Max(Height / 2, 1);
	}

	FString FormatName;
	PulseGetTagString(Asset, TEXT("Format"), FormatName);
	const FPixelFormatInfo* Format = PulseFindPixelFormat(FormatName);
	if (Format == nullptr)
	{
		FString CompressionSettings;
		PulseGetTagString(Asset, TEXT("CompressionSettings"), CompressionSettings);
		bool bHasAlpha = false;
		PulseGetTagBool(Asset, TEXT("HasAlphaChannel"), bHasAlpha);
		Format = &GPixelFormats[PulseGuessPixelFormat(CompressionSettings, bHasAlpha)];
		FormatName = FString(Format->Name) + TEXT("?");
	}

	// Mips: none when asked for none, and none for a non-power-of-two texture the engine will not
	// pad, which is also exactly the case IsPossibleToStream rejects.
	FString MipGenSettings;
	PulseGetTagString(Asset, TEXT("MipGenSettings"), MipGenSettings);
	FString PowerOfTwoMode;
	PulseGetTagString(Asset, TEXT("PowerOfTwoMode"), PowerOfTwoMode);
	const bool bIsPowerOfTwo = FMath::IsPowerOfTwo(Width) && FMath::IsPowerOfTwo(Height);
	const bool bPadsToPowerOfTwo = !PowerOfTwoMode.IsEmpty() && PowerOfTwoMode != TEXT("None");
	const bool bHasMips = MipGenSettings != TEXT("TMGS_NoMipmaps") && (bIsPowerOfTwo || bPadsToPowerOfTwo);
	const int32 NumMips = bHasMips ? FMath::FloorLog2(static_cast<uint32>(FMath::Max(Width, Height))) + 1 : 1;

	const int64 FullBytes = PulseMipRangeBytes(Width, Height, 0, NumMips, *Format);

	bool bVirtualTexture = false;
	PulseGetTagBool(Asset, TEXT("VirtualTextureStreaming"), bVirtualTexture);
	if (bVirtualTexture)
	{
		// Virtual textures page through a fixed pool sized by project settings, so no single texture
		// holds anything resident on its own account.
		OutEstimate.Cost.StreamingBytes = FullBytes;
		OutEstimate.Note = FString::Printf(TEXT("%s %dx%d, virtual texture"), *FormatName, Width, Height);
		return true;
	}

	// Mirrors UTexture::IsPossibleToStream: NeverStream, the UI group, a single mip, or a
	// non-power-of-two size all keep the whole texture resident.
	bool bNeverStream = false;
	PulseGetTagBool(Asset, TEXT("NeverStream"), bNeverStream);
	FString LODGroup;
	PulseGetTagString(Asset, TEXT("LODGroup"), LODGroup);
	const int32 MinResidentMips = UTexture::GetStaticMinTextureResidentMipCount();
	const bool bStreams = !bNeverStream && LODGroup != TEXT("TEXTUREGROUP_UI") && bIsPowerOfTwo && NumMips > MinResidentMips;

	if (bStreams)
	{
		// The smallest MinResidentMips stay resident for as long as the texture is loaded; the rest
		// stream in only when something on screen asks for them.
		const int64 ResidentBytes = PulseMipRangeBytes(Width, Height, NumMips - MinResidentMips, NumMips, *Format);
		OutEstimate.Cost.VRAMBytes = ResidentBytes;
		OutEstimate.Cost.StreamingBytes = FullBytes - ResidentBytes;
	}
	else
	{
		OutEstimate.Cost.VRAMBytes = FullBytes;
	}

	OutEstimate.Note = FString::Printf(TEXT("%s %dx%d, %d mip%s, %s"),
		*FormatName, Width, Height, NumMips, NumMips == 1 ? TEXT("") : TEXT("s"),
		bStreams ? TEXT("streams") : TEXT("fully resident"));
	return true;
}

bool FPulseMemoryEstimator::EstimateStaticMesh(const FAssetData& Asset, FPulseMemoryEstimate& OutEstimate) const
{
	// Vertices and Triangles describe LOD0 of the render data — for a Nanite mesh, the fallback mesh.
	// UStaticMesh writes no tags while compiling, so absence means a stale registry: fall back.
	int64 Vertices = 0;
	int64 Triangles = 0;
	if (!PulseGetTagInt(Asset, TEXT("Vertices"), Vertices) || !PulseGetTagInt(Asset, TEXT("Triangles"), Triangles))
	{
		return false;
	}

	int64 NumLODs = 1;
	PulseGetTagInt(Asset, TEXT("LODs"), NumLODs);
	int64 UVChannels = 1;
	PulseGetTagInt(Asset, TEXT("UVChannels"), UVChannels);

	// Position (12) + packed tangent basis (8) + a half-precision UV per channel (4 each).
	const int64 BytesPerVertex = 12 + 8 + 4 * FMath::Max<int64>(UVChannels, 1);
	OutEstimate.Cost.VRAMBytes = PulseMeshBufferBytes(Vertices, Triangles, NumLODs, BytesPerVertex);

	bool bNanite = false;
	PulseGetTagBool(Asset, TEXT("NaniteEnabled"), bNanite);
	int64 NaniteBytes = 0;
	if (bNanite && PulseGetTagInt(Asset, TEXT("EstNaniteCompressedSize"), NaniteBytes) && NaniteBytes > 0)
	{
		// Nanite pages stream through a fixed pool; only root pages stay resident, and those are small.
		OutEstimate.Cost.StreamingBytes += NaniteBytes;
	}

	// Mesh distance fields stream into a global atlas, and only when the project uses them.
	int64 DistanceFieldBytes = 0;
	if (PulseGetTagInt(Asset, TEXT("DistanceFieldSize"), DistanceFieldBytes) && DistanceFieldBytes > 0)
	{
		OutEstimate.Cost.StreamingBytes += DistanceFieldBytes;
	}

	OutEstimate.Note = FString::Printf(TEXT("%lld verts, %lld tris, %lld LOD%s%s"),
		Vertices, Triangles, NumLODs, NumLODs == 1 ? TEXT("") : TEXT("s"), bNanite ? TEXT(", Nanite") : TEXT(""));
	return true;
}

bool FPulseMemoryEstimator::EstimateSkeletalMesh(const FAssetData& Asset, FPulseMemoryEstimate& OutEstimate) const
{
	int64 Vertices = 0;
	int64 Triangles = 0;
	if (!PulseGetTagInt(Asset, TEXT("Vertices"), Vertices) || !PulseGetTagInt(Asset, TEXT("Triangles"), Triangles))
	{
		return false;
	}

	int64 NumLODs = 1;
	PulseGetTagInt(Asset, TEXT("LODs"), NumLODs);
	int64 Bones = 0;
	PulseGetTagInt(Asset, TEXT("Bones"), Bones);

	// Static-mesh layout with one UV, plus four skin influences: 8 bytes with 8-bit bone indices,
	// 16 once a skeleton outgrows them.
	const int64 SkinWeightBytes = Bones > 256 ? 16 : 8;
	const int64 BytesPerVertex = 12 + 8 + 4 + SkinWeightBytes;
	OutEstimate.Cost.VRAMBytes = PulseMeshBufferBytes(Vertices, Triangles, NumLODs, BytesPerVertex);

	bool bNanite = false;
	PulseGetTagBool(Asset, TEXT("NaniteEnabled"), bNanite);
	int64 NaniteBytes = 0;
	if (bNanite && PulseGetTagInt(Asset, TEXT("EstNaniteCompressedSize"), NaniteBytes) && NaniteBytes > 0)
	{
		OutEstimate.Cost.StreamingBytes += NaniteBytes;
	}

	OutEstimate.Note = FString::Printf(TEXT("%lld verts, %lld tris, %lld LOD%s, %lld bones%s"),
		Vertices, Triangles, NumLODs, NumLODs == 1 ? TEXT("") : TEXT("s"), Bones, bNanite ? TEXT(", Nanite") : TEXT(""));
	return true;
}

bool FPulseMemoryEstimator::EstimateAnimSequence(const FAssetData& Asset, FPulseMemoryEstimate& OutEstimate) const
{
	// The engine's own figure for the compressed track data, in kilobytes.
	double CompressedKB = 0.0;
	if (!PulseGetTagReal(Asset, GPulseAnimCompressedSizeTag, CompressedKB) || CompressedKB < 0.0)
	{
		return false;
	}

	OutEstimate.Cost.RAMBytes = static_cast<int64>(CompressedKB * 1024.0);
	OutEstimate.Note = TEXT("Compressed animation data");
	return true;
}

bool FPulseMemoryEstimator::EstimateSoundWave(const FAssetData& Asset, FPulseMemoryEstimate& OutEstimate) const
{
	double Duration = 0.0;
	int64 SampleRate = 0;
	int64 NumChannels = 0;
	if (!PulseGetTagReal(Asset, TEXT("Duration"), Duration) || !PulseGetTagInt(Asset, TEXT("SampleRate"), SampleRate)
		|| !PulseGetTagInt(Asset, TEXT("NumChannels"), NumChannels) || Duration <= 0.0 || SampleRate <= 0 || NumChannels <= 0)
	{
		return false;
	}

	// Streaming, not resident: with stream caching (the UE5 default) compressed audio is paged in when
	// the sound plays. A wave set to Retain or Force Inline is resident instead; that setting is not a
	// registry tag, so it cannot be told apart here.
	const double PcmBytes = Duration * static_cast<double>(SampleRate) * static_cast<double>(NumChannels) * 2.0;
	OutEstimate.Cost.StreamingBytes = static_cast<int64>(PcmBytes / FMath::Max(Settings.AudioCompressionRatio, 1.0f));
	OutEstimate.Note = FString::Printf(TEXT("%.1fs, %lld Hz, %lld ch, PCM / %.0f"),
		Duration, SampleRate, NumChannels, Settings.AudioCompressionRatio);
	return true;
}
