// Copyright (c) 2026 Pulse contributors. MIT License.

#include "UI/SPulseMemoryAuditorPanel.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "IContentBrowserSingleton.h"
#include "ISettingsModule.h"
#include "MemoryAuditor/PulseMemoryAnalyzer.h"
#include "MemoryAuditor/PulseMemoryAuditorConfig.h"
#include "MemoryAuditor/PulseMemoryEstimator.h"
#include "MemoryAuditor/PulseMemoryReportWriter.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "Pulse.h"
#include "PulseMemoryAuditorSettings.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "PulseMemoryAuditorPanel"

static const FName GPulseMemColAsset(TEXT("Asset"));
static const FName GPulseMemColType(TEXT("Type"));
static const FName GPulseMemColRAM(TEXT("RAM"));
static const FName GPulseMemColVRAM(TEXT("VRAM"));
static const FName GPulseMemColUniqueRAM(TEXT("UniqueRAM"));
static const FName GPulseMemColUniqueVRAM(TEXT("UniqueVRAM"));
static const FName GPulseMemColReach(TEXT("Reach"));
static const FName GPulseMemColSource(TEXT("Source"));
static const FName GPulseMemColVia(TEXT("Via"));

/** A dash rather than "0 B": most assets genuinely have nothing in one of the two buckets. */
static FText PulseBytesText(int64 Bytes)
{
	return Bytes == 0 ? FText::FromString(TEXT("–")) : FText::FromString(PulseFormatBytes(Bytes));
}

static FString PulseRootKindLabel(const FPulseMemoryRoot& Root)
{
	FString Label;
	switch (Root.Kind)
	{
	case EPulseMemoryRootKind::AlwaysLoaded: Label = TEXT("Always loaded"); break;
	case EPulseMemoryRootKind::Map:          Label = Root.bWorldPartition ? TEXT("Map (WP)") : TEXT("Map"); break;
	case EPulseMemoryRootKind::PrimaryAsset: Label = FString::Printf(TEXT("Primary: %s"), *Root.KindDetail); break;
	case EPulseMemoryRootKind::Configured:   Label = TEXT("Configured"); break;
	// KindDetail is the Blueprint's own class, so Widget and Anim Blueprints read as such.
	case EPulseMemoryRootKind::Blueprint:    Label = Root.KindDetail; break;
	case EPulseMemoryRootKind::Inspected:    return TEXT("Inspected");
	case EPulseMemoryRootKind::Count:        break;
	}
	return Root.bInspected ? FString::Printf(TEXT("%s (inspected)"), *Label) : Label;
}

/**
 * The number a column sorts by. A top-level row and a dependency row read different fields for the
 * same column — a root's RAM is everything it loads, a dependency's is the package itself — which is
 * exactly what the column tooltips say.
 */
static int64 PulseSortValue(const FPulseMemoryReport& Report, const FPulseMemoryTreeItem& Item, FName Column)
{
	const FPulseMemoryNode& Node = Report.Nodes[Item.NodeIndex];
	if (Item.bIsRoot)
	{
		const FPulseMemoryRoot& Root = Report.Roots[Item.RootIndex];
		if (Column == GPulseMemColRAM)        { return Root.Total.RAMBytes; }
		if (Column == GPulseMemColVRAM)       { return Root.Total.VRAMBytes; }
		if (Column == GPulseMemColUniqueRAM)  { return Root.Unique.RAMBytes; }
		if (Column == GPulseMemColUniqueVRAM) { return Root.Unique.VRAMBytes; }
		if (Column == GPulseMemColReach)      { return Root.NumHardPackages; }
		return Root.Total.GetResidentBytes();
	}

	if (Column == GPulseMemColRAM)        { return Node.Self.RAMBytes; }
	if (Column == GPulseMemColVRAM)       { return Node.Self.VRAMBytes; }
	if (Column == GPulseMemColUniqueRAM)  { return Node.Retained.RAMBytes; }
	if (Column == GPulseMemColUniqueVRAM) { return Node.Retained.VRAMBytes; }
	if (Column == GPulseMemColReach)      { return Node.NumRootsReaching; }
	return Node.Self.GetResidentBytes();
}

