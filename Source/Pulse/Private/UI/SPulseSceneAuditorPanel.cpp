// Copyright (c) 2026 Pulse contributors. MIT License.

#include "UI/SPulseSceneAuditorPanel.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IContentBrowserSingleton.h"
#include "ISettingsModule.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "Pulse.h"
#include "PulseSceneAuditorSettings.h"
#include "SceneAuditor/PulseSceneAnalyzer.h"
#include "SceneAuditor/PulseSceneReportWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "PulseSceneAuditorPanel"

static const FName GPulseSceneColName(TEXT("Name"));
static const FName GPulseSceneColSeverity(TEXT("Severity"));
static const FName GPulseSceneColCount(TEXT("Count"));
static const FName GPulseSceneColPrimitives(TEXT("Primitives"));
static const FName GPulseSceneColTriangles(TEXT("Triangles"));
static const FName GPulseSceneColPairs(TEXT("Pairs"));
static const FName GPulseSceneColLights(TEXT("Lights"));

static FSlateColor PulseSeverityColor(EPulseSeverity Severity)
{
	switch (Severity)
	{
	case EPulseSeverity::Critical:
	case EPulseSeverity::High:   return FSlateColor(FStyleColors::Error);
	case EPulseSeverity::Medium: return FSlateColor(FStyleColors::Warning);
	default:                     return FSlateColor::UseForeground();
	}
}

class SPulseSceneRow : public SMultiColumnTableRow<TSharedPtr<FPulseSceneTreeItem>>
{
public:
	SLATE_BEGIN_ARGS(SPulseSceneRow)
	{
	}
	SLATE_ARGUMENT(TSharedPtr<FPulseSceneTreeItem>, Item)
	SLATE_ARGUMENT(const FPulseSceneReport*, Report)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Item = InArgs._Item;
		Report = InArgs._Report;
		SMultiColumnTableRow<TSharedPtr<FPulseSceneTreeItem>>::Construct(FSuperRowType::FArguments(), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid() || Report == nullptr || !Report->Maps.IsValidIndex(Item->MapIndex))
		{
			return SNullWidget::NullWidget;
		}
		const FPulseSceneMap& Map = Report->Maps[Item->MapIndex];
		const FPulseSceneFinding* Finding = Map.Findings.IsValidIndex(Item->FindingIndex) ? &Map.Findings[Item->FindingIndex] : nullptr;

