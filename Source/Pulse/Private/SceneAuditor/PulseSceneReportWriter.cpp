// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseSceneReportWriter.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "PulseCsvWriter.h"
#include "PulseSceneResult.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

static const TCHAR* GPulseSceneRunPrefix = TEXT("SceneAudit_");

FPulseSceneReportWriter::FPulseSceneReportWriter(const FString& InReportDir, int32 InMaxHistoricalRuns)
	: ReportDir(InReportDir)
	, MaxHistoricalRuns(InMaxHistoricalRuns)
{
}

bool FPulseSceneReportWriter::Write(const FPulseSceneReport& Report, FString& OutError)
{
	RunDir = ReportDir / (GPulseSceneRunPrefix + Report.GeneratedAtUtc);
	IFileManager::Get().MakeDirectory(*RunDir, /*Tree*/ true);
	if (!IFileManager::Get().DirectoryExists(*RunDir))
	{
		OutError = FString::Printf(TEXT("Failed to create report directory %s"), *RunDir);
		return false;
	}
	if (!WriteMapsCsv(Report, OutError) || !WriteFindingsCsv(Report, OutError) || !WriteRunJson(Report, OutError))
	{
		return false;
	}
	PruneHistoricalRuns();
	return true;
}

bool FPulseSceneReportWriter::WriteMapsCsv(const FPulseSceneReport& Report, FString& OutError) const
{
	FPulseCsvWriter Csv;
	Csv.WriteRow({
		TEXT("Map"), TEXT("MapPath"), TEXT("Audited"), TEXT("SkipReason"), TEXT("WorldPartition"),
		TEXT("Actors"), TEXT("Primitives"), TEXT("StaticMeshComponents"), TEXT("NaniteComponents"),
		TEXT("InstancedComponents"), TEXT("Instances"), TEXT("SkeletalMeshComponents"), TEXT("NonNaniteTriangles"),
		TEXT("UniqueMeshes"), TEXT("UniqueMaterials"), TEXT("MeshMaterialPairs"),
		TEXT("Lights"), TEXT("StationaryLights"), TEXT("MovableShadowLights"), TEXT("MovableShadowCasters"), TEXT("TranslucentPrimitives"),
		TEXT("HighFindings"), TEXT("MediumFindings"), TEXT("LowFindings")
	});

	for (const FPulseSceneMap& Map : Report.Maps)
	{
		const int32 High = Map.CountFindings(EPulseSeverity::High);
		const int32 Medium = Map.CountFindings(EPulseSeverity::Medium) - High;
		const int32 Low = Map.CountFindings(EPulseSeverity::Low) - High - Medium;
		Csv.WriteRow({
			Map.MapName, Map.MapPath, Map.bAudited ? TEXT("true") : TEXT("false"), Map.SkipReason,
			Map.bWorldPartition ? TEXT("true") : TEXT("false"),
			FString::FromInt(Map.NumActors), FString::FromInt(Map.NumPrimitives), FString::FromInt(Map.NumStaticMeshComponents),
			FString::FromInt(Map.NumNaniteComponents), FString::FromInt(Map.NumInstancedComponents), FString::FromInt(Map.NumInstances),
			FString::FromInt(Map.NumSkeletalMeshComponents), FString::Printf(TEXT("%lld"), Map.NonNaniteTriangles),
			FString::FromInt(Map.NumUniqueMeshes), FString::FromInt(Map.NumUniqueMaterials), FString::FromInt(Map.NumMeshMaterialPairs),
			FString::FromInt(Map.NumLights), FString::FromInt(Map.NumStationaryLights), FString::FromInt(Map.NumMovableShadowLights),
			FString::FromInt(Map.NumMovableShadowCasters), FString::FromInt(Map.NumTranslucentPrimitives),
			FString::FromInt(High), FString::FromInt(Medium), FString::FromInt(Low)
		});
	}
	return Csv.SaveToFile(RunDir / TEXT("Maps.csv"), OutError);
}