/** One results row: a top-level asset, one of its dependencies, or the placeholder under an unexpanded root. */
class SPulseMemoryRow : public SMultiColumnTableRow<TSharedPtr<FPulseMemoryTreeItem>>
{
public:
	SLATE_BEGIN_ARGS(SPulseMemoryRow)
	{
	}
	SLATE_ARGUMENT(TSharedPtr<FPulseMemoryTreeItem>, Item)
	SLATE_ARGUMENT(TSharedPtr<const FPulseMemoryReport>, Report)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Item = InArgs._Item;
		Report = InArgs._Report;
		SMultiColumnTableRow<TSharedPtr<FPulseMemoryTreeItem>>::Construct(FSuperRowType::FArguments(), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid() || !Report.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		if (Item->bIsPlaceholder)
		{
			return ColumnName == GPulseMemColAsset
				? StaticCastSharedRef<SWidget>(SNew(STextBlock).Text(LOCTEXT("Computing", "...")))
				: SNullWidget::NullWidget;
		}

		const FPulseMemoryNode& Node = Report->Nodes[Item->NodeIndex];
		const FPulseMemoryRoot* Root = Item->bIsRoot ? &Report->Roots[Item->RootIndex] : nullptr;

		// Soft-only rows are greyed: they cost nothing unless code actually loads them.
		const FSlateColor TextColor = Item->bHard ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();

		if (ColumnName == GPulseMemColAsset)
		{
			const bool bOverBudget = Root != nullptr && Report->IsRootOverBudget(*Root);
			FText Tooltip = FText::FromString(Node.ObjectPath.IsEmpty() ? Node.PackageName.ToString() : Node.ObjectPath);
			if (bOverBudget)
			{
				Tooltip = FText::Format(LOCTEXT("OverBudgetTooltip", "{0}\nOver the configured RAM or VRAM budget."), Tooltip);
			}

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SExpanderArrow, SharedThis(this)).IndentAmount(12.0f)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Node.AssetName))
					.ToolTipText(Tooltip)
					.Font(Root != nullptr ? FCoreStyle::GetDefaultFontStyle("Bold", 9) : FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(bOverBudget ? FSlateColor(FStyleColors::Error) : TextColor)
				];
		}

		if (ColumnName == GPulseMemColType)
		{
			const FString Label = Root != nullptr ? PulseRootKindLabel(*Root) : Node.ClassName;
			const FString Tooltip = Root != nullptr ? FString::Printf(TEXT("%s\n%s"), *Node.ClassName, *Root->KindDetail) : Node.ClassName;
			return SNew(STextBlock).Text(FText::FromString(Label)).ToolTipText(FText::FromString(Tooltip)).ColorAndOpacity(TextColor);
		}

		if (ColumnName == GPulseMemColRAM || ColumnName == GPulseMemColVRAM)
		{
			const bool bRAM = ColumnName == GPulseMemColRAM;
			if (Root == nullptr && Node.SizeSource == EPulseMemorySizeSource::Unknown)
			{
				return SNew(STextBlock).Text(LOCTEXT("UnknownSize", "?"))
					.ToolTipText(LOCTEXT("UnknownSizeTooltip", "The registry has no size information for this package; it is counted as zero."));
			}

			const FPulseMemoryCost& Cost = Root != nullptr ? Root->Total : Node.Self;
			FString Tooltip;
			if (Root != nullptr)
			{
				Tooltip = FString::Printf(TEXT("Resident while this asset is loaded, including everything it hard-references.\nStreaming on demand (not counted): %s\nMay load through soft references: %s RAM, %s VRAM"),
					*PulseFormatBytes(Root->Total.StreamingBytes), *PulseFormatBytes(Root->MayLoad.RAMBytes), *PulseFormatBytes(Root->MayLoad.VRAMBytes));
			}
			else
			{
				Tooltip = FString::Printf(TEXT("%s\nStreaming on demand (not counted): %s\nSize from: %s"),
					*Node.SizeNote, *PulseFormatBytes(Node.Self.StreamingBytes), PulseMemorySizeSourceToString(Node.SizeSource));
			}
			return SNew(STextBlock)
				.Text(PulseBytesText(bRAM ? Cost.RAMBytes : Cost.VRAMBytes))
				.ToolTipText(FText::FromString(Tooltip))
				.ColorAndOpacity(TextColor);
		}

		if (ColumnName == GPulseMemColUniqueRAM || ColumnName == GPulseMemColUniqueVRAM)
		{
			const FPulseMemoryCost& Cost = Root != nullptr ? Root->Unique : Node.Retained;
			return SNew(STextBlock)
				.Text(PulseBytesText(ColumnName == GPulseMemColUniqueRAM ? Cost.RAMBytes : Cost.VRAMBytes))
				.ToolTipText(LOCTEXT("UniqueCellTooltip", "Leaves memory if nothing loads this asset any more: itself plus everything only it keeps loaded. Measured against what maps, primary assets and always-loaded assets load."))
				.ColorAndOpacity(TextColor);
		}

		if (ColumnName == GPulseMemColReach)
		{
			if (Root != nullptr)
			{
				return SNew(STextBlock)
					.Text(FText::AsNumber(Root->NumHardPackages))
					.ToolTipText(FText::Format(LOCTEXT("ReachRootTooltip", "{0} packages loaded with this asset; {1} more reachable only through soft references."),
						FText::AsNumber(Root->NumHardPackages), FText::AsNumber(Root->NumSoftOnlyPackages)));
			}
			return SNew(STextBlock)
				.Text(FText::AsNumber(Node.NumRootsReaching))
				.ToolTipText(FText::Format(LOCTEXT("ReachDepTooltip", "Loaded by {0} map(s), primary asset(s) or always-loaded asset(s). {1} hop(s) from this one."),
					FText::AsNumber(Node.NumRootsReaching), FText::AsNumber(Item->Depth)))
				.ColorAndOpacity(TextColor);
		}

		if (ColumnName == GPulseMemColSource)
		{
			if (Root != nullptr)
			{
				const int64 Resident = Root->Total.GetResidentBytes();
				const int32 TagPercent = Resident > 0
					? FMath::RoundToInt(100.0 * static_cast<double>(Resident - Root->ResidentBytesFromDiskSize) / static_cast<double>(Resident))
					: 100;
				return SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%d%% tags"), TagPercent)))
					.ToolTipText(FText::Format(LOCTEXT("SourceRootTooltip", "Share of the resident total computed from registry tags. The rest uses editor disk size as a stand-in, which usually overstates.\n{0} package(s) could not be sized at all."),
						FText::AsNumber(Root->NumUnknownSize)));
			}
			const TCHAR* Label = Node.SizeSource == EPulseMemorySizeSource::Tags ? TEXT("Tags")
				: Node.SizeSource == EPulseMemorySizeSource::DiskSize ? TEXT("Disk") : TEXT("?");
			return SNew(STextBlock).Text(FText::FromString(Label)).ToolTipText(FText::FromString(Node.SizeNote)).ColorAndOpacity(TextColor);
		}

		if (ColumnName == GPulseMemColVia)
		{
			if (Root != nullptr)
			{
				return SNew(STextBlock).Text(FText::FromString(Root->KindDetail)).ToolTipText(FText::FromString(Root->KindDetail));
			}

			FString Via = Item->bHard ? Item->Via : FString::Printf(TEXT("(may load) %s"), *Item->Via);

			// The chain shown is one route. Saying how many others exist up front is what stops
			// someone cutting it and wondering why the asset is still loaded.
			if (Item->NumReferencers > 1)
			{
				Via += FString::Printf(TEXT("  [+%d other referencer%s]"), Item->NumReferencers - 1, Item->NumReferencers == 2 ? TEXT("") : TEXT("s"));
			}
			const FString Tooltip = FString::Printf(TEXT("Shortest chain: %s\n\nReferenced directly by %d package(s) here. All of them must drop the reference before this asset unloads:\n%s"),
				*Item->Via, Item->NumReferencers, *Item->ReferencedBy.Replace(TEXT("; "), TEXT("\n")));
			return SNew(STextBlock).Text(FText::FromString(Via)).ToolTipText(FText::FromString(Tooltip)).ColorAndOpacity(TextColor);
		}

		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FPulseMemoryTreeItem> Item;
	TSharedPtr<const FPulseMemoryReport> Report;
};

