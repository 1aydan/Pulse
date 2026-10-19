// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseSceneAnalyzer.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorWorldUtils.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "HAL/IConsoleManager.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Pulse.h"
#include "PulseSceneAuditorSettings.h"
#include "Settings/ProjectPackagingSettings.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "PulseSceneAnalyzer"

/** Actors listed per finding. The count is always exact; the list is for selecting and reading. */
static constexpr int32 GPulseSceneMaxActorsPerFinding = 200;

// ---- Config -------------------------------------------------------------------------------------

static TArray<FString> PulseSceneSplitList(const FString& Value)
{
	TArray<FString> Entries;
	Value.ParseIntoArray(Entries, TEXT(","), /*InCullEmpty*/ true);
	for (FString& Entry : Entries)
	{
		Entry.TrimStartAndEndInline();
	}
	Entries.RemoveAll([](const FString& Entry) { return Entry.IsEmpty(); });
	return Entries;
}

bool FPulseSceneConfig::Parse(const FString& Params, FPulseSceneConfig& OutConfig, FString& OutError)
{
	FString RawValue;
	if (FParse::Value(*Params, TEXT("maps="), RawValue))
	{
		OutConfig.Maps = PulseSceneSplitList(RawValue);
		if (OutConfig.Maps.Num() == 0)
		{
			OutError = TEXT("-maps= was given but listed no maps.");
			return false;
		}
	}
	if (FParse::Value(*Params, TEXT("path="), RawValue))
	{
		OutConfig.PathFilter = PulseSceneSplitList(RawValue);
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
	return true;
}

void FPulseSceneConfig::ApplySettingsDefaults(const UPulseSceneAuditorSettings& Settings)
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
	bOnlyMapsToCook = Settings.bOnlyMapsToCook && !bAllMapsFromCommandLine;
	Thresholds = Settings.Thresholds;
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

FString FPulseSceneConfig::ToString() const
{
	return FString::Printf(TEXT("maps=[%s] paths=[%s] mapstocook=%s reportdir=%s"),
		*FString::Join(Maps, TEXT(",")), *FString::Join(PathFilter, TEXT(",")),
		bOnlyMapsToCook ? TEXT("only") : TEXT("ignored"), *ReportDir);
}

// ---- Audit --------------------------------------------------------------------------------------

/** Adds an actor to a finding's list, respecting the cap. */
static void PulseAddActor(FPulseSceneFinding& Finding, const AActor* Actor)
{
	if (Actor != nullptr && Finding.ActorNames.Num() < GPulseSceneMaxActorsPerFinding && !Finding.ActorNames.Contains(Actor->GetName()))
	{
		Finding.ActorNames.Add(Actor->GetName());
		Finding.ActorLabels.Add(Actor->GetActorLabel());
	}
}

static FPulseSceneFinding& PulseAddFinding(FPulseSceneMap& Map, const TCHAR* RuleId, EPulseSeverity Severity, const FString& Message, const TCHAR* Recommendation)
{
	FPulseSceneFinding& Finding = Map.Findings.AddDefaulted_GetRef();
	Finding.RuleId = RuleId;
	Finding.Severity = Severity;
	Finding.Message = Message;
	Finding.Recommendation = Recommendation;
	return Finding;
}

static bool PulseUsesTranslucency(const UPrimitiveComponent& Primitive)
{
	TArray<UMaterialInterface*> Materials;
	Primitive.GetUsedMaterials(Materials);
	for (const UMaterialInterface* Material : Materials)
	{
		if (Material != nullptr && IsTranslucentBlendMode(Material->GetBlendMode()))
		{
			return true;
		}
	}
	return false;
}

/** Same mesh at the same place: a copy-paste or a double import nobody can see. */
static FString PulseDuplicateKey(const UStaticMesh& Mesh, const FTransform& Transform)
{
	const FVector Location = Transform.GetLocation();
	const FRotator Rotation = Transform.Rotator();
	const FVector Scale = Transform.GetScale3D();
	return FString::Printf(TEXT("%s|%.0f,%.0f,%.0f|%.1f,%.1f,%.1f|%.3f,%.3f,%.3f"), *Mesh.GetPathName(),
		Location.X, Location.Y, Location.Z, Rotation.Pitch, Rotation.Yaw, Rotation.Roll, Scale.X, Scale.Y, Scale.Z);
}

void FPulseSceneAnalyzer::AuditWorld(UWorld& World, const FPulseSceneThresholds& Thresholds, bool bLiveEditorWorld, FPulseSceneMap& OutMap)
{
	OutMap.bAudited = true;
	OutMap.bWorldPartition = World.IsPartitionedWorld();

	// Cull distance volumes write each primitive's CachedMaxDrawDistance, but only when something asks
	// them to. Without this a world loaded for the audit reads as having no cull distances at all.
	World.UpdateCullDistanceVolumes();

	// Placements of each non-Nanite mesh, as separate components.
	TMap<const UStaticMesh*, TArray<const AActor*>> MeshPlacements;
	TMap<FString, TArray<const AActor*>> DuplicateGroups;
	TSet<const UStaticMesh*> UniqueMeshes;
	TSet<const UMaterialInterface*> UniqueMaterials;
	TSet<TPair<const UStaticMesh*, const UMaterialInterface*>> MeshMaterialPairs;
	TArray<const AActor*> SmallPropsWithoutCull;
	TArray<const AActor*> MovableShadowLights;
	TArray<const AActor*> LargeShadowLights;
	TArray<const AActor*> OverlappingStationaryLights;
	TArray<const AActor*> MovableShadowCasters;
	TArray<const AActor*> TranslucentActors;

	// Stationary lights only get shadowmap channels when the project builds static lighting. With it
	// off — the usual Lumen setup — there is no channel to run out of and the overlap rule means nothing.
	static const TConsoleVariableData<int32>* AllowStaticLighting = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.AllowStaticLighting"));
	const bool bStaticLighting = AllowStaticLighting == nullptr || AllowStaticLighting->GetValueOnAnyThread() != 0;

	for (const ULevel* Level : World.GetLevels())
	{
		if (Level == nullptr)
		{
			continue;
		}
		for (const AActor* Actor : Level->Actors)
		{
			if (!IsValid(Actor) || Actor->IsEditorOnly())
			{
				continue;
			}
			OutMap.NumActors++;

			TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
			for (const UPrimitiveComponent* Primitive : Primitives)
			{
				// What never renders in game costs nothing in game: editor-only helpers, hidden components.
				if (!IsValid(Primitive) || Primitive->IsEditorOnly() || Primitive->bHiddenInGame || Primitive->IsVisualizationComponent())
				{
					continue;
				}
				OutMap.NumPrimitives++;

				if (Primitive->Mobility == EComponentMobility::Movable && Primitive->CastShadow && Primitive->bCastDynamicShadow)
				{
					OutMap.NumMovableShadowCasters++;
					MovableShadowCasters.AddUnique(Actor);
				}
				if (PulseUsesTranslucency(*Primitive))
				{
					OutMap.NumTranslucentPrimitives++;
					TranslucentActors.AddUnique(Actor);
				}

				if (Primitive->IsA<USkeletalMeshComponent>())
				{
					OutMap.NumSkeletalMeshComponents++;
					continue;
				}

				const UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Primitive);
				const UStaticMesh* Mesh = MeshComponent != nullptr ? MeshComponent->GetStaticMesh() : nullptr;
				if (Mesh == nullptr)
				{
					continue;
				}

				UniqueMeshes.Add(Mesh);
				for (int32 Slot = 0; Slot < MeshComponent->GetNumMaterials(); ++Slot)
				{
					UniqueMaterials.Add(MeshComponent->GetMaterial(Slot));
				}

				const bool bNanite = Mesh->HasValidNaniteData();
				const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(MeshComponent);
				const int32 Instances = Instanced != nullptr ? Instanced->GetInstanceCount() : 1;

				if (Instanced != nullptr)
				{
					OutMap.NumInstancedComponents++;
					OutMap.NumInstances += Instances;
				}
				else
				{
					OutMap.NumStaticMeshComponents++;
				}

				if (bNanite)
				{
					OutMap.NumNaniteComponents++;
					continue;
				}

				OutMap.NonNaniteTriangles += static_cast<int64>(Mesh->GetNumTriangles(0)) * Instances;
				for (int32 Slot = 0; Slot < MeshComponent->GetNumMaterials(); ++Slot)
				{
					MeshMaterialPairs.Add(TPair<const UStaticMesh*, const UMaterialInterface*>(Mesh, MeshComponent->GetMaterial(Slot)));
				}

				if (Instanced == nullptr)
				{
					MeshPlacements.FindOrAdd(Mesh).Add(Actor);
					DuplicateGroups.FindOrAdd(PulseDuplicateKey(*Mesh, MeshComponent->GetComponentTransform())).Add(Actor);

					// Small and drawn at any distance: no per-component distance, no cull distance volume.
					const float Radius = MeshComponent->CalcBounds(MeshComponent->GetComponentTransform()).SphereRadius;
					if (Thresholds.SmallPropRadius > 0.0f && Radius < Thresholds.SmallPropRadius
						&& MeshComponent->LDMaxDrawDistance <= 0.0f && MeshComponent->CachedMaxDrawDistance <= 0.0f)
					{
						SmallPropsWithoutCull.AddUnique(Actor);
					}
				}
			}

			TInlineComponentArray<ULightComponent*> Lights(Actor);
			for (ULightComponent* Light : Lights)
			{
				if (!IsValid(Light) || !Light->bAffectsWorld || !Light->IsVisible())
				{
					continue;
				}
				OutMap.NumLights++;

				if (Light->Mobility == EComponentMobility::Movable && Light->CastShadows)
				{
					OutMap.NumMovableShadowLights++;
					MovableShadowLights.AddUnique(Actor);
					const ULocalLightComponent* LocalLight = Cast<ULocalLightComponent>(Light);
					if (LocalLight != nullptr && Thresholds.LargeShadowLightRadius > 0.0f && LocalLight->AttenuationRadius >= Thresholds.LargeShadowLightRadius)
					{
						LargeShadowLights.AddUnique(Actor);
					}
				}
				else if (Light->Mobility == EComponentMobility::Stationary)
				{
					OutMap.NumStationaryLights++;
					if (Light->CastShadows && bStaticLighting)
					{
						// The lighting build records which of the four shadowmap channels each stationary
						// light got; INDEX_NONE means it overlapped too many others and casts fully dynamic
						// shadows instead. Unbuilt lighting has no record, and the editor's live preview of
						// the same packing exists only for the map open in the editor.
						if (const FLightComponentMapBuildData* BuildData = Light->GetLightComponentMapBuildData())
						{
							if (BuildData->ShadowMapChannel == INDEX_NONE)
							{
								OverlappingStationaryLights.AddUnique(Actor);
							}
						}
						else if (bLiveEditorWorld && Light->PreviewShadowMapChannel == INDEX_NONE)
						{
							OverlappingStationaryLights.AddUnique(Actor);
						}
					}
				}
			}
		}
	}

	OutMap.NumUniqueMeshes = UniqueMeshes.Num();
	OutMap.NumUniqueMaterials = UniqueMaterials.Num();
	OutMap.NumMeshMaterialPairs = MeshMaterialPairs.Num();

	// ---- Findings -------------------------------------------------------------------------------

	if (OverlappingStationaryLights.Num() > 0)
	{
		FPulseSceneFinding& Finding = PulseAddFinding(OutMap, TEXT("Scene.StationaryLightOverlap"), EPulseSeverity::High,
			FString::Printf(TEXT("%d stationary light(s) got no shadowmap channel: more than four overlap, so they cast fully dynamic shadows."), OverlappingStationaryLights.Num()),
			TEXT("Shrink their attenuation radii so no point is touched by more than four, make some Static, or turn off Cast Shadows on the least important. The editor's Stationary Light Overlap view mode shows where."));
		Finding.Count = OverlappingStationaryLights.Num();
		for (const AActor* Actor : OverlappingStationaryLights)
		{
			PulseAddActor(Finding, Actor);
		}
	}

	// Repeated meshes, worst first so the biggest wins lead the list.
	TArray<TPair<const UStaticMesh*, TArray<const AActor*>>> Repeated;
	for (TPair<const UStaticMesh*, TArray<const AActor*>>& Pair : MeshPlacements)
	{
		if (Thresholds.RepeatedMeshMinCount > 0 && Pair.Value.Num() >= Thresholds.RepeatedMeshMinCount)
		{
			Repeated.Add(Pair);
		}
	}
	Repeated.Sort([](const auto& A, const auto& B) { return A.Value.Num() > B.Value.Num(); });
	for (const TPair<const UStaticMesh*, TArray<const AActor*>>& Pair : Repeated)
	{
		const bool bSevere = Pair.Value.Num() >= Thresholds.RepeatedMeshMinCount * 4;
		FPulseSceneFinding& Finding = PulseAddFinding(OutMap, TEXT("Scene.RepeatedMesh"), bSevere ? EPulseSeverity::High : EPulseSeverity::Medium,
			FString::Printf(TEXT("%s is placed as %d separate non-Nanite components (%d triangles each)."),
				*Pair.Key->GetName(), Pair.Value.Num(), Pair.Key->GetNumTriangles(0)),
			TEXT("Enable Nanite on the mesh, or turn the placements into one Instanced or Hierarchical Instanced Static Mesh (Merge Actors > Batch, foliage, or PCG). Each separate component is culled, sorted and updated on its own."));
		Finding.AssetPath = Pair.Key->GetPathName();
		Finding.Count = Pair.Value.Num();
		for (const AActor* Actor : Pair.Value)
		{
			PulseAddActor(Finding, Actor);
		}
	}

	// Stacked duplicates: one finding for the whole map, every extra copy listed.
	int32 NumExtraCopies = 0;
	FPulseSceneFinding DuplicateFinding;
	for (const TPair<FString, TArray<const AActor*>>& Pair : DuplicateGroups)
	{
		if (Pair.Value.Num() < 2)
		{
			continue;
		}
		NumExtraCopies += Pair.Value.Num() - 1;
		for (const AActor* Actor : Pair.Value)
		{
			PulseAddActor(DuplicateFinding, Actor);
		}
	}
	if (NumExtraCopies > 0)
	{
		FPulseSceneFinding& Finding = PulseAddFinding(OutMap, TEXT("Scene.StackedDuplicates"), EPulseSeverity::Medium,
			FString::Printf(TEXT("%d mesh placement(s) sit exactly on top of an identical one."), NumExtraCopies),
			TEXT("Delete the extra copies. They render, cast shadows and collide twice and look identical. Usually a duplicate-in-place (Ctrl+D, Alt-drag) that never moved."));
		Finding.Count = NumExtraCopies;
		Finding.ActorNames = MoveTemp(DuplicateFinding.ActorNames);
		Finding.ActorLabels = MoveTemp(DuplicateFinding.ActorLabels);
	}

	if (Thresholds.MaxMovableShadowLights > 0 && MovableShadowLights.Num() > Thresholds.MaxMovableShadowLights)
	{
		FPulseSceneFinding& Finding = PulseAddFinding(OutMap, TEXT("Scene.MovableShadowLights"), EPulseSeverity::Medium,
			FString::Printf(TEXT("%d movable lights cast shadows (limit %d)."), MovableShadowLights.Num(), Thresholds.MaxMovableShadowLights),
			TEXT("Every movable shadow-casting light re-renders shadow depth for what it reaches, every frame. Make lights that never move Stationary or Static, and turn off shadows where they add nothing."));
		Finding.Count = MovableShadowLights.Num();
		for (const AActor* Actor : MovableShadowLights)
		{
			PulseAddActor(Finding, Actor);
		}
	}

	if (LargeShadowLights.Num() > 0)
	{
		FPulseSceneFinding& Finding = PulseAddFinding(OutMap, TEXT("Scene.LargeShadowLight"), EPulseSeverity::Low,
			FString::Printf(TEXT("%d movable shadow-casting light(s) reach %.0f cm or more."), LargeShadowLights.Num(), Thresholds.LargeShadowLightRadius),
			TEXT("Shadow cost grows with everything inside the radius. Shrink it to what the light visibly needs."));
		Finding.Count = LargeShadowLights.Num();
		for (const AActor* Actor : LargeShadowLights)
		{
			PulseAddActor(Finding, Actor);
		}
	}

	if (Thresholds.MaxPrimitives > 0 && OutMap.NumPrimitives > Thresholds.MaxPrimitives)
	{
		FPulseSceneFinding& Finding = PulseAddFinding(OutMap, TEXT("Scene.TooManyPrimitives"), EPulseSeverity::Medium,
			FString::Printf(TEXT("%d primitive components (limit %d)%s."), OutMap.NumPrimitives, Thresholds.MaxPrimitives,
				OutMap.bWorldPartition ? TEXT(" in the persistent level and always-loaded actors") : TEXT("")),
			TEXT("Every primitive costs scene management, culling and GPU scene upkeep. Instance repeated meshes, merge static clusters, or stream the map (World Partition, level streaming)."));
		Finding.Count = OutMap.NumPrimitives;
	}

	if (Thresholds.MaxMovableShadowCasters > 0 && MovableShadowCasters.Num() > Thresholds.MaxMovableShadowCasters)
	{
		FPulseSceneFinding& Finding = PulseAddFinding(OutMap, TEXT("Scene.MovableShadowCasters"), EPulseSeverity::Low,
			FString::Printf(TEXT("%d actors have movable primitives casting dynamic shadows (limit %d)."), MovableShadowCasters.Num(), Thresholds.MaxMovableShadowCasters),
			TEXT("Movable casters are re-rendered into shadow maps whenever a light sees them. Set props that never move to Static."));
		Finding.Count = MovableShadowCasters.Num();
		for (const AActor* Actor : MovableShadowCasters)
		{
			PulseAddActor(Finding, Actor);
		}
	}

	if (Thresholds.MaxSmallPropsWithoutCullDistance > 0 && SmallPropsWithoutCull.Num() > Thresholds.MaxSmallPropsWithoutCullDistance)
	{
		FPulseSceneFinding& Finding = PulseAddFinding(OutMap, TEXT("Scene.NoCullDistance"), EPulseSeverity::Low,
			FString::Printf(TEXT("%d small props (under %.0f cm) have no draw distance and render at any range."), SmallPropsWithoutCull.Num(), Thresholds.SmallPropRadius),
			TEXT("Add a Cull Distance Volume covering the area, or set Desired Max Draw Distance on the props. Small things far away cost a draw and contribute nothing visible."));
		Finding.Count = SmallPropsWithoutCull.Num();
		for (const AActor* Actor : SmallPropsWithoutCull)
		{
			PulseAddActor(Finding, Actor);
		}
	}

	if (Thresholds.MaxTranslucentPrimitives > 0 && OutMap.NumTranslucentPrimitives > Thresholds.MaxTranslucentPrimitives)
	{
		FPulseSceneFinding& Finding = PulseAddFinding(OutMap, TEXT("Scene.ManyTranslucent"), EPulseSeverity::Low,
			FString::Printf(TEXT("%d primitives use a translucent material (limit %d)."), OutMap.NumTranslucentPrimitives, Thresholds.MaxTranslucentPrimitives),
			TEXT("Translucency is sorted and drawn per primitive with no early depth rejection, and overlapping layers multiply shading cost. Use Masked where an edge is enough."));
		Finding.Count = OutMap.NumTranslucentPrimitives;
		for (const AActor* Actor : TranslucentActors)
		{
			PulseAddActor(Finding, Actor);
		}
	}

	if (Thresholds.HighPolyTriangles > 0)
	{
		for (const TPair<const UStaticMesh*, TArray<const AActor*>>& Pair : MeshPlacements)
		{
			const int32 Triangles = Pair.Key->GetNumTriangles(0);
			if (Triangles < Thresholds.HighPolyTriangles)
			{
				continue;
			}
			FPulseSceneFinding& Finding = PulseAddFinding(OutMap, TEXT("Scene.HighPolyNonNanite"), Pair.Value.Num() >= 10 ? EPulseSeverity::Medium : EPulseSeverity::Low,
				FString::Printf(TEXT("%s has %d triangles at LOD0, is not Nanite, and is placed %d time(s)."), *Pair.Key->GetName(), Triangles, Pair.Value.Num()),
				TEXT("Enable Nanite on it, or give it LODs. Without either, every placement draws its full triangle count at any distance."));
			Finding.AssetPath = Pair.Key->GetPathName();
			Finding.Count = Pair.Value.Num();
			for (const AActor* Actor : Pair.Value)
			{
				PulseAddActor(Finding, Actor);
			}
		}
	}

	OutMap.Findings.Sort([](const FPulseSceneFinding& A, const FPulseSceneFinding& B)
	{
		if (A.Severity != B.Severity)
		{
			return static_cast<uint8>(A.Severity) > static_cast<uint8>(B.Severity);
		}
		if (A.Count != B.Count)
		{
			return A.Count > B.Count;
		}
		return A.Message < B.Message;
	});
}

