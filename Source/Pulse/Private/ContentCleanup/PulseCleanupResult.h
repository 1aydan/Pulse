// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "MemoryAuditor/PulseMemoryResult.h"

/** Why an asset is listed as cleanup material. */
enum class EPulseCleanupStatus : uint8
{
	/** Nothing references it at all, not even editor-only references. */
	Orphan,

	/** Something references it, but nothing that ships: only other unused assets, or editor-only references. */
	Unreachable,

	/** An ObjectRedirector. Fix up the redirectors and delete it, whether or not anything still points at it. */
	Redirector
};

inline const TCHAR* PulseCleanupStatusToString(EPulseCleanupStatus Status)
{
	switch (Status)
	{
	case EPulseCleanupStatus::Orphan:      return TEXT("Orphan");
	case EPulseCleanupStatus::Unreachable: return TEXT("Unreachable");
	case EPulseCleanupStatus::Redirector:  return TEXT("Redirector");
	}
	return TEXT("Unknown");
}

/** One asset nothing that ships can load. */
struct FPulseCleanupAsset
{
	FName PackageName;
	FString AssetName;
	FString ObjectPath;
	FString ClassName;
	EPulseCleanupStatus Status = EPulseCleanupStatus::Orphan;

	/** Editor package size on disk: what deleting it removes from the project. */
	int64 DiskBytes = 0;

	/** What it would cost loaded, from the memory auditor's estimator. */
	FPulseMemoryCost Memory;

	/** Packages referencing it, editor-only references included. Zero for an orphan. */
	int32 NumReferencers = 0;
	FString ReferencedBy;

	/**
	 * Config or source files that name this package. A path in an .ini value or a ConstructorHelpers
	 * call is invisible to the Asset Registry, so a named asset may be live: check before deleting.
	 */
	FString MentionedIn;

	bool IsMentioned() const { return !MentionedIn.IsEmpty(); }
};

/** One copy of a duplicated import. */
struct FPulseDuplicateMember
{
	FName PackageName;
	FString AssetName;
	FString ObjectPath;
	int64 DiskBytes = 0;
	FPulseMemoryCost Memory;

	/** True when something that ships loads this copy. */
	bool bUsed = false;

	int32 NumReferencers = 0;
};

/** Assets of one class imported from the same source file content. */
struct FPulseDuplicateGroup
{
	FString ClassName;

	/** The source file's MD5, from the AssetImportData tag. */
	FString SourceHash;

	/** Source filename as recorded by the first copy's import, for recognising the group. */
	FString SourceFile;

	/** Largest first. */
	TArray<FPulseDuplicateMember> Members;

	/** Disk the extra copies take: everything but the largest. */
	int64 WastedDiskBytes = 0;

	/**
	 * Memory the extra copies cost when more than one ships: every used copy but the largest. Zero
	 * when at most one copy is used, since only one is ever loaded then.
	 */
	FPulseMemoryCost WastedMemory;
};

/** Everything one cleanup run found. */
struct FPulseCleanupReport
{
	FString GeneratedAtUtc;
	FString EngineVersion;
	TArray<FString> ScanPaths;

	/** True when Maps to Cook was set and respected, so only those maps kept content alive. */
	bool bUsedMapsToCook = false;
	int32 NumMapsToCook = 0;
	int32 NumAlwaysCookDirectories = 0;

	/** True when .ini files and C++ source were searched for package paths. */
	bool bSearchedConfigAndSource = false;
	int32 NumFilesSearched = 0;

	/** Packages any root reaches through hard or soft references. */
	int32 NumUsedPackages = 0;
	int32 NumAssetsScanned = 0;

	/** Largest on disk first. */
	TArray<FPulseCleanupAsset> Unused;

	/** Most wasted disk first. */
	TArray<FPulseDuplicateGroup> Duplicates;

	int64 GetUnusedDiskBytes(bool bIncludeMentioned) const
	{
		int64 Total = 0;
		for (const FPulseCleanupAsset& Asset : Unused)
		{
			if (bIncludeMentioned || !Asset.IsMentioned())
			{
				Total += Asset.DiskBytes;
			}
		}
		return Total;
	}

	int64 GetDuplicateWastedDiskBytes() const
	{
		int64 Total = 0;
		for (const FPulseDuplicateGroup& Group : Duplicates)
		{
			Total += Group.WastedDiskBytes;
		}
		return Total;
	}
};