void SPulseMemoryAuditorPanel::Construct(const FArguments& InArgs)
{
	StatusLine = TEXT("Press Analyze to rank top-level assets by the memory they keep loaded.");

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			BuildControlsRow()
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 0.0f)
		[
			SNew(STextBlock).Text(this, &SPulseMemoryAuditorPanel::GetStatusText)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(this, &SPulseMemoryAuditorPanel::GetSummaryText)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SSeparator)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 0.0f)
		[
			BuildFilterRow()
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(6.0f, 2.0f)
		[
			SNew(STextBlock)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("DropHint", "Drag assets here from the Content Browser to inspect them, or right-click any row and choose Inspect."))
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(4.0f)
		[
			BuildResultsTree()
		]
	];
}

TSharedRef<SWidget> SPulseMemoryAuditorPanel::BuildControlsRow()
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Analyze", "Analyze"))
			.ToolTipText(LOCTEXT("AnalyzeTooltip", "Walk every top-level asset's dependencies through the Asset Registry and estimate what each keeps in memory. Nothing is loaded."))
			.OnClicked(this, &SPulseMemoryAuditorPanel::OnRunClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Export", "Export report"))
			.ToolTipText(LOCTEXT("ExportTooltip", "Write Roots.csv, Dependencies.csv, Assets.csv and Run.json, identical to the commandlet's output."))
			.IsEnabled(this, &SPulseMemoryAuditorPanel::IsExportEnabled)
			.OnClicked(this, &SPulseMemoryAuditorPanel::OnExportClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Clear", "Clear"))
			.ToolTipText(LOCTEXT("ClearTooltip", "Clear the results and every inspected asset."))
			.IsEnabled(this, &SPulseMemoryAuditorPanel::IsClearEnabled)
			.OnClicked(this, &SPulseMemoryAuditorPanel::OnClearClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Settings", "Settings"))
			.OnClicked(this, &SPulseMemoryAuditorPanel::OnOpenSettingsClicked)
		];
}

TSharedRef<SWidget> SPulseMemoryAuditorPanel::BuildFilterRow()
{
	auto MakeKindToggle = [this](EPulseMemoryRootKind Kind, const FText& Label)
	{
		return SNew(SCheckBox)
			.IsChecked(this, &SPulseMemoryAuditorPanel::GetKindFilterState, Kind)
			.OnCheckStateChanged(this, &SPulseMemoryAuditorPanel::OnKindFilterChanged, Kind)
			[
				SNew(STextBlock).Text(Label)
			];
	};

	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2.0f)
		[
			SNew(SSearchBox)
			.HintText(LOCTEXT("SearchHint", "Filter top-level assets"))
			.OnTextChanged(this, &SPulseMemoryAuditorPanel::OnSearchTextChanged)
		]

		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 2.0f).VAlign(VAlign_Center)
		[
			MakeKindToggle(EPulseMemoryRootKind::AlwaysLoaded, LOCTEXT("KindAlwaysLoaded", "Always loaded"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 2.0f).VAlign(VAlign_Center)
		[
			MakeKindToggle(EPulseMemoryRootKind::Map, LOCTEXT("KindMaps", "Maps"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 2.0f).VAlign(VAlign_Center)
		[
			MakeKindToggle(EPulseMemoryRootKind::PrimaryAsset, LOCTEXT("KindPrimary", "Primary assets"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 2.0f).VAlign(VAlign_Center)
		[
			MakeKindToggle(EPulseMemoryRootKind::Blueprint, LOCTEXT("KindBlueprints", "Blueprints"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 2.0f).VAlign(VAlign_Center)
		[
			MakeKindToggle(EPulseMemoryRootKind::Configured, LOCTEXT("KindConfigured", "Configured"))
		]

		+ SHorizontalBox::Slot().AutoWidth().Padding(12.0f, 2.0f, 2.0f, 2.0f).VAlign(VAlign_Center)
		[
			SNew(SCheckBox)
			.IsChecked(this, &SPulseMemoryAuditorPanel::GetShowMayLoadState)
			.OnCheckStateChanged(this, &SPulseMemoryAuditorPanel::OnShowMayLoadChanged)
			.ToolTipText(LOCTEXT("ShowMayLoadTooltip", "List packages reachable only through soft references (greyed) under each expanded asset. They never count toward the totals."))
			[
				SNew(STextBlock).Text(LOCTEXT("ShowMayLoad", "Show may-load"))
			]
		];
}

TSharedRef<SWidget> SPulseMemoryAuditorPanel::BuildResultsTree()
{
	auto SortableColumn = [this](const FName& Id, const FText& Label, const FText& Tooltip, float Width)
	{
		return SHeaderRow::Column(Id)
			.DefaultLabel(Label)
			.DefaultTooltip(Tooltip)
			.FillWidth(Width)
			.SortMode(this, &SPulseMemoryAuditorPanel::GetColumnSortMode, Id)
			.OnSort(this, &SPulseMemoryAuditorPanel::OnColumnSortModeChanged);
	};

	return SAssignNew(TreeView, STreeView<TSharedPtr<FPulseMemoryTreeItem>>)
		.TreeItemsSource(&VisibleRootItems)
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SPulseMemoryAuditorPanel::OnGenerateRow)
		.OnGetChildren(this, &SPulseMemoryAuditorPanel::OnGetChildren)
		.OnExpansionChanged(this, &SPulseMemoryAuditorPanel::OnExpansionChanged)
		.OnMouseButtonDoubleClick(this, &SPulseMemoryAuditorPanel::OnItemDoubleClicked)
		.OnContextMenuOpening(this, &SPulseMemoryAuditorPanel::OnContextMenuOpening)
		.HeaderRow(
			SNew(SHeaderRow)
			+ SortableColumn(GPulseMemColAsset, LOCTEXT("ColAsset", "Asset"),
				LOCTEXT("ColAssetTooltip", "Top-level assets in bold; expand one to list every package it loads."), 0.24f)
			+ SHeaderRow::Column(GPulseMemColType).DefaultLabel(LOCTEXT("ColType", "Type")).FillWidth(0.11f)
			+ SortableColumn(GPulseMemColRAM, LOCTEXT("ColRAM", "RAM"),
				LOCTEXT("ColRAMTooltip", "System memory. Top-level row: everything it keeps resident, its hard references included. Dependency row: the package itself."), 0.08f)
			+ SortableColumn(GPulseMemColVRAM, LOCTEXT("ColVRAM", "VRAM"),
				LOCTEXT("ColVRAMTooltip", "Video memory, excluding texture mips and Nanite pages that stream on demand. Top-level row: everything it keeps resident. Dependency row: the package itself."), 0.08f)
			+ SortableColumn(GPulseMemColUniqueRAM, LOCTEXT("ColUniqueRAM", "Unique RAM"),
				LOCTEXT("ColUniqueRAMTooltip", "System memory that leaves if nothing loads this asset any more: itself plus everything only it keeps loaded. Shared dependencies are not counted."), 0.08f)
			+ SortableColumn(GPulseMemColUniqueVRAM, LOCTEXT("ColUniqueVRAM", "Unique VRAM"),
				LOCTEXT("ColUniqueVRAMTooltip", "Video memory that leaves if nothing loads this asset any more."), 0.08f)
			+ SortableColumn(GPulseMemColReach, LOCTEXT("ColReach", "Reach"),
				LOCTEXT("ColReachTooltip", "Top-level row: packages it loads. Dependency row: how many maps, primary assets and always-loaded assets load it. Blueprints and inspected assets are not counted, so this reflects what the game loads."), 0.06f)
			+ SHeaderRow::Column(GPulseMemColSource).DefaultLabel(LOCTEXT("ColSource", "Src"))
				.DefaultTooltip(LOCTEXT("ColSourceTooltip", "Where the size came from: registry tags, or editor disk size as a stand-in."))
				.FillWidth(0.07f)
			+ SHeaderRow::Column(GPulseMemColVia).DefaultLabel(LOCTEXT("ColVia", "Via"))
				.DefaultTooltip(LOCTEXT("ColViaTooltip", "Top-level row: why it is top-level. Dependency row: the shortest reference chain that loads it."))
				.FillWidth(0.20f)
		);
}

FText SPulseMemoryAuditorPanel::GetStatusText() const
{
	return FText::FromString(StatusLine);
}

FText SPulseMemoryAuditorPanel::GetSummaryText() const
{
	if (!Report.IsValid())
	{
		return FText::GetEmpty();
	}

	return FText::Format(
		LOCTEXT("Summary", "Always loaded (paid in every map): {0} RAM, {1} VRAM across {2} packages{3}.   {4} top-level assets, {5} packages loaded by hard reference. Sizes are estimates from registry tags, for ranking rather than budgeting."),
		FText::FromString(PulseFormatBytes(Report->Baseline.RAMBytes)),
		FText::FromString(PulseFormatBytes(Report->Baseline.VRAMBytes)),
		FText::AsNumber(Report->NumBaselinePackages),
		Report->IsBaselineOverBudget() ? LOCTEXT("BaselineOverBudget", " (over budget)") : FText::GetEmpty(),
		FText::AsNumber(Report->Roots.Num()),
		FText::AsNumber(Report->NumReachablePackages));
}

bool SPulseMemoryAuditorPanel::IsExportEnabled() const
{
	return Report.IsValid();
}

FReply SPulseMemoryAuditorPanel::OnRunClicked()
{
	RunAnalysis();
	return FReply::Handled();
}

bool SPulseMemoryAuditorPanel::RunAnalysis()
{
	const UPulseMemoryAuditorSettings& Settings = *GetDefault<UPulseMemoryAuditorSettings>();
	FPulseMemoryAuditorConfig Config;
	Config.ApplySettingsDefaults(Settings);
	for (const FName& PackageName : InspectedPackages)
	{
		Config.InspectAssets.Add(PackageName.ToString());
	}

	TSharedPtr<FPulseMemoryReport> NewReport = MakeShared<FPulseMemoryReport>();
	FString Error;
	bool bSucceeded = false;
	{
		FScopedSlowTask SlowTask(1.0f, LOCTEXT("Analyzing", "Analyzing asset memory..."));
		SlowTask.MakeDialog(/*bShowCancelButton*/ true);

		float ReportedFraction = 0.0f;
		FPulseMemoryAnalyzer Analyzer(Config);
		bSucceeded = Analyzer.Run(*NewReport, Error, [&SlowTask, &ReportedFraction](const FText& Stage, float Fraction)
		{
			const float Delta = FMath::Max(Fraction - ReportedFraction, 0.0f);
			ReportedFraction += Delta;
			SlowTask.EnterProgressFrame(Delta, Stage);
			return !SlowTask.ShouldCancel();
		});
	}

	if (!bSucceeded)
	{
		StatusLine = FString::Printf(TEXT("Analysis stopped: %s"), *Error);
		UE_LOG(LogPulse, Warning, TEXT("Pulse.Memory: %s"), *Error);
		return false;
	}

	Report = NewReport;
	RebuildRootItems();
	StatusLine = FString::Printf(TEXT("Analyzed %d top-level assets. Expand one to see what it loads; double-click to browse, right-click for the Reference Viewer."),
		Report->Roots.Num());
	return true;
}

FReply SPulseMemoryAuditorPanel::OnClearClicked()
{
	Report.Reset();
	AllRootItems.Reset();
	VisibleRootItems.Reset();
	InspectedPackages.Reset();
	StatusLine = TEXT("Press Analyze to rank top-level assets by the memory they keep loaded.");
	if (TreeView.IsValid())
	{
		TreeView->RequestTreeRefresh();
	}
	return FReply::Handled();
}

bool SPulseMemoryAuditorPanel::IsClearEnabled() const
{
	return Report.IsValid() || InspectedPackages.Num() > 0;
}

void SPulseMemoryAuditorPanel::InspectPackages(const TArray<FName>& PackageNames)
{
	if (PackageNames.Num() == 0)
	{
		return;
	}
	for (const FName& PackageName : PackageNames)
	{
		InspectedPackages.AddUnique(PackageName);
	}

	if (RunAnalysis())
	{
		FocusRootForPackage(PackageNames[0]);
		if (PackageNames.Num() == 1)
		{
			StatusLine = FString::Printf(TEXT("Inspecting %s. Its dependencies are listed below; the row stays pinned at the top until you clear it."),
				*FPackageName::GetShortName(PackageNames[0]));
		}
		else
		{
			StatusLine = FString::Printf(TEXT("Inspecting %d assets, pinned at the top until you clear them."), PackageNames.Num());
		}
	}
}

void SPulseMemoryAuditorPanel::FocusRootForPackage(FName PackageName)
{
	if (!Report.IsValid() || !TreeView.IsValid())
	{
		return;
	}

	for (const TSharedPtr<FPulseMemoryTreeItem>& Item : VisibleRootItems)
	{
		if (Report->Nodes[Item->NodeIndex].PackageName == PackageName)
		{
			TreeView->SetSelection(Item);
			TreeView->SetItemExpansion(Item, true);
			TreeView->RequestScrollIntoView(Item);
			return;
		}
	}

	// Not in the list: the registry did not know it, so the analyzer skipped it and logged why.
	StatusLine = FString::Printf(TEXT("%s is not in the Asset Registry, so it could not be inspected."), *PackageName.ToString());
}

void SPulseMemoryAuditorPanel::OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	if (const TSharedPtr<FAssetDragDropOp> Operation = DragDropEvent.GetOperationAs<FAssetDragDropOp>())
	{
		Operation->SetToolTip(LOCTEXT("DropToInspect", "Inspect memory cost"), FAppStyle::GetBrush("Graph.ConnectorFeedback.OK"));
	}
}

void SPulseMemoryAuditorPanel::OnDragLeave(const FDragDropEvent& DragDropEvent)
{
	if (const TSharedPtr<FAssetDragDropOp> Operation = DragDropEvent.GetOperationAs<FAssetDragDropOp>())
	{
		Operation->ResetToDefaultToolTip();
	}
}

FReply SPulseMemoryAuditorPanel::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	const TSharedPtr<FAssetDragDropOp> Operation = DragDropEvent.GetOperationAs<FAssetDragDropOp>();
	return Operation.IsValid() && Operation->HasAssets() ? FReply::Handled() : FReply::Unhandled();
}

FReply SPulseMemoryAuditorPanel::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	const TSharedPtr<FAssetDragDropOp> Operation = DragDropEvent.GetOperationAs<FAssetDragDropOp>();
	if (!Operation.IsValid() || !Operation->HasAssets())
	{
		return FReply::Unhandled();
	}

	TArray<FName> PackageNames;
	for (const FAssetData& Asset : Operation->GetAssets())
	{
		PackageNames.AddUnique(Asset.PackageName);
	}
	InspectPackages(PackageNames);
	return FReply::Handled();
}

FReply SPulseMemoryAuditorPanel::OnExportClicked()
{
	if (!Report.IsValid())
	{
		return FReply::Handled();
	}

	const UPulseMemoryAuditorSettings& Settings = *GetDefault<UPulseMemoryAuditorSettings>();
	FPulseMemoryAuditorConfig Config;
	Config.ApplySettingsDefaults(Settings);

	FPulseMemoryReportWriter Writer(Config.ReportDir, Config.MaxDependenciesPerRootInReport, Config.MaxHistoricalRuns);
	FString Error;
	if (!Writer.Write(*Report, Error))
	{
		StatusLine = FString::Printf(TEXT("Export failed: %s"), *Error);
		UE_LOG(LogPulse, Error, TEXT("%s"), *Error);
		return FReply::Handled();
	}

	LastExportDir = Writer.GetRunDir();
	StatusLine = FString::Printf(TEXT("Exported to %s"), *LastExportDir);
	UE_LOG(LogPulse, Display, TEXT("Memory audit report written to %s"), *LastExportDir);
	return FReply::Handled();
}

FReply SPulseMemoryAuditorPanel::OnOpenSettingsClicked()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
	{
		SettingsModule->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("PulseMemoryAuditorSettings"));
	}
	return FReply::Handled();
}

void SPulseMemoryAuditorPanel::OnSearchTextChanged(const FText& InText)
{
	SearchText = InText.ToString();
	ApplyFilters();
}

void SPulseMemoryAuditorPanel::OnKindFilterChanged(ECheckBoxState NewState, EPulseMemoryRootKind Kind)
{
	if (NewState == ECheckBoxState::Checked)
	{
		HiddenKinds.Remove(Kind);
	}
	else
	{
		HiddenKinds.Add(Kind);
	}
	ApplyFilters();
}

ECheckBoxState SPulseMemoryAuditorPanel::GetKindFilterState(EPulseMemoryRootKind Kind) const
{
	return HiddenKinds.Contains(Kind) ? ECheckBoxState::Unchecked : ECheckBoxState::Checked;
}

void SPulseMemoryAuditorPanel::OnShowMayLoadChanged(ECheckBoxState NewState)
{
	bShowMayLoad = NewState == ECheckBoxState::Checked;
	ResetChildren();
}

ECheckBoxState SPulseMemoryAuditorPanel::GetShowMayLoadState() const
{
	return bShowMayLoad ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

/** A child that only exists so the tree draws an expander before the real children are computed. */
static TSharedPtr<FPulseMemoryTreeItem> PulseMakePlaceholder(int32 RootIndex)
{
	TSharedPtr<FPulseMemoryTreeItem> Placeholder = MakeShared<FPulseMemoryTreeItem>();
	Placeholder->RootIndex = RootIndex;
	Placeholder->bIsPlaceholder = true;
	return Placeholder;
}

void SPulseMemoryAuditorPanel::RebuildRootItems()
{
	AllRootItems.Reset();
	if (!Report.IsValid())
	{
		ApplyFilters();
		return;
	}

	for (int32 RootIndex = 0; RootIndex < Report->Roots.Num(); ++RootIndex)
	{
		const FPulseMemoryRoot& Root = Report->Roots[RootIndex];
		TSharedPtr<FPulseMemoryTreeItem> Item = MakeShared<FPulseMemoryTreeItem>();
		Item->NodeIndex = Root.NodeIndex;
		Item->RootIndex = RootIndex;
		Item->bIsRoot = true;

		// STreeView asks every item for its children to decide whether to draw an expander, so the
		// real list (a breadth-first walk) is deferred until the row is actually expanded.
		if (Root.NumHardPackages > 1 || Root.NumSoftOnlyPackages > 0)
		{
			Item->Children.Add(PulseMakePlaceholder(RootIndex));
		}
		AllRootItems.Add(MoveTemp(Item));
	}

	SortItems(AllRootItems);
	ApplyFilters();
}

void SPulseMemoryAuditorPanel::ApplyFilters()
{
	VisibleRootItems.Reset();
	if (Report.IsValid())
	{
		for (const TSharedPtr<FPulseMemoryTreeItem>& Item : AllRootItems)
		{
			const FPulseMemoryRoot& Root = Report->Roots[Item->RootIndex];

			// Inspected rows ignore the filters: they were asked for by name, and hiding one because it
			// also happens to be a Blueprint would look like the drop did nothing.
			if (Root.bInspected)
			{
				VisibleRootItems.Add(Item);
				continue;
			}
			if (HiddenKinds.Contains(Root.Kind))
			{
				continue;
			}
			if (!SearchText.IsEmpty())
			{
				const FPulseMemoryNode& Node = Report->Nodes[Item->NodeIndex];
				if (!Node.AssetName.Contains(SearchText) && !Node.PackageName.ToString().Contains(SearchText))
				{
					continue;
				}
			}
			VisibleRootItems.Add(Item);
		}
	}

	if (TreeView.IsValid())
	{
		TreeView->RequestTreeRefresh();
	}
}

void SPulseMemoryAuditorPanel::BuildChildren(const TSharedPtr<FPulseMemoryTreeItem>& RootItem)
{
	RootItem->Children.Reset();
	RootItem->bChildrenBuilt = true;
	if (!Report.IsValid())
	{
		return;
	}

	FPulseMemoryReach Reach;
	Report->ComputeReach(RootItem->NodeIndex, bShowMayLoad && Report->Estimation.bIncludeSoftReferences, Reach);
	TArray<TArray<int32>> ReferencerSlots;
	Report->CollectReferencers(Reach, ReferencerSlots);

	RootItem->Children.Reserve(Reach.Nodes.Num() - 1);
	for (int32 Slot = 1; Slot < Reach.Nodes.Num(); ++Slot)
	{
		TSharedPtr<FPulseMemoryTreeItem> Child = MakeShared<FPulseMemoryTreeItem>();
		Child->NodeIndex = Reach.Nodes[Slot];
		Child->RootIndex = RootItem->RootIndex;
		Child->bHard = Slot < Reach.NumHard;
		Child->Depth = Reach.Depths[Slot];
		Child->Via = Report->BuildViaPath(Reach, Slot);
		Child->NumReferencers = ReferencerSlots[Slot].Num();
		Child->ReferencedBy = Report->FormatReferencers(Reach, ReferencerSlots[Slot], /*MaxNames*/ 40);
		Child->bChildrenBuilt = true;
		RootItem->Children.Add(MoveTemp(Child));
	}
	SortItems(RootItem->Children);
}

void SPulseMemoryAuditorPanel::ResetChildren()
{
	for (const TSharedPtr<FPulseMemoryTreeItem>& Item : AllRootItems)
	{
		if (!Item->bChildrenBuilt)
		{
			continue;
		}

		// Rebuild what is open now so the toggle visibly takes effect; the rest go back to a
		// placeholder and rebuild when next expanded.
		if (TreeView.IsValid() && TreeView->IsItemExpanded(Item))
		{
			BuildChildren(Item);
		}
		else
		{
			Item->bChildrenBuilt = false;
			Item->Children.Reset();
			Item->Children.Add(PulseMakePlaceholder(Item->RootIndex));
		}
	}

	if (TreeView.IsValid())
	{
		TreeView->RequestTreeRefresh();
	}
}

void SPulseMemoryAuditorPanel::SortItems(TArray<TSharedPtr<FPulseMemoryTreeItem>>& Items) const
{
	if (!Report.IsValid())
	{
		return;
	}

	const FPulseMemoryReport& ReportRef = *Report;
	const FName Column = SortColumn;
	const bool bAscending = SortMode == EColumnSortMode::Ascending;

	Items.Sort([&ReportRef, Column, bAscending](const TSharedPtr<FPulseMemoryTreeItem>& Left, const TSharedPtr<FPulseMemoryTreeItem>& Right)
	{
		// Hard dependencies always above may-load ones, whatever the sort: the greyed rows are context,
		// and interleaving them with resident cost would make the list read as one ranking.
		if (Left->bHard != Right->bHard)
		{
			return Left->bHard;
		}

		// Inspected top-level rows stay pinned above the ranking, in any sort.
		const bool bLeftPinned = Left->bIsRoot && ReportRef.Roots[Left->RootIndex].bInspected;
		const bool bRightPinned = Right->bIsRoot && ReportRef.Roots[Right->RootIndex].bInspected;
		if (bLeftPinned != bRightPinned)
		{
			return bLeftPinned;
		}

		const FPulseMemoryNode& LeftNode = ReportRef.Nodes[Left->NodeIndex];
		const FPulseMemoryNode& RightNode = ReportRef.Nodes[Right->NodeIndex];

		if (Column == GPulseMemColAsset)
		{
			const int32 Compare = LeftNode.AssetName.Compare(RightNode.AssetName, ESearchCase::IgnoreCase);
			if (Compare != 0)
			{
				return bAscending ? Compare < 0 : Compare > 0;
			}
		}
		else
		{
			const int64 LeftValue = PulseSortValue(ReportRef, *Left, Column);
			const int64 RightValue = PulseSortValue(ReportRef, *Right, Column);
			if (LeftValue != RightValue)
			{
				return bAscending ? LeftValue < RightValue : LeftValue > RightValue;
			}
		}

		return LeftNode.PackageName.ToString().Compare(RightNode.PackageName.ToString(), ESearchCase::CaseSensitive) < 0;
	});
}

void SPulseMemoryAuditorPanel::SortAll()
{
	SortItems(AllRootItems);
	for (const TSharedPtr<FPulseMemoryTreeItem>& Item : AllRootItems)
	{
		if (Item->bChildrenBuilt)
		{
			SortItems(Item->Children);
		}
	}
	ApplyFilters();
}

EColumnSortMode::Type SPulseMemoryAuditorPanel::GetColumnSortMode(const FName ColumnId) const
{
	return ColumnId == SortColumn ? SortMode : EColumnSortMode::None;
}

void SPulseMemoryAuditorPanel::OnColumnSortModeChanged(const EColumnSortPriority::Type SortPriority, const FName& ColumnId, const EColumnSortMode::Type NewSortMode)
{
	// Byte columns open largest-first, which is the question being asked; the header's own toggle
	// would start them ascending.
	if (ColumnId != SortColumn)
	{
		SortColumn = ColumnId;
		SortMode = ColumnId == GPulseMemColAsset ? EColumnSortMode::Ascending : EColumnSortMode::Descending;
	}
	else
	{
		SortMode = NewSortMode;
	}
	SortAll();
}

TSharedRef<ITableRow> SPulseMemoryAuditorPanel::OnGenerateRow(TSharedPtr<FPulseMemoryTreeItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SPulseMemoryRow, OwnerTable).Item(Item).Report(Report);
}

void SPulseMemoryAuditorPanel::OnGetChildren(TSharedPtr<FPulseMemoryTreeItem> Item, TArray<TSharedPtr<FPulseMemoryTreeItem>>& OutChildren)
{
	if (Item.IsValid())
	{
		OutChildren = Item->Children;
	}
}

void SPulseMemoryAuditorPanel::OnExpansionChanged(TSharedPtr<FPulseMemoryTreeItem> Item, bool bExpanded)
{
	if (bExpanded && Item.IsValid() && Item->bIsRoot && !Item->bChildrenBuilt)
	{
		BuildChildren(Item);
		if (TreeView.IsValid())
		{
			TreeView->RequestTreeRefresh();
		}
	}
}

void SPulseMemoryAuditorPanel::OnItemDoubleClicked(TSharedPtr<FPulseMemoryTreeItem> Item)
{
	BrowseToItem(Item);
}

TSharedPtr<SWidget> SPulseMemoryAuditorPanel::OnContextMenuOpening()
{
	if (!TreeView.IsValid() || !Report.IsValid())
	{
		return nullptr;
	}

	const TArray<TSharedPtr<FPulseMemoryTreeItem>> Selected = TreeView->GetSelectedItems();
	if (Selected.Num() != 1 || !Selected[0].IsValid() || Selected[0]->bIsPlaceholder)
	{
		return nullptr;
	}
	const TSharedPtr<FPulseMemoryTreeItem> Item = Selected[0];

	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection*/ true, nullptr);

	MenuBuilder.AddMenuEntry(
		LOCTEXT("BrowseToAsset", "Browse to Asset"),
		LOCTEXT("BrowseToAssetTooltip", "Select this asset in the Content Browser."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SystemWideCommands.FindInContentBrowser"),
		FUIAction(FExecuteAction::CreateSP(this, &SPulseMemoryAuditorPanel::BrowseToItem, Item)));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("OpenReferenceViewer", "Reference Viewer"),
		LOCTEXT("OpenReferenceViewerTooltip", "Open the engine's Reference Viewer on this package."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ContentBrowser.ReferenceViewer"),
		FUIAction(FExecuteAction::CreateSP(this, &SPulseMemoryAuditorPanel::OpenReferenceViewer, Item)));

	if (Item->bIsRoot)
	{
		// First entry for a top-level row: it is the step the whole drag-in workflow leads to.
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ExportAssetReport", "Export Asset Report"),
			LOCTEXT("ExportAssetReportTooltip", "Write Summary.md and Dependencies.csv for this asset: its biggest cuts ranked by how much they save it, each with every reference that has to go. Opens the folder when done."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Save"),
			FUIAction(FExecuteAction::CreateSP(this, &SPulseMemoryAuditorPanel::ExportAssetReport, Item)));
		MenuBuilder.AddSeparator();
	}

	const bool bInspected = Item->bIsRoot && Report->Roots[Item->RootIndex].bInspected;
	if (bInspected)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("StopInspecting", "Stop Inspecting"),
			LOCTEXT("StopInspectingTooltip", "Remove this asset from the inspected set and re-run the analysis."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.X"),
			FUIAction(FExecuteAction::CreateSP(this, &SPulseMemoryAuditorPanel::StopInspectingItem, Item)));
	}
	else
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("Inspect", "Inspect"),
			LOCTEXT("InspectTooltip", "Pin this asset at the top as its own entry, with everything it loads."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
			FUIAction(FExecuteAction::CreateSP(this, &SPulseMemoryAuditorPanel::InspectItem, Item)));
	}

	if (!Item->bIsRoot)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("CopyChain", "Copy Reference Chain"),
			LOCTEXT("CopyChainTooltip", "Copy the shortest chain of references that loads this asset, and every other package referencing it directly."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
			FUIAction(FExecuteAction::CreateSP(this, &SPulseMemoryAuditorPanel::CopyReferenceChain, Item)));
	}

	return MenuBuilder.MakeWidget();
}

