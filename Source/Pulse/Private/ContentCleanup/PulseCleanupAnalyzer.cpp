// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseCleanupAnalyzer.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorFramework/AssetImportData.h"
#include "HAL/FileManager.h"
#include "MemoryAuditor/PulseMemoryAnalyzer.h"
#include "MemoryAuditor/PulseMemoryEstimator.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Pulse.h"
#include "PulseContentCleanupSettings.h"
#include "PulseMemoryAuditorSettings.h"
#include "Settings/ProjectPackagingSettings.h"

#define LOCTEXT_NAMESPACE "PulseCleanupAnalyzer"

/** Referencer names shown per asset. The count is always exact; the list is for reading. */
static constexpr int32 GPulseCleanupMaxReferencerNames = 10;

/** Config and source files larger than this are skipped: nobody hand-writes an 8 MB .ini. */
static constexpr int64 GPulseCleanupMaxSearchFileBytes = 8 * 1024 * 1024;

// ---- Config -------------------------------------------------------------------------------------

bool FPulseCleanupConfig::Parse(const FString& Params, FPulseCleanupConfig& OutConfig, FString& OutError)
{
	FString RawValue;
	if (FParse::Value(*Params, TEXT("path="), RawValue))
	{
		RawValue.ParseIntoArray(OutConfig.PathFilter, TEXT(","), /*InCullEmpty*/ true);
		for (FString& Path : OutConfig.PathFilter)
		{
			Path.TrimStartAndEndInline();
		}
		OutConfig.PathFilter.RemoveAll([](const FString& Path) { return Path.IsEmpty(); });
		if (OutConfig.PathFilter.Num() == 0)
		{
			OutError = TEXT("-path= was given but listed no paths.");
			return false;
		}
	}
	if (FParse::Value(*Params, TEXT("reportdir="), RawValue))
	{
		OutConfig.ReportDir = RawValue.TrimQuotes();
	}
	OutConfig.bAllMapsFromCommandLine = FParse::Param(*Params, TEXT("allmaps"));
	OutConfig.bNoSearchFromCommandLine = FParse::Param(*Params, TEXT("nosearch"));
	return true;
}

void FPulseCleanupConfig::ApplySettingsDefaults(const UPulseContentCleanupSettings& Settings)
{
	if (PathFilter.Num() == 0)
	{
		for (const FDirectoryPath& Directory : Settings.ScanPaths)
		{
			if (!Directory.Path.IsEmpty())
			{
				PathFilter.Add(Directory.Path);
			}
		}
	}

	ExcludePathPatterns = Settings.ExcludePathPatterns;
	IgnoredClassNames = Settings.IgnoredClassNames;
	AlwaysUsedClasses = Settings.AlwaysUsedClasses;
	bRespectMapsToCook = Settings.bRespectMapsToCook && !bAllMapsFromCommandLine;
	bSearchConfigAndSource = Settings.bSearchConfigAndSource && !bNoSearchFromCommandLine;
	MinDuplicateWasteKB = Settings.MinDuplicateWasteKB;
	MaxHistoricalRuns = Settings.MaxHistoricalRuns;

	if (ReportDir.IsEmpty())
	{
		ReportDir = Settings.ReportDirectory;
	}
	if (FPaths::IsRelative(ReportDir))
	{
		ReportDir = FPaths::ProjectDir() / ReportDir;
	}
	ReportDir = FPaths::ConvertRelativePathToFull(ReportDir);
}

FString FPulseCleanupConfig::ToString() const
{
	return FString::Printf(TEXT("paths=[%s] mapstocook=%s search=%s reportdir=%s"),
		*FString::Join(PathFilter, TEXT(",")),
		bRespectMapsToCook ? TEXT("respected") : TEXT("ignored"),
		bSearchConfigAndSource ? TEXT("true") : TEXT("false"),
		*ReportDir);
}

// ---- Helpers ------------------------------------------------------------------------------------

/** Packaging settings store paths as /Game/X or as X relative to /Game. */
static FString PulseNormalizePackagingPath(const FString& Path)
{
	FString Normalized = Path;
	Normalized.TrimStartAndEndInline();
	if (!Normalized.StartsWith(TEXT("/")))
	{
		Normalized = TEXT("/Game/") + Normalized;
	}
	return Normalized;
}