		if (ColumnName == GPulseSceneColName)
		{
			FText Label;
			FText Tooltip;
			if (Finding != nullptr)
			{
				Label = FText::FromString(Finding->Message);
				TArray<FString> Lines;
				Lines.Add(Finding->Recommendation);
				if (Finding->ActorLabels.Num() > 0)
				{
					Lines.Add(FString());
					Lines.Add(FString::Printf(TEXT("Actors (%d%s):"), Finding->ActorLabels.Num(), Finding->Count > Finding->ActorLabels.Num() ? TEXT(", list capped") : TEXT("")));
					for (int32 Index = 0; Index < FMath::Min(25, Finding->ActorLabels.Num()); ++Index)
					{
						Lines.Add(Finding->ActorLabels[Index]);
					}
				}
				Tooltip = FText::FromString(FString::Join(Lines, TEXT("\n")));
			}
			else
			{
				Label = FText::FromString(Map.MapName);
				Tooltip = FText::FromString(Map.bAudited
					? (Map.bWorldPartition ? Map.MapPath + TEXT("\nWorld Partition: persistent level and always-loaded actors only.") : Map.MapPath)
					: FString::Printf(TEXT("%s\nNot audited: %s"), *Map.MapPath, *Map.SkipReason));
			}

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SExpanderArrow, SharedThis(this)).IndentAmount(12.0f)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Label)
					.ToolTipText(Tooltip)
					.Font(Finding == nullptr ? FCoreStyle::GetDefaultFontStyle("Bold", 9) : FCoreStyle::GetDefaultFontStyle("Regular", 9))
				];
		}

		if (ColumnName == GPulseSceneColSeverity)
		{
			if (Finding != nullptr)
			{
				return SNew(STextBlock).Text(FText::FromString(PulseSeverityToString(Finding->Severity)))
					.ToolTipText(FText::FromString(Finding->RuleId))
					.ColorAndOpacity(PulseSeverityColor(Finding->Severity));
			}
			if (!Map.bAudited)
			{
				return SNew(STextBlock).Text(LOCTEXT("NotAudited", "not audited")).ToolTipText(FText::FromString(Map.SkipReason));
			}
			const int32 High = Map.CountFindings(EPulseSeverity::High);
			const int32 Medium = Map.CountFindings(EPulseSeverity::Medium) - High;
			const int32 Low = Map.CountFindings(EPulseSeverity::Low) - High - Medium;
			return SNew(STextBlock)
				.Text(FText::FromString(Map.Findings.Num() == 0 ? FString(TEXT("clean")) : FString::Printf(TEXT("%dH %dM %dL"), High, Medium, Low)))
				.ColorAndOpacity(High > 0 ? PulseSeverityColor(EPulseSeverity::High) : Medium > 0 ? PulseSeverityColor(EPulseSeverity::Medium) : FSlateColor::UseForeground());
		}

		if (ColumnName == GPulseSceneColCount)
		{
			return Finding != nullptr ? StaticCastSharedRef<SWidget>(SNew(STextBlock).Text(FText::AsNumber(Finding->Count))) : SNullWidget::NullWidget;
		}

		if (Finding != nullptr || !Map.bAudited)
		{
			return SNullWidget::NullWidget;
		}

		if (ColumnName == GPulseSceneColPrimitives)
		{
			return SNew(STextBlock).Text(FText::AsNumber(Map.NumPrimitives))
				.ToolTipText(FText::Format(LOCTEXT("PrimitivesTooltip", "{0} static mesh components ({1} Nanite), {2} instanced components holding {3} instances, {4} skeletal meshes, {5} actors."),
					FText::AsNumber(Map.NumStaticMeshComponents), FText::AsNumber(Map.NumNaniteComponents), FText::AsNumber(Map.NumInstancedComponents),
					FText::AsNumber(Map.NumInstances), FText::AsNumber(Map.NumSkeletalMeshComponents), FText::AsNumber(Map.NumActors)));
		}
		if (ColumnName == GPulseSceneColTriangles)
		{
			return SNew(STextBlock).Text(FText::AsNumber(Map.NonNaniteTriangles))
				.ToolTipText(LOCTEXT("TrianglesTooltip", "LOD0 triangles across every non-Nanite static mesh placement, instances included."));
		}
		if (ColumnName == GPulseSceneColPairs)
		{
			return SNew(STextBlock).Text(FText::AsNumber(Map.NumMeshMaterialPairs))
				.ToolTipText(FText::Format(LOCTEXT("PairsTooltip", "Distinct (mesh, material) pairs among non-Nanite placements: roughly what the renderer batches into. {0} unique meshes, {1} unique materials, {2} translucent primitives."),
					FText::AsNumber(Map.NumUniqueMeshes), FText::AsNumber(Map.NumUniqueMaterials), FText::AsNumber(Map.NumTranslucentPrimitives)));
		}
		if (ColumnName == GPulseSceneColLights)
		{
			return SNew(STextBlock).Text(FText::Format(LOCTEXT("LightsCell", "{0} ({1} dyn. shadow)"), FText::AsNumber(Map.NumLights), FText::AsNumber(Map.NumMovableShadowLights)))
				.ToolTipText(FText::Format(LOCTEXT("LightsTooltip", "{0} lights, {1} stationary, {2} movable casting shadows. {3} movable primitives cast dynamic shadows."),
					FText::AsNumber(Map.NumLights), FText::AsNumber(Map.NumStationaryLights), FText::AsNumber(Map.NumMovableShadowLights), FText::AsNumber(Map.NumMovableShadowCasters)));
		}
		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FPulseSceneTreeItem> Item;
	const FPulseSceneReport* Report = nullptr;
};

void SPulseSceneAuditorPanel::Construct(const FArguments& InArgs)
{
	StatusLine = TEXT("Audit the open map to inspect it in place, or audit the maps that ship (Maps to Cook, or every map).");

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			BuildControlsRow()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 0.0f)
		[
			SNew(STextBlock).AutoWrapText(true).Text(this, &SPulseSceneAuditorPanel::GetStatusText)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			SNew(SSeparator)
		]
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f)
		[
			BuildTree()
		]
	];
}