void SPulseMemoryAuditorPanel::InspectItem(TSharedPtr<FPulseMemoryTreeItem> Item)
{
	if (Item.IsValid() && !Item->bIsPlaceholder && Report.IsValid())
	{
		InspectPackages({ Report->Nodes[Item->NodeIndex].PackageName });
	}
}

void SPulseMemoryAuditorPanel::ExportAssetReport(TSharedPtr<FPulseMemoryTreeItem> Item)
{
	if (!Item.IsValid() || !Item->bIsRoot || !Report.IsValid())
	{
		return;
	}

	const UPulseMemoryAuditorSettings& Settings = *GetDefault<UPulseMemoryAuditorSettings>();
	FPulseMemoryAuditorConfig Config;
	Config.ApplySettingsDefaults(Settings);

	FPulseMemoryReportWriter Writer(Config.ReportDir, Config.MaxDependenciesPerRootInReport, Config.MaxHistoricalRuns);
	FString Error;
	if (!Writer.WriteAssetReport(*Report, Item->RootIndex, Error))
	{
		StatusLine = FString::Printf(TEXT("Asset report failed: %s"), *Error);
		UE_LOG(LogPulse, Error, TEXT("%s"), *Error);
		return;
	}

	LastExportDir = Writer.GetAssetReportDir();
	StatusLine = FString::Printf(TEXT("Asset report written to %s"), *LastExportDir);
	UE_LOG(LogPulse, Display, TEXT("Memory asset report written to %s"), *LastExportDir);
	FPlatformProcess::ExploreFolder(*LastExportDir);
}