// ---- Runs ---------------------------------------------------------------------------------------

FPulseSceneAnalyzer::FPulseSceneAnalyzer(const FPulseSceneConfig& InConfig)
	: Config(InConfig)
{
}

FPulseSceneReport FPulseSceneAnalyzer::MakeReport(const FPulseSceneThresholds& Thresholds)
{
	FPulseSceneReport Report;
	Report.GeneratedAtUtc = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	Report.EngineVersion = FEngineVersion::Current().ToString();
	Report.Thresholds = Thresholds;
	return Report;
}

void FPulseSceneAnalyzer::SortMaps(TArray<FPulseSceneMap>& Maps)
{
	Maps.Sort([](const FPulseSceneMap& A, const FPulseSceneMap& B)
	{
		if (A.bAudited != B.bAudited)
		{
			return A.bAudited;
		}
		const int32 HighA = A.CountFindings(EPulseSeverity::High);
		const int32 HighB = B.CountFindings(EPulseSeverity::High);
		if (HighA != HighB)
		{
			return HighA > HighB;
		}
		const int32 MediumA = A.CountFindings(EPulseSeverity::Medium);
		const int32 MediumB = B.CountFindings(EPulseSeverity::Medium);
		if (MediumA != MediumB)
		{
			return MediumA > MediumB;
		}
		if (A.NumPrimitives != B.NumPrimitives)
		{
			return A.NumPrimitives > B.NumPrimitives;
		}
		return A.MapPath < B.MapPath;
	});
}