TSharedRef<SWidget> SPulseSceneAuditorPanel::BuildControlsRow()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("AuditOpen", "Audit open map"))
			.ToolTipText(LOCTEXT("AuditOpenTooltip", "Inspect the level open in the editor, as it stands. Findings can then select their actors in the viewport."))
			.IsEnabled_Static(&SPulseSceneAuditorPanel::HasOpenMap)
			.OnClicked(this, &SPulseSceneAuditorPanel::OnAuditOpenMapClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("AuditMaps", "Audit maps"))
			.ToolTipText(LOCTEXT("AuditMapsTooltip", "Load Maps to Cook (or every map under the scan paths) one at a time, with their streaming sublevels, and inspect each. Nothing is modified or saved."))
			.OnClicked(this, &SPulseSceneAuditorPanel::OnAuditMapsClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Export", "Export report"))
			.ToolTipText(LOCTEXT("ExportTooltip", "Write Maps.csv, Findings.csv and Run.json, identical to the commandlet's output."))
			.IsEnabled(this, &SPulseSceneAuditorPanel::HasReport)
			.OnClicked(this, &SPulseSceneAuditorPanel::OnExportClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Clear", "Clear"))
			.IsEnabled(this, &SPulseSceneAuditorPanel::HasReport)
			.OnClicked(this, &SPulseSceneAuditorPanel::OnClearClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Settings", "Settings"))
			.OnClicked(this, &SPulseSceneAuditorPanel::OnOpenSettingsClicked)
		];
}

TSharedRef<SWidget> SPulseSceneAuditorPanel::BuildTree()
{
	return SAssignNew(TreeView, STreeView<TSharedPtr<FPulseSceneTreeItem>>)
		.TreeItemsSource(&Items)
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SPulseSceneAuditorPanel::OnGenerateRow)
		.OnGetChildren(this, &SPulseSceneAuditorPanel::OnGetChildren)
		.OnMouseButtonDoubleClick(this, &SPulseSceneAuditorPanel::OnItemDoubleClicked)
		.OnContextMenuOpening(this, &SPulseSceneAuditorPanel::OnContextMenuOpening)
		.HeaderRow(
			SNew(SHeaderRow)
			+ SHeaderRow::Column(GPulseSceneColName).DefaultLabel(LOCTEXT("ColName", "Map / finding")).FillWidth(0.44f)
			+ SHeaderRow::Column(GPulseSceneColSeverity).DefaultLabel(LOCTEXT("ColSeverity", "Severity")).FillWidth(0.09f)
			+ SHeaderRow::Column(GPulseSceneColCount).DefaultLabel(LOCTEXT("ColCount", "Count")).FillWidth(0.06f)
			+ SHeaderRow::Column(GPulseSceneColPrimitives).DefaultLabel(LOCTEXT("ColPrimitives", "Primitives")).FillWidth(0.08f)
			+ SHeaderRow::Column(GPulseSceneColTriangles).DefaultLabel(LOCTEXT("ColTriangles", "Non-Nanite tris")).FillWidth(0.11f)
			+ SHeaderRow::Column(GPulseSceneColPairs).DefaultLabel(LOCTEXT("ColPairs", "Mesh/material")).FillWidth(0.09f)
			+ SHeaderRow::Column(GPulseSceneColLights).DefaultLabel(LOCTEXT("ColLights", "Lights")).FillWidth(0.13f)
		);
}

bool SPulseSceneAuditorPanel::HasOpenMap()
{
	return GEditor != nullptr && GEditor->PlayWorld == nullptr && GEditor->GetEditorWorldContext().World() != nullptr;
}

FText SPulseSceneAuditorPanel::GetStatusText() const
{
	return FText::FromString(StatusLine);
}

FReply SPulseSceneAuditorPanel::OnAuditOpenMapClicked()
{
	UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World == nullptr)
	{
		return FReply::Handled();
	}

	const UPulseSceneAuditorSettings& Settings = *GetDefault<UPulseSceneAuditorSettings>();
	Report = FPulseSceneAnalyzer::MakeReport(Settings.Thresholds);
	FPulseSceneMap& Map = Report.Maps.AddDefaulted_GetRef();
	Map.MapPath = World->GetOutermost()->GetName();
	Map.MapName = FPackageName::GetShortName(Map.MapPath);
	FPulseSceneAnalyzer::AuditWorld(*World, Settings.Thresholds, /*bLiveEditorWorld*/ true, Map);
	bHasReport = true;
	RebuildItems();

	if (Items.Num() > 0 && TreeView.IsValid())
	{
		TreeView->SetItemExpansion(Items[0], true);
	}
	StatusLine = FString::Printf(TEXT("%s: %d primitives, %d finding(s). Right-click a finding to select its actors."),
		*Map.MapName, Map.NumPrimitives, Map.Findings.Num());
	return FReply::Handled();
}