bool FPulseSceneReportWriter::WriteFindingsCsv(const FPulseSceneReport& Report, FString& OutError) const
{
	FPulseCsvWriter Csv;
	Csv.WriteRow({ TEXT("Map"), TEXT("Rule"), TEXT("Severity"), TEXT("Count"), TEXT("Asset"), TEXT("Message"), TEXT("Recommendation"), TEXT("Actors") });
	for (const FPulseSceneMap& Map : Report.Maps)
	{
		for (const FPulseSceneFinding& Finding : Map.Findings)
		{
			Csv.WriteRow({
				Map.MapName, Finding.RuleId, PulseSeverityToString(Finding.Severity), FString::FromInt(Finding.Count),
				Finding.AssetPath, Finding.Message, Finding.Recommendation, FString::Join(Finding.ActorLabels, TEXT("; "))
			});
		}
	}
	return Csv.SaveToFile(RunDir / TEXT("Findings.csv"), OutError);
}

bool FPulseSceneReportWriter::WriteRunJson(const FPulseSceneReport& Report, FString& OutError) const
{
	int32 NumAudited = 0;
	int32 NumFindings = 0;
	for (const FPulseSceneMap& Map : Report.Maps)
	{
		NumAudited += Map.bAudited ? 1 : 0;
		NumFindings += Map.Findings.Num();
	}

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("engineVersion"), Report.EngineVersion);
	Writer->WriteValue(TEXT("maps"), Report.Maps.Num());
	Writer->WriteValue(TEXT("mapsAudited"), NumAudited);
	Writer->WriteValue(TEXT("findings"), NumFindings);

	const FPulseSceneThresholds& T = Report.Thresholds;
	Writer->WriteObjectStart(TEXT("thresholds"));
	Writer->WriteValue(TEXT("repeatedMeshMinCount"), T.RepeatedMeshMinCount);
	Writer->WriteValue(TEXT("highPolyTriangles"), T.HighPolyTriangles);
	Writer->WriteValue(TEXT("maxPrimitives"), T.MaxPrimitives);
	Writer->WriteValue(TEXT("maxMovableShadowLights"), T.MaxMovableShadowLights);
	Writer->WriteValue(TEXT("largeShadowLightRadius"), T.LargeShadowLightRadius);
	Writer->WriteValue(TEXT("maxMovableShadowCasters"), T.MaxMovableShadowCasters);
	Writer->WriteValue(TEXT("smallPropRadius"), T.SmallPropRadius);
	Writer->WriteValue(TEXT("maxSmallPropsWithoutCullDistance"), T.MaxSmallPropsWithoutCullDistance);
	Writer->WriteValue(TEXT("maxTranslucentPrimitives"), T.MaxTranslucentPrimitives);
	Writer->WriteObjectEnd();

	Writer->WriteValue(TEXT("note"),
		TEXT("Counts what each map places. World Partition maps are audited as loaded: the persistent level and always-loaded actors, not streamed cells. ")
		TEXT("Stationary light overlap comes from the lighting build, so unbuilt lighting is not checked except for the map open in the editor."));
	Writer->WriteObjectEnd();
	Writer->Close();

	const FString Path = RunDir / TEXT("Run.json");
	if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write %s"), *Path);
		return false;
	}
	return true;
}

void FPulseSceneReportWriter::PruneHistoricalRuns() const
{
	if (MaxHistoricalRuns <= 0)
	{
		return;
	}
	TArray<FString> RunFolders;
	IFileManager::Get().FindFiles(RunFolders, *(ReportDir / (FString(GPulseSceneRunPrefix) + TEXT("*"))), /*Files*/ false, /*Directories*/ true);
	if (RunFolders.Num() <= MaxHistoricalRuns)
	{
		return;
	}
	RunFolders.Sort();
	for (int32 Index = 0; Index < RunFolders.Num() - MaxHistoricalRuns; ++Index)
	{
		IFileManager::Get().DeleteDirectory(*(ReportDir / RunFolders[Index]), /*RequireExists*/ false, /*Tree*/ true);
	}
}