TArray<FString> FPulseSceneAnalyzer::ResolveMaps() const
{
	TArray<FString> Maps = Config.Maps;

	if (Maps.Num() == 0 && Config.bOnlyMapsToCook)
	{
		for (const FFilePath& Map : GetDefault<UProjectPackagingSettings>()->MapsToCook)
		{
			FString Path = Map.FilePath;
			Path.TrimStartAndEndInline();
			if (!Path.IsEmpty())
			{
				Maps.AddUnique(Path.StartsWith(TEXT("/")) ? Path : TEXT("/Game/") + Path);
			}
		}
	}

	if (Maps.Num() == 0)
	{
		const IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
		Filter.bRecursivePaths = true;
		for (const FString& Path : Config.PathFilter)
		{
			Filter.PackagePaths.Add(FName(*Path));
		}
		TArray<FAssetData> Worlds;
		Registry.GetAssets(Filter, Worlds);
		for (const FAssetData& World : Worlds)
		{
			const FString Package = World.PackageName.ToString();
			const bool bExcluded = Config.ExcludePathPatterns.ContainsByPredicate([&Package](const FString& Pattern)
			{
				return !Pattern.IsEmpty() && Package.Contains(Pattern, ESearchCase::IgnoreCase);
			});
			if (!bExcluded)
			{
				Maps.AddUnique(Package);
			}
		}
	}

	Maps.Sort();
	return Maps;
}

