// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseMemoryAuditorSettings.h"

/**
 * Bytes split by where they live once loaded.
 *
 * StreamingBytes is kept apart from both resident buckets on purpose. A hard reference to a
 * streaming texture keeps only its low mips resident; the rest arrive when the camera asks for them
 * and leave when it stops. Counting those bytes as resident would make every textured asset look
 * several times worse than it is, and a tool that cries wolf on every row stops being read.
 */
struct FPulseMemoryCost
{
	/** System memory held for as long as the package is loaded. */
	int64 RAMBytes = 0;

	/** Video memory held for as long as the package is loaded. */
	int64 VRAMBytes = 0;

	/** Paged in on demand: texture mips above the resident floor, Nanite pages, streamed audio. */
	int64 StreamingBytes = 0;

	int64 GetResidentBytes() const { return RAMBytes + VRAMBytes; }

	bool IsZero() const { return RAMBytes == 0 && VRAMBytes == 0 && StreamingBytes == 0; }

	FPulseMemoryCost& operator+=(const FPulseMemoryCost& Other)
	{
		RAMBytes += Other.RAMBytes;
		VRAMBytes += Other.VRAMBytes;
		StreamingBytes += Other.StreamingBytes;
		return *this;
	}
};

/** Where a package's own size came from. Reaches every report so an estimate is never mistaken for a measurement. */
enum class EPulseMemorySizeSource : uint8
{
	/** Nothing to go on: no registry entry, or no tags and no disk size. Counted as zero and flagged. */
	Unknown,

	/** Computed from Asset Registry tags: texture format and dimensions, mesh vertex counts, and so on. */
	Tags,

	/**
	 * The package's size on disk in the editor, used as a stand-in. Editor packages carry editor-only
	 * data (Blueprint graphs, material node graphs), so this usually overstates what a cooked build loads.
	 */
	DiskSize
};

/** Why an asset is top-level. Ordered by precedence: an asset that qualifies twice keeps the lowest. */
enum class EPulseMemoryRootKind : uint8
{
	/** Resident in every map: Maps & Modes classes and AlwaysLoadedAssets. */
	AlwaysLoaded,

	Map,

	/** Matched AdditionalRootClasses. */
	Configured,

	PrimaryAsset
};

inline const TCHAR* PulseMemoryRootKindToString(EPulseMemoryRootKind Kind)
{
	switch (Kind)
	{
	case EPulseMemoryRootKind::AlwaysLoaded: return TEXT("AlwaysLoaded");
	case EPulseMemoryRootKind::Map:          return TEXT("Map");
	case EPulseMemoryRootKind::Configured:   return TEXT("Configured");
	case EPulseMemoryRootKind::PrimaryAsset: return TEXT("PrimaryAsset");
	}
	return TEXT("Unknown");
}

inline const TCHAR* PulseMemorySizeSourceToString(EPulseMemorySizeSource Source)
{
	switch (Source)
	{
	case EPulseMemorySizeSource::Unknown:  return TEXT("Unknown");
	case EPulseMemorySizeSource::Tags:     return TEXT("Tags");
	case EPulseMemorySizeSource::DiskSize: return TEXT("DiskSize");
	}
	return TEXT("Unknown");
}

/** One package in the dependency graph. The graph is package-level because that is the unit that loads. */
struct FPulseMemoryNode
{
	FName PackageName;

	/** The package's main asset, for display and Content Browser sync. Empty when the registry has none. */
	FString AssetName;
	FString ObjectPath;
	FString ClassName;

	/** What the package itself costs, excluding anything it references. */
	FPulseMemoryCost Self;
	EPulseMemorySizeSource SizeSource = EPulseMemorySizeSource::Unknown;

	/** Short human explanation of the estimate, e.g. "DXT1 2048x2048, 12 mips, streams". */
	FString SizeNote;

	/** Node indices. Game dependencies only — editor-only references never load in a cooked build. */
	TArray<int32> HardDependencies;
	TArray<int32> SoftDependencies;

	// ---- Filled by analysis -----------------------------------------------------------------------

	/**
	 * What would leave memory if this package stopped loading: its own cost plus everything only it
	 * keeps alive. Computed as the package's subtree in the dominator tree of the hard graph rooted at
	 * every top-level asset, the same "retained size" a heap profiler reports.
	 */
	FPulseMemoryCost Retained;