FReply SPulseSceneAuditorPanel::OnAuditMapsClicked()
{
	FPulseSceneConfig Config;
	Config.ApplySettingsDefaults(*GetDefault<UPulseSceneAuditorSettings>());

	FPulseSceneReport NewReport;
	FString Error;
	bool bSucceeded = false;
	{
		FScopedSlowTask SlowTask(1.0f, LOCTEXT("Auditing", "Auditing maps..."));
		SlowTask.MakeDialog(/*bShowCancelButton*/ true);
		float ReportedFraction = 0.0f;
		FPulseSceneAnalyzer Analyzer(Config);
		bSucceeded = Analyzer.Run(NewReport, Error, [&SlowTask, &ReportedFraction](const FText& Stage, float Fraction)
		{
			const float Delta = FMath::Max(Fraction - ReportedFraction, 0.0f);
			ReportedFraction += Delta;
			SlowTask.EnterProgressFrame(Delta, Stage);
			return !SlowTask.ShouldCancel();
		});
	}

	// A cancelled run still shows what it got through.
	if (NewReport.Maps.Num() > 0)
	{
		Report = MoveTemp(NewReport);
		bHasReport = true;
		RebuildItems();
	}

	int32 NumFindings = 0;
	for (const FPulseSceneMap& Map : Report.Maps)
	{
		NumFindings += Map.Findings.Num();
	}
	StatusLine = bSucceeded
		? FString::Printf(TEXT("Audited %d map(s): %d finding(s). Worst maps first. Open a map to select a finding's actors."), Report.Maps.Num(), NumFindings)
		: FString::Printf(TEXT("Stopped: %s"), *Error);
	return FReply::Handled();
}

FReply SPulseSceneAuditorPanel::OnExportClicked()
{
	FPulseSceneConfig Config;
	Config.ApplySettingsDefaults(*GetDefault<UPulseSceneAuditorSettings>());
	FPulseSceneReportWriter Writer(Config.ReportDir, Config.MaxHistoricalRuns);
	FString Error;
	if (!Writer.Write(Report, Error))
	{
		StatusLine = FString::Printf(TEXT("Export failed: %s"), *Error);
		return FReply::Handled();
	}
	StatusLine = FString::Printf(TEXT("Exported to %s"), *Writer.GetRunDir());
	UE_LOG(LogPulse, Display, TEXT("Scene audit report written to %s"), *Writer.GetRunDir());
	return FReply::Handled();
}

FReply SPulseSceneAuditorPanel::OnClearClicked()
{
	Report = FPulseSceneReport();
	bHasReport = false;
	RebuildItems();
	StatusLine.Reset();
	return FReply::Handled();
}

FReply SPulseSceneAuditorPanel::OnOpenSettingsClicked()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
	{
		SettingsModule->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("PulseSceneAuditorSettings"));
	}
	return FReply::Handled();
}

void SPulseSceneAuditorPanel::RebuildItems()
{
	Items.Reset();
	if (bHasReport)
	{
		for (int32 MapIndex = 0; MapIndex < Report.Maps.Num(); ++MapIndex)
		{
			TSharedPtr<FPulseSceneTreeItem> MapItem = MakeShared<FPulseSceneTreeItem>();
			MapItem->MapIndex = MapIndex;
			for (int32 FindingIndex = 0; FindingIndex < Report.Maps[MapIndex].Findings.Num(); ++FindingIndex)
			{
				TSharedPtr<FPulseSceneTreeItem> FindingItem = MakeShared<FPulseSceneTreeItem>();
				FindingItem->MapIndex = MapIndex;
				FindingItem->FindingIndex = FindingIndex;
				MapItem->Children.Add(MoveTemp(FindingItem));
			}
			Items.Add(MoveTemp(MapItem));
		}
	}
	if (TreeView.IsValid())
	{
		TreeView->RebuildList();
	}
}