void SPulseMemoryAuditorPanel::StopInspectingItem(TSharedPtr<FPulseMemoryTreeItem> Item)
{
	if (!Item.IsValid() || !Report.IsValid())
	{
		return;
	}

	InspectedPackages.Remove(Report->Nodes[Item->NodeIndex].PackageName);
	RunAnalysis();
}

void SPulseMemoryAuditorPanel::BrowseToItem(const TSharedPtr<FPulseMemoryTreeItem> Item) const
{
	if (!Item.IsValid() || Item->bIsPlaceholder || !Report.IsValid())
	{
		return;
	}

	const FPulseMemoryNode& Node = Report->Nodes[Item->NodeIndex];
	if (Node.ObjectPath.IsEmpty())
	{
		return;
	}

	// From the registry, never TryLoad: browsing to a map must not load it.
	const IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(Node.ObjectPath));
	if (!AssetData.IsValid())
	{
		return;
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowserModule.Get().SyncBrowserToAssets(TArray<FAssetData>{ AssetData });
}

void SPulseMemoryAuditorPanel::OpenReferenceViewer(const TSharedPtr<FPulseMemoryTreeItem> Item) const
{
	if (!Item.IsValid() || Item->bIsPlaceholder || !Report.IsValid())
	{
		return;
	}

	const TArray<FAssetIdentifier> Identifiers{ FAssetIdentifier(Report->Nodes[Item->NodeIndex].PackageName) };
	FEditorDelegates::OnOpenReferenceViewer.Broadcast(Identifiers, FReferenceViewerParams());
}

void SPulseMemoryAuditorPanel::CopyReferenceChain(const TSharedPtr<FPulseMemoryTreeItem> Item) const
{
	if (!Item.IsValid() || Item->bIsPlaceholder || !Report.IsValid())
	{
		return;
	}

	FString Chain = FString::Printf(TEXT("%s > %s"), *Item->Via, *Report->Nodes[Item->NodeIndex].AssetName);
	if (Item->NumReferencers > 1)
	{
		Chain += FString::Printf(TEXT("\nAlso referenced directly by: %s"), *Item->ReferencedBy);
	}
	FPlatformApplicationMisc::ClipboardCopy(*Chain);
}

#undef LOCTEXT_NAMESPACE