/** Short names of a package's referencers, sorted for stable output and capped for reading. */
static FString PulseFormatReferencerNames(TArray<FName> Referencers)
{
	TArray<FString> Names;
	for (const FName& Referencer : Referencers)
	{
		Names.Add(FPackageName::GetShortName(Referencer));
	}
	Names.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });

	const int32 NumHidden = Names.Num() - GPulseCleanupMaxReferencerNames;
	if (NumHidden > 0)
	{
		Names.SetNum(GPulseCleanupMaxReferencerNames);
		return FString::Printf(TEXT("%s (+%d more)"), *FString::Join(Names, TEXT("; ")), NumHidden);
	}
	return FString::Join(Names, TEXT("; "));
}

/** True for characters that can end a package path written in text: quotes, brackets, separators. */
static bool PulseIsPathTerminator(TCHAR Char)
{
	return FChar::IsWhitespace(Char) || Char == TEXT('"') || Char == TEXT('\'') || Char == TEXT('(') || Char == TEXT(')')
		|| Char == TEXT(',') || Char == TEXT(';') || Char == TEXT('[') || Char == TEXT(']') || Char == TEXT('<')
		|| Char == TEXT('>') || Char == TEXT('{') || Char == TEXT('}') || Char == TEXT('|') || Char == TEXT('\\')
		|| Char == TEXT('.') || Char == TEXT(':') || Char == TEXT('=');
}

// ---- Analyzer -----------------------------------------------------------------------------------

FPulseCleanupAnalyzer::FPulseCleanupAnalyzer(const FPulseCleanupConfig& InConfig)
	: Config(InConfig)
{
}