TSharedRef<ITableRow> SPulseSceneAuditorPanel::OnGenerateRow(TSharedPtr<FPulseSceneTreeItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SPulseSceneRow, OwnerTable).Item(Item).Report(&Report);
}

void SPulseSceneAuditorPanel::OnGetChildren(TSharedPtr<FPulseSceneTreeItem> Item, TArray<TSharedPtr<FPulseSceneTreeItem>>& OutChildren)
{
	if (Item.IsValid())
	{
		OutChildren = Item->Children;
	}
}

void SPulseSceneAuditorPanel::OnItemDoubleClicked(TSharedPtr<FPulseSceneTreeItem> Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	if (Item->FindingIndex == INDEX_NONE)
	{
		if (TreeView.IsValid())
		{
			TreeView->SetItemExpansion(Item, !TreeView->IsItemExpanded(Item));
		}
		return;
	}
	if (IsMapOpen(Item->MapIndex))
	{
		SelectActors(Item);
	}
	else
	{
		BrowseToAsset(Item);
	}
}

bool SPulseSceneAuditorPanel::IsMapOpen(int32 MapIndex) const
{
	if (!Report.Maps.IsValidIndex(MapIndex) || GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{
		return false;
	}
	const UWorld* World = GEditor->GetEditorWorldContext().World();
	if (World == nullptr)
	{
		return false;
	}
	// A sublevel row counts as open when that sublevel is loaded into the editor world.
	for (const ULevel* Level : World->GetLevels())
	{
		if (Level != nullptr && Level->GetOutermost()->GetName() == Report.Maps[MapIndex].MapPath)
		{
			return true;
		}
	}
	return false;
}

TSharedPtr<SWidget> SPulseSceneAuditorPanel::OnContextMenuOpening()
{
	if (!TreeView.IsValid())
	{
		return nullptr;
	}
	const TArray<TSharedPtr<FPulseSceneTreeItem>> Selected = TreeView->GetSelectedItems();
	if (Selected.Num() != 1 || !Report.Maps.IsValidIndex(Selected[0]->MapIndex))
	{
		return nullptr;
	}
	const TSharedPtr<FPulseSceneTreeItem> Item = Selected[0];
	const FPulseSceneMap& Map = Report.Maps[Item->MapIndex];
	const bool bOpen = IsMapOpen(Item->MapIndex);

	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection*/ true, nullptr);

	if (Item->FindingIndex != INDEX_NONE)
	{
		const FPulseSceneFinding& Finding = Map.Findings[Item->FindingIndex];
		MenuBuilder.AddMenuEntry(
			LOCTEXT("SelectActors", "Select Actors"),
			bOpen ? LOCTEXT("SelectActorsTooltip", "Select this finding's actors in the level and frame them in the viewport.")
				  : LOCTEXT("SelectActorsClosedTooltip", "Open this map first (right-click the map row) to select its actors."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"),
			FUIAction(
				FExecuteAction::CreateSP(this, &SPulseSceneAuditorPanel::SelectActors, Item),
				FCanExecuteAction::CreateLambda([bCanSelect = bOpen && Finding.ActorNames.Num() > 0]() { return bCanSelect; })));

		if (!Finding.AssetPath.IsEmpty())
		{
			MenuBuilder.AddMenuEntry(
				LOCTEXT("BrowseAsset", "Browse to Asset"),
				LOCTEXT("BrowseAssetTooltip", "Select the asset this finding is about in the Content Browser."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "SystemWideCommands.FindInContentBrowser"),
				FUIAction(FExecuteAction::CreateSP(this, &SPulseSceneAuditorPanel::BrowseToAsset, Item)));
		}
		MenuBuilder.AddMenuEntry(
			LOCTEXT("CopyActors", "Copy Actor Names"),
			LOCTEXT("CopyActorsTooltip", "Copy the actor labels, one per line."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
			FUIAction(FExecuteAction::CreateSP(this, &SPulseSceneAuditorPanel::CopyActorNames, Item)));
	}
	else
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("OpenMap", "Open Map"),
			LOCTEXT("OpenMapTooltip", "Prompts to save, then opens this map so its findings can select actors."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Levels"),
			FUIAction(
				FExecuteAction::CreateSP(this, &SPulseSceneAuditorPanel::OpenMap, Item),
				FCanExecuteAction::CreateLambda([bOpen]() { return !bOpen; })));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("BrowseMap", "Browse to Map"),
			FText::GetEmpty(),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "SystemWideCommands.FindInContentBrowser"),
			FUIAction(FExecuteAction::CreateSP(this, &SPulseSceneAuditorPanel::BrowseToAsset, Item)));
	}
	return MenuBuilder.MakeWidget();
}