	/** Top-level assets whose hard closure contains this package. 0 means nothing analysed loads it. */
	int32 NumRootsReaching = 0;

	/** True when an always-loaded asset hard-references this package, directly or not. */
	bool bInBaseline = false;
};

/** One top-level asset and what loading it costs. */
struct FPulseMemoryRoot
{
	int32 NodeIndex = INDEX_NONE;
	EPulseMemoryRootKind Kind = EPulseMemoryRootKind::PrimaryAsset;

	/** Where the root came from: a primary asset type, a settings field, a class. */
	FString KindDetail;

	/**
	 * True for World Partition maps. Their actors are separate packages that stream per cell, so they
	 * are counted under MayLoad rather than Total — Total is the persistent level only.
	 */
	bool bWorldPartition = false;

	/** The root plus everything it hard-references, transitively. Resident while the root is. */
	FPulseMemoryCost Total;

	/** The root's retained cost: what leaves memory if nothing loads it any more. */
	FPulseMemoryCost Unique;

	/** Reachable only through at least one soft reference (or a streamed World Partition actor). */
	FPulseMemoryCost MayLoad;

	/** Packages in the hard closure, the root included. */
	int32 NumHardPackages = 0;

	/** Packages reachable only softly. */
	int32 NumSoftOnlyPackages = 0;

	/** Resident bytes in Total whose size is a disk-size stand-in rather than computed from tags. */
	int64 ResidentBytesFromDiskSize = 0;

	/** Packages in the hard closure the auditor could not size at all. */
	int32 NumUnknownSize = 0;
};

/**
 * What one root reaches, in breadth-first order, so a shortest reference chain can be read back for
 * any entry. Computed on demand rather than stored per root: a map can reach most of a project, and
 * keeping that for every root would cost more memory than the report is worth.
 */
struct FPulseMemoryReach
{
	/** Node indices. [0] is the root; [0, NumHard) are hard; the remainder are soft-only. */
	TArray<int32> Nodes;

	/** Parallel to Nodes: the slot of the entry that first referenced this one. INDEX_NONE for the root. */
	TArray<int32> ParentSlots;

	/** Parallel to Nodes: hops from the root. */
	TArray<int32> Depths;

	int32 NumHard = 0;
};

/** Everything one memory audit produced. */
struct FPulseMemoryReport
{
	FString GeneratedAtUtc;
	FString EngineVersion;

	/** Echoed so a report is never separated from the settings that shaped it. */
	FPulseMemoryRootSettings RootSettings;
	FPulseMemoryEstimationSettings Estimation;
	FPulseMemoryBudget Budget;
	TArray<FString> ScanPaths;

	TArray<FPulseMemoryNode> Nodes;
	TArray<FPulseMemoryRoot> Roots;

	/** The union of every always-loaded root's hard closure: paid in every map. */
	FPulseMemoryCost Baseline;
	int32 NumBaselinePackages = 0;

	/** Packages reachable from at least one root through hard references. */
	int32 NumReachablePackages = 0;

	/** Package name to node index. */
	TMap<FName, int32> NodeIndexByPackage;

	/** Breadth-first walk from one node. bIncludeSoft appends soft-only entries after the hard ones. */
	void ComputeReach(int32 RootNodeIndex, bool bIncludeSoft, FPulseMemoryReach& OutReach) const;

	/** "A > B > C": the chain from the root to the entry at Slot, excluding the entry itself. */
	FString BuildViaPath(const FPulseMemoryReach& Reach, int32 Slot) const;

	/**
	 * For every entry in Reach, the slots of the entries that reference it directly. The via chain is
	 * one route; this is all of them, which is what breaking a chain actually needs — cutting one
	 * reference does nothing while another still holds the package. A hard entry lists only hard
	 * referencers, since only those keep it resident; a soft-only entry lists both kinds.
	 */
	void CollectReferencers(const FPulseMemoryReach& Reach, TArray<TArray<int32>>& OutReferencerSlots) const;

	/** "A; B; C (+4 more)": the referencers' asset names, capped for display. */
	FString FormatReferencers(const FPulseMemoryReach& Reach, const TArray<int32>& ReferencerSlots, int32 MaxNames) const;

	/** True when the root's totals exceed a configured budget. */
	bool IsRootOverBudget(const FPulseMemoryRoot& Root) const;

	bool IsBaselineOverBudget() const;
};