void FPulseSceneAnalyzer::AuditMapPackage(const FString& PackageName, FPulseSceneMap& OutMap, TArray<FString>& OutSublevels) const
{
	OutMap.MapPath = PackageName;
	if (OutMap.MapName.IsEmpty())
	{
		OutMap.MapName = FPackageName::GetShortName(PackageName);
	}

	// The map already open in the editor is audited as it stands, unsaved edits and loaded sublevels
	// included — GetLevels covers those, so none are queued.
	if (GEditor != nullptr && !IsRunningCommandlet())
	{
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			if (EditorWorld->GetOutermost()->GetName() == PackageName)
			{
				AuditWorld(*EditorWorld, Config.Thresholds, /*bLiveEditorWorld*/ true, OutMap);
				return;
			}
		}
	}

	if (!FPackageName::DoesPackageExist(PackageName))
	{
		OutMap.SkipReason = TEXT("Map package does not exist.");
		return;
	}

	UPackage* Package = LoadWorldPackageForEditor(PackageName);
	UWorld* World = Package != nullptr ? UWorld::FindWorldInPackage(Package) : nullptr;
	if (World == nullptr)
	{
		OutMap.SkipReason = TEXT("The map failed to load.");
		return;
	}

	for (const ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		if (Streaming != nullptr)
		{
			const FString Sublevel = Streaming->GetWorldAssetPackageName();
			if (!Sublevel.IsEmpty())
			{
				OutSublevels.AddUnique(UWorld::RemovePIEPrefix(Sublevel));
			}
		}
	}

	if (World->IsInitialized())
	{
		AuditWorld(*World, Config.Thresholds, /*bLiveEditorWorld*/ false, OutMap);
		return;
	}

	// Everything a world can host is off, as in the level collector: the scope exists only to make
	// the level's actors and components real enough to inspect.
	UWorld::InitializationValues InitValues;
	InitValues.RequiresHitProxies(false);
	InitValues.ShouldSimulatePhysics(false);
	InitValues.EnableTraceCollision(false);
	InitValues.CreateNavigation(false);
	InitValues.CreateAISystem(false);
	InitValues.AllowAudioPlayback(false);
	InitValues.CreatePhysicsScene(false);

	FScopedEditorWorld ScopedWorld(World, InitValues);
	if (ScopedWorld.GetWorld() == nullptr)
	{
		OutMap.SkipReason = TEXT("The map could not be initialized for inspection.");
		return;
	}
	AuditWorld(*ScopedWorld.GetWorld(), Config.Thresholds, /*bLiveEditorWorld*/ false, OutMap);
}