void SPulseSceneAuditorPanel::SelectActors(TSharedPtr<FPulseSceneTreeItem> Item) const
{
	if (!Item.IsValid() || !IsMapOpen(Item->MapIndex) || GEditor == nullptr)
	{
		return;
	}
	const FPulseSceneMap& Map = Report.Maps[Item->MapIndex];
	if (!Map.Findings.IsValidIndex(Item->FindingIndex))
	{
		return;
	}
	const FPulseSceneFinding& Finding = Map.Findings[Item->FindingIndex];
	UWorld* World = GEditor->GetEditorWorldContext().World();

	GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
	TArray<AActor*> Selected;
	for (ULevel* Level : World->GetLevels())
	{
		if (Level == nullptr || Level->GetOutermost()->GetName() != Map.MapPath)
		{
			continue;
		}
		for (const FString& Name : Finding.ActorNames)
		{
			if (AActor* Actor = FindObjectFast<AActor>(Level, FName(*Name)))
			{
				GEditor->SelectActor(Actor, /*bInSelected*/ true, /*bNotify*/ false);
				Selected.Add(Actor);
			}
		}
	}
	GEditor->NoteSelectionChange();
	if (Selected.Num() > 0)
	{
		GEditor->MoveViewportCamerasToActor(Selected, /*bActiveViewportOnly*/ true);
	}
}

void SPulseSceneAuditorPanel::BrowseToAsset(TSharedPtr<FPulseSceneTreeItem> Item) const
{
	if (!Item.IsValid() || !Report.Maps.IsValidIndex(Item->MapIndex))
	{
		return;
	}
	const FPulseSceneMap& Map = Report.Maps[Item->MapIndex];
	FString ObjectPath;
	if (Map.Findings.IsValidIndex(Item->FindingIndex))
	{
		ObjectPath = Map.Findings[Item->FindingIndex].AssetPath;
	}
	else
	{
		ObjectPath = Map.MapPath + TEXT(".") + FPackageName::GetShortName(Map.MapPath);
	}
	if (ObjectPath.IsEmpty())
	{
		return;
	}

	const IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	if (AssetData.IsValid())
	{
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(TArray<FAssetData>{ AssetData });
	}
}

void SPulseSceneAuditorPanel::CopyActorNames(TSharedPtr<FPulseSceneTreeItem> Item) const
{
	if (Item.IsValid() && Report.Maps.IsValidIndex(Item->MapIndex) && Report.Maps[Item->MapIndex].Findings.IsValidIndex(Item->FindingIndex))
	{
		FPlatformApplicationMisc::ClipboardCopy(*FString::Join(Report.Maps[Item->MapIndex].Findings[Item->FindingIndex].ActorLabels, LINE_TERMINATOR));
	}
}

void SPulseSceneAuditorPanel::OpenMap(TSharedPtr<FPulseSceneTreeItem> Item)
{
	if (!Item.IsValid() || !Report.Maps.IsValidIndex(Item->MapIndex))
	{
		return;
	}

	// Prompt first, always: loading a map discards unsaved work in the current one.
	if (!FEditorFileUtils::SaveDirtyPackages(/*bPromptUserToSave*/ true, /*bSaveMapPackages*/ true, /*bSaveContentPackages*/ true))
	{
		StatusLine = TEXT("Map not opened: unsaved changes were kept.");
		return;
	}

	FString Filename;
	if (!FPackageName::DoesPackageExist(Report.Maps[Item->MapIndex].MapPath, &Filename))
	{
		StatusLine = TEXT("That map no longer exists.");
		return;
	}
	FEditorFileUtils::LoadMap(Filename, /*bLoadAsTemplate*/ false, /*bShowProgress*/ true);
	StatusLine = TEXT("Map opened. Right-click a finding to select its actors.");
}

#undef LOCTEXT_NAMESPACE