bool FPulseCleanupAnalyzer::IsExcluded(FName PackageName) const
{
	const FString Package = PackageName.ToString();
	for (const FString& Pattern : Config.ExcludePathPatterns)
	{
		if (!Pattern.IsEmpty() && Package.Contains(Pattern, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

bool FPulseCleanupAnalyzer::Run(FPulseCleanupReport& OutReport, FString& OutError, const FProgressCallback& Progress)
{
	auto ReportProgress = [&Progress](const FText& Stage, float Fraction)
	{
		return !Progress || Progress(Stage, Fraction);
	};

	OutReport = FPulseCleanupReport();
	OutReport.GeneratedAtUtc = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	OutReport.EngineVersion = FEngineVersion::Current().ToString();
	OutReport.ScanPaths = Config.PathFilter;

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.SearchAllAssets(/*bSynchronousSearch*/ true);

	// ---- What ships: the memory auditor's walk from the cooker's roots -----------------------------
	const UProjectPackagingSettings& Packaging = *GetDefault<UProjectPackagingSettings>();
	const UPulseMemoryAuditorSettings& MemorySettings = *GetDefault<UPulseMemoryAuditorSettings>();

	FPulseMemoryAuditorConfig MemoryConfig;
	// No path filter: content under the scan paths is kept alive by roots anywhere in the project.
	MemoryConfig.ExcludePathPatterns = Config.ExcludePathPatterns;
	MemoryConfig.Roots.bIncludePrimaryAssets = true;
	MemoryConfig.Roots.bIncludeGameMapsSettings = true;
	MemoryConfig.Roots.AlwaysLoadedAssets = MemorySettings.Roots.AlwaysLoadedAssets;
	MemoryConfig.Roots.bIncludeBlueprints = false;
	MemoryConfig.Roots.AdditionalRootClasses.Reset();
	MemoryConfig.Estimation = MemorySettings.Estimation;
	// Soft references count: an asset a soft reference can load is not dead, whatever its memory cost.
	MemoryConfig.Estimation.bIncludeSoftReferences = true;

	OutReport.bUsedMapsToCook = Config.bRespectMapsToCook && Packaging.MapsToCook.Num() > 0;
	MemoryConfig.Roots.bIncludeMaps = !OutReport.bUsedMapsToCook;
	if (OutReport.bUsedMapsToCook)
	{
		for (const FFilePath& Map : Packaging.MapsToCook)
		{
			if (!Map.FilePath.IsEmpty())
			{
				MemoryConfig.InspectAssets.Add(PulseNormalizePackagingPath(Map.FilePath));
				OutReport.NumMapsToCook++;
			}
		}
	}

	for (const FDirectoryPath& Directory : Packaging.DirectoriesToAlwaysCook)
	{
		if (Directory.Path.IsEmpty())
		{
			continue;
		}
		OutReport.NumAlwaysCookDirectories++;
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*PulseNormalizePackagingPath(Directory.Path)), Assets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ true);
		for (const FAssetData& Asset : Assets)
		{
			MemoryConfig.InspectAssets.AddUnique(Asset.PackageName.ToString());
		}
	}

	// Content the game finds by scanning rather than by reference: every asset of these classes is a
	// root, so what it references counts as used too.
	for (const FSoftClassPath& ClassPath : Config.AlwaysUsedClasses)
	{
		if (ClassPath.IsNull())
		{
			continue;
		}
		FARFilter ClassFilter;
		ClassFilter.ClassPaths.Add(ClassPath.GetAssetPath());
		ClassFilter.bRecursiveClasses = true;
		ClassFilter.bIncludeOnlyOnDiskAssets = true;
		TArray<FAssetData> Assets;
		Registry.GetAssets(ClassFilter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			MemoryConfig.InspectAssets.AddUnique(Asset.PackageName.ToString());
		}
	}

	FPulseMemoryReport Graph;
	FPulseMemoryAnalyzer GraphAnalyzer(MemoryConfig);
	const bool bGraphBuilt = GraphAnalyzer.Run(Graph, OutError, [&ReportProgress](const FText& Stage, float Fraction)
	{
		return ReportProgress(Stage, 0.5f * Fraction);
	});
	if (!bGraphBuilt)
	{
		return false;
	}
	// Every node in the walk was reached from a root through some reference, hard or soft.
	OutReport.NumUsedPackages = Graph.Nodes.Num();

	// ---- Every asset under the scan paths ----------------------------------------------------------
	if (!ReportProgress(LOCTEXT("StageScan", "Checking what is not used"), 0.5f))
	{
		OutError = TEXT("Cancelled.");
		return false;
	}

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bIncludeOnlyOnDiskAssets = true;
	for (const FString& Path : Config.PathFilter)
	{
		Filter.PackagePaths.Add(FName(*Path));
	}
	TArray<FAssetData> AllAssets;
	Registry.GetAssets(Filter, AllAssets);

	TSet<FName> PackageSet;
	for (const FAssetData& Asset : AllAssets)
	{
		PackageSet.Add(Asset.PackageName);
	}
	TArray<FName> Packages = PackageSet.Array();
	Packages.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	const FPulseMemoryEstimator Estimator(MemorySettings.Estimation);

	// Duplicate candidates keyed by class and source hash.
	struct FPulseDuplicateCandidate
	{
		FString SourceFile;
		FPulseDuplicateMember Member;
	};
	TMap<FString, TArray<FPulseDuplicateCandidate>> BySource;

	for (int32 PackageIndex = 0; PackageIndex < Packages.Num(); ++PackageIndex)
	{
		const FName PackageName = Packages[PackageIndex];
		if (IsExcluded(PackageName))
		{
			continue;
		}

		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyOnDiskAssets*/ true);
		const FAssetData* MainAsset = FPulseMemoryEstimator::PickMainAsset(Assets);
		if (MainAsset == nullptr)
		{
			continue;
		}

		const FString ClassName = MainAsset->AssetClassPath.GetAssetName().ToString();
		if (Config.IgnoredClassNames.Contains(ClassName))
		{
			continue;
		}
		OutReport.NumAssetsScanned++;

		const bool bUsed = Graph.NodeIndexByPackage.Contains(PackageName);
		const bool bRedirector = ClassName == TEXT("ObjectRedirector");
		const TOptional<FAssetPackageData> PackageData = Registry.GetAssetPackageDataCopy(PackageName);
		const int64 DiskBytes = PackageData.IsSet() ? FMath::Max<int64>(PackageData->DiskSize, 0) : 0;

		// Sized lazily: most packages are used and never need it.
		TOptional<FPulseMemoryEstimate> Estimate;
		auto GetEstimate = [&]() -> const FPulseMemoryEstimate&
		{
			if (!Estimate.IsSet())
			{
				Estimate = Estimator.EstimatePackage(Assets, PackageData.IsSet() ? &PackageData.GetValue() : nullptr);
			}
			return Estimate.GetValue();
		};

		TArray<FName> Referencers;
		auto GetReferencers = [&]() -> const TArray<FName>&
		{
			if (Referencers.Num() == 0)
			{
				// Every category and flag, editor-only included: an asset only a tool references is
				// still not an orphan, and the referencer list is how someone finds that tool.
				Registry.GetReferencers(PackageName, Referencers);
				Referencers.Remove(PackageName);
			}
			return Referencers;
		};

		if (!bUsed || bRedirector)
		{
			FPulseCleanupAsset& Unused = OutReport.Unused.AddDefaulted_GetRef();
			Unused.PackageName = PackageName;
			Unused.AssetName = MainAsset->AssetName.ToString();
			Unused.ObjectPath = MainAsset->GetSoftObjectPath().ToString();
			Unused.ClassName = ClassName;
			Unused.DiskBytes = DiskBytes;
			Unused.Memory = GetEstimate().Cost;
			Unused.NumReferencers = GetReferencers().Num();
			Unused.ReferencedBy = PulseFormatReferencerNames(GetReferencers());
			Unused.Status = bRedirector ? EPulseCleanupStatus::Redirector
				: Unused.NumReferencers == 0 ? EPulseCleanupStatus::Orphan
				: EPulseCleanupStatus::Unreachable;
		}

		// Duplicates: the same source file content imported more than once as the same class.
		FString ImportJson;
		if (!bRedirector && MainAsset->GetTagValue(UObject::SourceFileTagName(), ImportJson))
		{
			const TOptional<FAssetImportInfo> ImportInfo = FAssetImportInfo::FromJson(ImportJson);
			if (ImportInfo.IsSet() && ImportInfo->SourceFiles.Num() > 0 && ImportInfo->SourceFiles[0].FileHash.IsValid())
			{
				const FAssetImportInfo::FSourceFile& Source = ImportInfo->SourceFiles[0];
				FPulseDuplicateCandidate& Candidate = BySource.FindOrAdd(ClassName + TEXT("|") + LexToString(Source.FileHash)).AddDefaulted_GetRef();
				Candidate.SourceFile = FPaths::GetCleanFilename(Source.RelativeFilename);
				Candidate.Member.PackageName = PackageName;
				Candidate.Member.AssetName = MainAsset->AssetName.ToString();
				Candidate.Member.ObjectPath = MainAsset->GetSoftObjectPath().ToString();
				Candidate.Member.DiskBytes = DiskBytes;
				Candidate.Member.Memory = GetEstimate().Cost;
				Candidate.Member.bUsed = bUsed;
				Candidate.Member.NumReferencers = GetReferencers().Num();
			}
		}

		if ((PackageIndex & 511) == 0)
		{
			const FText Stage = FText::Format(LOCTEXT("StageScanCount", "Checking {0} of {1} packages"),
				FText::AsNumber(PackageIndex + 1), FText::AsNumber(Packages.Num()));
			if (!ReportProgress(Stage, 0.5f + 0.35f * static_cast<float>(PackageIndex) / static_cast<float>(FMath::Max(Packages.Num(), 1))))
			{
				OutError = TEXT("Cancelled.");
				return false;
			}
		}
	}

	// ---- Duplicate groups ----------------------------------------------------------------------------
	const int64 MinWasteBytes = static_cast<int64>(Config.MinDuplicateWasteKB) * 1024;
	for (TPair<FString, TArray<FPulseDuplicateCandidate>>& Pair : BySource)
	{
		if (Pair.Value.Num() < 2)
		{
			continue;
		}

		FPulseDuplicateGroup Group;
		Pair.Key.Split(TEXT("|"), &Group.ClassName, &Group.SourceHash);
		for (FPulseDuplicateCandidate& Candidate : Pair.Value)
		{
			Group.Members.Add(MoveTemp(Candidate.Member));
		}
		Group.Members.Sort([](const FPulseDuplicateMember& A, const FPulseDuplicateMember& B)
		{
			if (A.DiskBytes != B.DiskBytes)
			{
				return A.DiskBytes > B.DiskBytes;
			}
			return A.PackageName.LexicalLess(B.PackageName);
		});
		Group.SourceFile = Pair.Value[0].SourceFile;

		for (int32 Index = 1; Index < Group.Members.Num(); ++Index)
		{
			Group.WastedDiskBytes += Group.Members[Index].DiskBytes;
		}

		// Memory is only wasted by copies that actually load: every used copy beyond the first.
		bool bFirstUsedSeen = false;
		for (const FPulseDuplicateMember& Member : Group.Members)
		{
			if (!Member.bUsed)
			{
				continue;
			}
			if (bFirstUsedSeen)
			{
				Group.WastedMemory += Member.Memory;
			}
			bFirstUsedSeen = true;
		}

		if (Group.WastedDiskBytes >= MinWasteBytes || Group.WastedMemory.GetResidentBytes() > 0)
		{
			OutReport.Duplicates.Add(MoveTemp(Group));
		}
	}

	OutReport.Duplicates.Sort([](const FPulseDuplicateGroup& A, const FPulseDuplicateGroup& B)
	{
		if (A.WastedMemory.GetResidentBytes() != B.WastedMemory.GetResidentBytes())
		{
			return A.WastedMemory.GetResidentBytes() > B.WastedMemory.GetResidentBytes();
		}
		if (A.WastedDiskBytes != B.WastedDiskBytes)
		{
			return A.WastedDiskBytes > B.WastedDiskBytes;
		}
		return A.Members[0].PackageName.LexicalLess(B.Members[0].PackageName);
	});

	// ---- Config and source mentions ------------------------------------------------------------------
	if (Config.bSearchConfigAndSource && OutReport.Unused.Num() > 0)
	{
		if (!ReportProgress(LOCTEXT("StageSearch", "Searching config and source for asset paths"), 0.85f))
		{
			OutError = TEXT("Cancelled.");
			return false;
		}
		OutReport.NumFilesSearched = SearchConfigAndSource(OutReport);
		OutReport.bSearchedConfigAndSource = true;
	}

	OutReport.Unused.Sort([](const FPulseCleanupAsset& A, const FPulseCleanupAsset& B)
	{
		if (A.DiskBytes != B.DiskBytes)
		{
			return A.DiskBytes > B.DiskBytes;
		}
		return A.PackageName.LexicalLess(B.PackageName);
	});

	ReportProgress(LOCTEXT("StageDone", "Done"), 1.0f);
	return true;
}

int32 FPulseCleanupAnalyzer::SearchConfigAndSource(FPulseCleanupReport& Report) const
{
	// Look up by package name, and search only for the mount points unused content actually lives
	// under, so a project full of "/Script/" strings costs nothing.
	TMap<FName, int32> UnusedByPackage;
	TSet<FString> MountPoints;
	for (int32 Index = 0; Index < Report.Unused.Num(); ++Index)
	{
		const FString Package = Report.Unused[Index].PackageName.ToString();
		UnusedByPackage.Add(Report.Unused[Index].PackageName, Index);

		int32 SecondSlash = INDEX_NONE;
		if (Package.Len() > 1 && Package.RightChop(1).FindChar(TEXT('/'), SecondSlash))
		{
			MountPoints.Add(Package.Left(SecondSlash + 2));
		}
	}

	TArray<FString> Files;
	auto Collect = [&Files](const FString& Directory, const TCHAR* Wildcard)
	{
		if (IFileManager::Get().DirectoryExists(*Directory))
		{
			IFileManager::Get().FindFilesRecursive(Files, *Directory, Wildcard, /*Files*/ true, /*Directories*/ false, /*bClearFileNames*/ false);
		}
	};
	Collect(FPaths::ProjectConfigDir(), TEXT("*.ini"));
	for (const TCHAR* Wildcard : { TEXT("*.h"), TEXT("*.cpp"), TEXT("*.inl"), TEXT("*.hpp") })
	{
		Collect(FPaths::GameSourceDir(), Wildcard);
	}
	// Project plugins: their Config folders and their own source, never build output.
	TArray<FString> PluginDirs;
	IFileManager::Get().FindFiles(PluginDirs, *(FPaths::ProjectPluginsDir() / TEXT("*")), /*Files*/ false, /*Directories*/ true);
	for (const FString& PluginDir : PluginDirs)
	{
		const FString Root = FPaths::ProjectPluginsDir() / PluginDir;
		Collect(Root / TEXT("Config"), TEXT("*.ini"));
		for (const TCHAR* Wildcard : { TEXT("*.h"), TEXT("*.cpp"), TEXT("*.inl"), TEXT("*.hpp") })
		{
			Collect(Root / TEXT("Source"), Wildcard);
		}
	}
	Files.Sort();

	TMap<int32, TArray<FString>> MentionsByAsset;
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	int32 NumRead = 0;
	for (const FString& File : Files)
	{
		if (IFileManager::Get().FileSize(*File) > GPulseCleanupMaxSearchFileBytes)
		{
			continue;
		}
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			continue;
		}
		NumRead++;

		FString Relative = FPaths::ConvertRelativePathToFull(File);
		FPaths::MakePathRelativeTo(Relative, *ProjectDir);

		for (const FString& Mount : MountPoints)
		{
			int32 Start = 0;
			while ((Start = Text.Find(Mount, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start)) != INDEX_NONE)
			{
				// A package path runs to the first quote, bracket, dot (object name) or separator.
				int32 End = Start + Mount.Len();
				while (End < Text.Len() && !PulseIsPathTerminator(Text[End]))
				{
					End++;
				}
				if (const int32* AssetIndex = UnusedByPackage.Find(FName(*Text.Mid(Start, End - Start))))
				{
					TArray<FString>& Mentions = MentionsByAsset.FindOrAdd(*AssetIndex);
					if (!Mentions.Contains(Relative))
					{
						Mentions.Add(Relative);
					}
				}
				Start = End;
			}
		}
	}

	for (const TPair<int32, TArray<FString>>& Pair : MentionsByAsset)
	{
		Report.Unused[Pair.Key].MentionedIn = FString::Join(Pair.Value, TEXT("; "));
	}
	return NumRead;
}

#undef LOCTEXT_NAMESPACE