bool FPulseSceneAnalyzer::Run(FPulseSceneReport& OutReport, FString& OutError, const FProgressCallback& Progress)
{
	OutReport = MakeReport(Config.Thresholds);

	// A work queue rather than a fixed list: a persistent map's streaming sublevels are discovered as
	// it is loaded and audited right after it, as rows named "Persistent / Sublevel".
	TArray<FString> Maps = ResolveMaps();
	if (Maps.Num() == 0)
	{
		OutError = TEXT("No maps to audit. Check Maps to Cook, the scan paths, or pass -maps=.");
		return false;
	}
	TMap<FString, FString> DisplayNames;

	for (int32 Index = 0; Index < Maps.Num(); ++Index)
	{
		if (Progress && !Progress(FText::Format(LOCTEXT("Stage", "Auditing {0} ({1} of {2})"),
			FText::FromString(FPackageName::GetShortName(Maps[Index])), FText::AsNumber(Index + 1), FText::AsNumber(Maps.Num())),
			static_cast<float>(Index) / static_cast<float>(Maps.Num())))
		{
			OutError = TEXT("Cancelled.");
			SortMaps(OutReport.Maps);
			return false;
		}

		FPulseSceneMap& Map = OutReport.Maps.AddDefaulted_GetRef();
		if (const FString* DisplayName = DisplayNames.Find(Maps[Index]))
		{
			Map.MapName = *DisplayName;
		}
		TArray<FString> Sublevels;
		AuditMapPackage(Maps[Index], Map, Sublevels);

		int32 InsertAt = Index + 1;
		for (const FString& Sublevel : Sublevels)
		{
			if (!Maps.Contains(Sublevel))
			{
				DisplayNames.Add(Sublevel, Map.MapName + TEXT(" / ") + FPackageName::GetShortName(Sublevel));
				Maps.Insert(Sublevel, InsertAt++);
			}
		}
		UE_LOG(LogPulse, Display, TEXT("Pulse.Scene: %s: %s"), *Map.MapName,
			Map.bAudited ? *FString::Printf(TEXT("%d primitives, %d finding(s)"), Map.NumPrimitives, Map.Findings.Num()) : *Map.SkipReason);

		// A map and everything it loaded goes before the next comes in.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}

	SortMaps(OutReport.Maps);
	return true;
}

#undef LOCTEXT_NAMESPACE
