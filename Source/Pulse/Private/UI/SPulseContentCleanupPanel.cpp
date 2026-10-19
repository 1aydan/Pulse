// Copyright (c) 2026 Pulse contributors. MIT License.

#include "UI/SPulseContentCleanupPanel.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "ContentCleanup/PulseCleanupAnalyzer.h"
#include "ContentCleanup/PulseCleanupReportWriter.h"
#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IContentBrowserSingleton.h"
#include "ISettingsModule.h"
#include "MemoryAuditor/PulseMemoryEstimator.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "Pulse.h"
#include "PulseContentCleanupSettings.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "PulseContentCleanupPanel"

static const FName GPulseCleanColAsset(TEXT("Asset"));
static const FName GPulseCleanColClass(TEXT("Class"));
static const FName GPulseCleanColStatus(TEXT("Status"));
static const FName GPulseCleanColDisk(TEXT("Disk"));
static const FName GPulseCleanColMemory(TEXT("Memory"));
static const FName GPulseCleanColReferencedBy(TEXT("ReferencedBy"));
static const FName GPulseCleanColNamedIn(TEXT("NamedIn"));
static const FName GPulseCleanColCopies(TEXT("Copies"));
static const FName GPulseCleanColWasted(TEXT("Wasted"));
static const FName GPulseCleanColUsed(TEXT("Used"));

static FText PulseCleanBytes(int64 Bytes)
{
	return Bytes == 0 ? FText::FromString(TEXT("–")) : FText::FromString(PulseFormatBytes(Bytes));
}

static FText PulseStatusTooltip(EPulseCleanupStatus Status)
{
	switch (Status)
	{
	case EPulseCleanupStatus::Orphan:
		return LOCTEXT("OrphanTooltip", "Nothing references this asset at all.");
	case EPulseCleanupStatus::Unreachable:
		return LOCTEXT("UnreachableTooltip", "Referenced, but only by other unused content or by editor-only references, so nothing that ships loads it.");
	case EPulseCleanupStatus::Redirector:
		return LOCTEXT("RedirectorTooltip", "A redirector left by a move or rename. Fix up redirectors in the Content Browser, then delete it.");
	}
	return FText::GetEmpty();
}

// ---- Rows ---------------------------------------------------------------------------------------

class SPulseUnusedRow : public SMultiColumnTableRow<TSharedPtr<FPulseUnusedItem>>
{
public:
	SLATE_BEGIN_ARGS(SPulseUnusedRow)
	{
	}
	SLATE_ARGUMENT(TSharedPtr<FPulseUnusedItem>, Item)
	SLATE_ARGUMENT(TSharedPtr<const FPulseCleanupReport>, Report)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Item = InArgs._Item;
		Report = InArgs._Report;
		SMultiColumnTableRow<TSharedPtr<FPulseUnusedItem>>::Construct(FSuperRowType::FArguments(), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid() || !Report.IsValid())
		{
			return SNullWidget::NullWidget;
		}
		const FPulseCleanupAsset& Asset = Report->Unused[Item->Index];

		// Named in config or source: possibly live, so it must not look like the rest.
		const FSlateColor Color = Asset.IsMentioned() ? FSlateColor(FStyleColors::Warning) : FSlateColor::UseForeground();

		if (ColumnName == GPulseCleanColAsset)
		{
			return SNew(STextBlock).Text(FText::FromString(Asset.AssetName)).ToolTipText(FText::FromString(Asset.ObjectPath)).ColorAndOpacity(Color);
		}
		if (ColumnName == GPulseCleanColClass)
		{
			return SNew(STextBlock).Text(FText::FromString(Asset.ClassName));
		}
		if (ColumnName == GPulseCleanColStatus)
		{
			return SNew(STextBlock).Text(FText::FromString(PulseCleanupStatusToString(Asset.Status))).ToolTipText(PulseStatusTooltip(Asset.Status));
		}
		if (ColumnName == GPulseCleanColDisk)
		{
			return SNew(STextBlock).Text(PulseCleanBytes(Asset.DiskBytes));
		}
		if (ColumnName == GPulseCleanColMemory)
		{
			return SNew(STextBlock)
				.Text(PulseCleanBytes(Asset.Memory.GetResidentBytes()))
				.ToolTipText(FText::FromString(FString::Printf(TEXT("RAM %s, VRAM %s, streaming %s (estimated)"),
					*PulseFormatBytes(Asset.Memory.RAMBytes), *PulseFormatBytes(Asset.Memory.VRAMBytes), *PulseFormatBytes(Asset.Memory.StreamingBytes))));
		}
		if (ColumnName == GPulseCleanColReferencedBy)
		{
			const FString Text = Asset.NumReferencers == 0 ? FString(TEXT("–")) : FString::Printf(TEXT("%d: %s"), Asset.NumReferencers, *Asset.ReferencedBy);
			return SNew(STextBlock).Text(FText::FromString(Text)).ToolTipText(FText::FromString(Asset.ReferencedBy));
		}
		if (ColumnName == GPulseCleanColNamedIn)
		{
			return SNew(STextBlock)
				.Text(FText::FromString(Asset.MentionedIn))
				.ToolTipText(Asset.IsMentioned()
					? FText::Format(LOCTEXT("MentionedTooltip", "This package's path appears in {0}. The Asset Registry cannot see config values or C++ paths, so it may be loaded that way. Check before deleting."), FText::FromString(Asset.MentionedIn))
					: FText::GetEmpty())
				.ColorAndOpacity(Color);
		}
		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FPulseUnusedItem> Item;
	TSharedPtr<const FPulseCleanupReport> Report;
};

class SPulseDuplicateRow : public SMultiColumnTableRow<TSharedPtr<FPulseDuplicateItem>>
{
public:
	SLATE_BEGIN_ARGS(SPulseDuplicateRow)
	{
	}
	SLATE_ARGUMENT(TSharedPtr<FPulseDuplicateItem>, Item)
	SLATE_ARGUMENT(TSharedPtr<const FPulseCleanupReport>, Report)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Item = InArgs._Item;
		Report = InArgs._Report;
		SMultiColumnTableRow<TSharedPtr<FPulseDuplicateItem>>::Construct(FSuperRowType::FArguments(), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid() || !Report.IsValid())
		{
			return SNullWidget::NullWidget;
		}
		const FPulseDuplicateGroup& Group = Report->Duplicates[Item->GroupIndex];
		const bool bGroup = Item->MemberIndex == INDEX_NONE;
		const FPulseDuplicateMember* Member = bGroup ? nullptr : &Group.Members[Item->MemberIndex];

		if (ColumnName == GPulseCleanColAsset)
		{
			const FText Label = bGroup ? FText::FromString(Group.SourceFile.IsEmpty() ? Group.SourceHash : Group.SourceFile) : FText::FromString(Member->AssetName);
			const FText Tooltip = bGroup
				? FText::Format(LOCTEXT("GroupTooltip", "Imported from the same source file content (MD5 {0})."), FText::FromString(Group.SourceHash))
				: FText::FromString(Member->ObjectPath);
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
					.Text(Label)
					.ToolTipText(Tooltip)
					.Font(bGroup ? FCoreStyle::GetDefaultFontStyle("Bold", 9) : FCoreStyle::GetDefaultFontStyle("Regular", 9))
				];
		}
		if (ColumnName == GPulseCleanColClass)
		{
			return SNew(STextBlock).Text(FText::FromString(Group.ClassName));
		}
		if (ColumnName == GPulseCleanColCopies)
		{
			return bGroup ? StaticCastSharedRef<SWidget>(SNew(STextBlock).Text(FText::AsNumber(Group.Members.Num()))) : SNullWidget::NullWidget;
		}
		if (ColumnName == GPulseCleanColDisk)
		{
			return SNew(STextBlock).Text(PulseCleanBytes(bGroup ? Group.WastedDiskBytes : Member->DiskBytes))
				.ToolTipText(bGroup ? LOCTEXT("WastedDiskTooltip", "Disk taken by every copy but the largest.") : FText::GetEmpty());
		}
		if (ColumnName == GPulseCleanColWasted)
		{
			return SNew(STextBlock).Text(PulseCleanBytes(bGroup ? Group.WastedMemory.GetResidentBytes() : Member->Memory.GetResidentBytes()))
				.ToolTipText(bGroup
					? LOCTEXT("WastedMemoryTooltip", "Memory paid more than once: every copy that ships, beyond the first. Zero when only one copy is used.")
					: LOCTEXT("MemberMemoryTooltip", "Estimated resident memory of this copy when loaded."));
		}
		if (ColumnName == GPulseCleanColUsed)
		{
			if (bGroup)
			{
				int32 NumUsed = 0;
				for (const FPulseDuplicateMember& Copy : Group.Members)
				{
					NumUsed += Copy.bUsed ? 1 : 0;
				}
				return SNew(STextBlock).Text(FText::Format(LOCTEXT("UsedCount", "{0} of {1}"), FText::AsNumber(NumUsed), FText::AsNumber(Group.Members.Num())));
			}
			return SNew(STextBlock)
				.Text(Member->bUsed ? LOCTEXT("Yes", "Yes") : LOCTEXT("No", "No"))
				.ToolTipText(FText::Format(LOCTEXT("MemberRefs", "{0} referencer(s)."), FText::AsNumber(Member->NumReferencers)));
		}
		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FPulseDuplicateItem> Item;
	TSharedPtr<const FPulseCleanupReport> Report;
};

// ---- Panel --------------------------------------------------------------------------------------

void SPulseContentCleanupPanel::Construct(const FArguments& InArgs)
{
	StatusLine = TEXT("Press Analyze to find content nothing that ships can load, and imports that exist more than once.");

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			BuildControlsRow()
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 0.0f)
		[
			SNew(STextBlock).Text(this, &SPulseContentCleanupPanel::GetStatusText)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			SNew(STextBlock).AutoWrapText(true).Text(this, &SPulseContentCleanupPanel::GetSummaryText)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			SNew(SSeparator)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 0.0f, 4.0f, 4.0f).HAlign(HAlign_Left)
		[
			SNew(SSegmentedControl<EView>)
			.Value(this, &SPulseContentCleanupPanel::GetView)
			.OnValueChanged(this, &SPulseContentCleanupPanel::SetView)
			+ SSegmentedControl<EView>::Slot(EView::Unused)
			.Text(this, &SPulseContentCleanupPanel::GetViewLabel, EView::Unused)
			+ SSegmentedControl<EView>::Slot(EView::Duplicates)
			.Text(this, &SPulseContentCleanupPanel::GetViewLabel, EView::Duplicates)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 0.0f)
		[
			BuildUnusedFilterRow()
		]

		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex(this, &SPulseContentCleanupPanel::GetViewIndex)
			+ SWidgetSwitcher::Slot()
			[
				BuildUnusedList()
			]
			+ SWidgetSwitcher::Slot()
			[
				BuildDuplicatesTree()
			]
		]
	];
}

TSharedRef<SWidget> SPulseContentCleanupPanel::BuildControlsRow()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Analyze", "Analyze"))
			.ToolTipText(LOCTEXT("AnalyzeTooltip", "Walk everything the cooker would start from — Maps to Cook (or every map), primary assets, always-loaded assets, always-cook directories — and list what under the scan paths it never reaches. Nothing is loaded or deleted."))
			.OnClicked(this, &SPulseContentCleanupPanel::OnRunClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Export", "Export report"))
			.ToolTipText(LOCTEXT("ExportTooltip", "Write Unused.csv, Duplicates.csv and Run.json, identical to the commandlet's output."))
			.IsEnabled(this, &SPulseContentCleanupPanel::HasReport)
			.OnClicked(this, &SPulseContentCleanupPanel::OnExportClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Clear", "Clear"))
			.IsEnabled(this, &SPulseContentCleanupPanel::HasReport)
			.OnClicked(this, &SPulseContentCleanupPanel::OnClearClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Settings", "Settings"))
			.OnClicked(this, &SPulseContentCleanupPanel::OnOpenSettingsClicked)
		];
}

TSharedRef<SWidget> SPulseContentCleanupPanel::BuildUnusedFilterRow()
{
	auto MakeStatusToggle = [this](EPulseCleanupStatus Status, const FText& Label)
	{
		return SNew(SCheckBox)
			.IsChecked(this, &SPulseContentCleanupPanel::GetStatusFilterState, Status)
			.OnCheckStateChanged(this, &SPulseContentCleanupPanel::OnStatusFilterChanged, Status)
			.ToolTipText(PulseStatusTooltip(Status))
			[
				SNew(STextBlock).Text(Label)
			];
	};

	return SNew(SHorizontalBox)
		.Visibility(this, &SPulseContentCleanupPanel::GetUnusedFilterVisibility)
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f)
		[
			SNew(SSearchBox)
			.HintText(LOCTEXT("SearchHint", "Filter by name, path or class"))
			.OnTextChanged(this, &SPulseContentCleanupPanel::OnSearchTextChanged)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 2.0f).VAlign(VAlign_Center)
		[
			MakeStatusToggle(EPulseCleanupStatus::Orphan, LOCTEXT("Orphans", "Orphans"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 2.0f).VAlign(VAlign_Center)
		[
			MakeStatusToggle(EPulseCleanupStatus::Unreachable, LOCTEXT("Unreachable", "Unreachable"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 2.0f).VAlign(VAlign_Center)
		[
			MakeStatusToggle(EPulseCleanupStatus::Redirector, LOCTEXT("Redirectors", "Redirectors"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(12.0f, 2.0f, 2.0f, 2.0f).VAlign(VAlign_Center)
		[
			SNew(SCheckBox)
			.IsChecked(this, &SPulseContentCleanupPanel::GetHideMentionedState)
			.OnCheckStateChanged(this, &SPulseContentCleanupPanel::OnHideMentionedChanged)
			.ToolTipText(LOCTEXT("HideMentionedTooltip", "Hide assets whose path appears in a project .ini or C++ file. Those may be loaded in ways the Asset Registry cannot see."))
			[
				SNew(STextBlock).Text(LOCTEXT("HideMentioned", "Hide named in config/code"))
			]
		];
}

TSharedRef<SWidget> SPulseContentCleanupPanel::BuildUnusedList()
{
	return SAssignNew(UnusedList, SListView<TSharedPtr<FPulseUnusedItem>>)
		.ListItemsSource(&VisibleUnusedItems)
		.SelectionMode(ESelectionMode::Multi)
		.OnGenerateRow(this, &SPulseContentCleanupPanel::OnGenerateUnusedRow)
		.OnMouseButtonDoubleClick(this, &SPulseContentCleanupPanel::OnUnusedDoubleClicked)
		.OnContextMenuOpening(this, &SPulseContentCleanupPanel::OnUnusedContextMenu)
		.HeaderRow(
			SNew(SHeaderRow)
			+ SHeaderRow::Column(GPulseCleanColAsset).DefaultLabel(LOCTEXT("ColAsset", "Asset")).FillWidth(0.22f)
			+ SHeaderRow::Column(GPulseCleanColClass).DefaultLabel(LOCTEXT("ColClass", "Class")).FillWidth(0.12f)
			+ SHeaderRow::Column(GPulseCleanColStatus).DefaultLabel(LOCTEXT("ColStatus", "Status")).FillWidth(0.08f)
			+ SHeaderRow::Column(GPulseCleanColDisk).DefaultLabel(LOCTEXT("ColDisk", "Disk")).FillWidth(0.07f)
				.DefaultTooltip(LOCTEXT("ColDiskTooltip", "Editor package size: what deleting it removes from the project."))
			+ SHeaderRow::Column(GPulseCleanColMemory).DefaultLabel(LOCTEXT("ColMemory", "Memory")).FillWidth(0.07f)
				.DefaultTooltip(LOCTEXT("ColMemoryTooltip", "Estimated RAM + VRAM if it were loaded. Not paid today, since nothing loads it."))
			+ SHeaderRow::Column(GPulseCleanColReferencedBy).DefaultLabel(LOCTEXT("ColReferencedBy", "Referenced by")).FillWidth(0.24f)
			+ SHeaderRow::Column(GPulseCleanColNamedIn).DefaultLabel(LOCTEXT("ColNamedIn", "Named in config/code")).FillWidth(0.20f)
		);
}

TSharedRef<SWidget> SPulseContentCleanupPanel::BuildDuplicatesTree()
{
	return SAssignNew(DuplicateTree, STreeView<TSharedPtr<FPulseDuplicateItem>>)
		.TreeItemsSource(&DuplicateItems)
		.SelectionMode(ESelectionMode::Multi)
		.OnGenerateRow(this, &SPulseContentCleanupPanel::OnGenerateDuplicateRow)
		.OnGetChildren(this, &SPulseContentCleanupPanel::OnGetDuplicateChildren)
		.OnMouseButtonDoubleClick(this, &SPulseContentCleanupPanel::OnDuplicateDoubleClicked)
		.OnContextMenuOpening(this, &SPulseContentCleanupPanel::OnDuplicateContextMenu)
		.HeaderRow(
			SNew(SHeaderRow)
			+ SHeaderRow::Column(GPulseCleanColAsset).DefaultLabel(LOCTEXT("ColSource", "Source file / copy")).FillWidth(0.36f)
			+ SHeaderRow::Column(GPulseCleanColClass).DefaultLabel(LOCTEXT("ColClass", "Class")).FillWidth(0.14f)
			+ SHeaderRow::Column(GPulseCleanColCopies).DefaultLabel(LOCTEXT("ColCopies", "Copies")).FillWidth(0.08f)
			+ SHeaderRow::Column(GPulseCleanColUsed).DefaultLabel(LOCTEXT("ColUsed", "Used")).FillWidth(0.10f)
				.DefaultTooltip(LOCTEXT("ColUsedTooltip", "Whether something that ships loads the copy."))
			+ SHeaderRow::Column(GPulseCleanColDisk).DefaultLabel(LOCTEXT("ColWastedDisk", "Disk")).FillWidth(0.14f)
				.DefaultTooltip(LOCTEXT("ColWastedDiskTooltip", "Group: disk taken by the extra copies. Copy: its own size."))
			+ SHeaderRow::Column(GPulseCleanColWasted).DefaultLabel(LOCTEXT("ColWastedMemory", "Memory")).FillWidth(0.14f)
				.DefaultTooltip(LOCTEXT("ColWastedMemoryTooltip", "Group: memory paid more than once because several copies ship. Copy: its estimated resident cost."))
		);
}

FText SPulseContentCleanupPanel::GetStatusText() const
{
	return FText::FromString(StatusLine);
}

FText SPulseContentCleanupPanel::GetSummaryText() const
{
	if (!Report.IsValid())
	{
		return FText::GetEmpty();
	}

	const FText Roots = Report->bUsedMapsToCook
		? FText::Format(LOCTEXT("RootsMapsToCook", "the {0} maps in Maps to Cook"), FText::AsNumber(Report->NumMapsToCook))
		: LOCTEXT("RootsAllMaps", "every map");

	int32 NumMentioned = 0;
	for (const FPulseCleanupAsset& Asset : Report->Unused)
	{
		NumMentioned += Asset.IsMentioned() ? 1 : 0;
	}

	return FText::Format(
		LOCTEXT("Summary", "Content is used when {0}, primary assets, always-loaded assets or always-cook directories reach it. {1} of {2} assets are unused or redirectors, {3} on disk; {4} of them are named in config or C++ (amber) and need checking by hand. {5} duplicate import groups waste {6} on disk."),
		Roots,
		FText::AsNumber(Report->Unused.Num()),
		FText::AsNumber(Report->NumAssetsScanned),
		FText::FromString(PulseFormatBytes(Report->GetUnusedDiskBytes(/*bIncludeMentioned*/ true))),
		FText::AsNumber(NumMentioned),
		FText::AsNumber(Report->Duplicates.Num()),
		FText::FromString(PulseFormatBytes(Report->GetDuplicateWastedDiskBytes())));
}

FText SPulseContentCleanupPanel::GetViewLabel(EView InView) const
{
	if (InView == EView::Unused)
	{
		return Report.IsValid()
			? FText::Format(LOCTEXT("UnusedLabelCount", "Unused ({0})"), FText::AsNumber(VisibleUnusedItems.Num()))
			: LOCTEXT("UnusedLabel", "Unused");
	}
	return Report.IsValid()
		? FText::Format(LOCTEXT("DuplicatesLabelCount", "Duplicates ({0})"), FText::AsNumber(DuplicateItems.Num()))
		: LOCTEXT("DuplicatesLabel", "Duplicates");
}

EVisibility SPulseContentCleanupPanel::GetUnusedFilterVisibility() const
{
	return View == EView::Unused ? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SPulseContentCleanupPanel::OnRunClicked()
{
	FPulseCleanupConfig Config;
	Config.ApplySettingsDefaults(*GetDefault<UPulseContentCleanupSettings>());

	TSharedPtr<FPulseCleanupReport> NewReport = MakeShared<FPulseCleanupReport>();
	FString Error;
	bool bSucceeded = false;
	{
		FScopedSlowTask SlowTask(1.0f, LOCTEXT("Analyzing", "Looking for unused content..."));
		SlowTask.MakeDialog(/*bShowCancelButton*/ true);
		float ReportedFraction = 0.0f;
		FPulseCleanupAnalyzer Analyzer(Config);
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
		UE_LOG(LogPulse, Warning, TEXT("Pulse.Cleanup: %s"), *Error);
		return FReply::Handled();
	}

	Report = NewReport;
	RebuildItems();
	StatusLine = TEXT("Done. Select rows and right-click to browse to them; deleting stays in the Content Browser, where the engine checks references again.");
	return FReply::Handled();
}

FReply SPulseContentCleanupPanel::OnExportClicked()
{
	if (!Report.IsValid())
	{
		return FReply::Handled();
	}

	FPulseCleanupConfig Config;
	Config.ApplySettingsDefaults(*GetDefault<UPulseContentCleanupSettings>());
	FPulseCleanupReportWriter Writer(Config.ReportDir, Config.MaxHistoricalRuns);
	FString Error;
	if (!Writer.Write(*Report, Error))
	{
		StatusLine = FString::Printf(TEXT("Export failed: %s"), *Error);
		UE_LOG(LogPulse, Error, TEXT("%s"), *Error);
		return FReply::Handled();
	}
	StatusLine = FString::Printf(TEXT("Exported to %s"), *Writer.GetRunDir());
	UE_LOG(LogPulse, Display, TEXT("Content cleanup report written to %s"), *Writer.GetRunDir());
	return FReply::Handled();
}

FReply SPulseContentCleanupPanel::OnClearClicked()
{
	Report.Reset();
	RebuildItems();
	StatusLine = TEXT("Press Analyze to find content nothing that ships can load, and imports that exist more than once.");
	return FReply::Handled();
}

FReply SPulseContentCleanupPanel::OnOpenSettingsClicked()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
	{
		SettingsModule->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("PulseContentCleanupSettings"));
	}
	return FReply::Handled();
}

void SPulseContentCleanupPanel::OnSearchTextChanged(const FText& InText)
{
	SearchText = InText.ToString();
	ApplyUnusedFilters();
}

ECheckBoxState SPulseContentCleanupPanel::GetStatusFilterState(EPulseCleanupStatus Status) const
{
	return HiddenStatuses.Contains(Status) ? ECheckBoxState::Unchecked : ECheckBoxState::Checked;
}

void SPulseContentCleanupPanel::OnStatusFilterChanged(ECheckBoxState NewState, EPulseCleanupStatus Status)
{
	if (NewState == ECheckBoxState::Checked)
	{
		HiddenStatuses.Remove(Status);
	}
	else
	{
		HiddenStatuses.Add(Status);
	}
	ApplyUnusedFilters();
}

ECheckBoxState SPulseContentCleanupPanel::GetHideMentionedState() const
{
	return bHideMentioned ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SPulseContentCleanupPanel::OnHideMentionedChanged(ECheckBoxState NewState)
{
	bHideMentioned = NewState == ECheckBoxState::Checked;
	ApplyUnusedFilters();
}

void SPulseContentCleanupPanel::RebuildItems()
{
	AllUnusedItems.Reset();
	DuplicateItems.Reset();

	if (Report.IsValid())
	{
		// The report is already ordered largest first; the list keeps that order.
		for (int32 Index = 0; Index < Report->Unused.Num(); ++Index)
		{
			TSharedPtr<FPulseUnusedItem> Item = MakeShared<FPulseUnusedItem>();
			Item->Index = Index;
			AllUnusedItems.Add(MoveTemp(Item));
		}

		for (int32 GroupIndex = 0; GroupIndex < Report->Duplicates.Num(); ++GroupIndex)
		{
			TSharedPtr<FPulseDuplicateItem> Group = MakeShared<FPulseDuplicateItem>();
			Group->GroupIndex = GroupIndex;
			for (int32 MemberIndex = 0; MemberIndex < Report->Duplicates[GroupIndex].Members.Num(); ++MemberIndex)
			{
				TSharedPtr<FPulseDuplicateItem> Member = MakeShared<FPulseDuplicateItem>();
				Member->GroupIndex = GroupIndex;
				Member->MemberIndex = MemberIndex;
				Group->Children.Add(MoveTemp(Member));
			}
			DuplicateItems.Add(MoveTemp(Group));
		}
	}

	ApplyUnusedFilters();
	if (DuplicateTree.IsValid())
	{
		DuplicateTree->RequestTreeRefresh();
	}
}

void SPulseContentCleanupPanel::ApplyUnusedFilters()
{
	VisibleUnusedItems.Reset();
	if (Report.IsValid())
	{
		for (const TSharedPtr<FPulseUnusedItem>& Item : AllUnusedItems)
		{
			const FPulseCleanupAsset& Asset = Report->Unused[Item->Index];
			if (HiddenStatuses.Contains(Asset.Status) || (bHideMentioned && Asset.IsMentioned()))
			{
				continue;
			}
			if (!SearchText.IsEmpty() && !Asset.ObjectPath.Contains(SearchText) && !Asset.ClassName.Contains(SearchText))
			{
				continue;
			}
			VisibleUnusedItems.Add(Item);
		}
	}
	if (UnusedList.IsValid())
	{
		UnusedList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SPulseContentCleanupPanel::OnGenerateUnusedRow(TSharedPtr<FPulseUnusedItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SPulseUnusedRow, OwnerTable).Item(Item).Report(Report);
}

TSharedRef<ITableRow> SPulseContentCleanupPanel::OnGenerateDuplicateRow(TSharedPtr<FPulseDuplicateItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SPulseDuplicateRow, OwnerTable).Item(Item).Report(Report);
}

void SPulseContentCleanupPanel::OnGetDuplicateChildren(TSharedPtr<FPulseDuplicateItem> Item, TArray<TSharedPtr<FPulseDuplicateItem>>& OutChildren)
{
	if (Item.IsValid())
	{
		OutChildren = Item->Children;
	}
}

void SPulseContentCleanupPanel::OnUnusedDoubleClicked(TSharedPtr<FPulseUnusedItem> Item)
{
	BrowseToSelected();
}

void SPulseContentCleanupPanel::OnDuplicateDoubleClicked(TSharedPtr<FPulseDuplicateItem> Item)
{
	if (Item.IsValid() && Item->MemberIndex == INDEX_NONE && DuplicateTree.IsValid())
	{
		DuplicateTree->SetItemExpansion(Item, !DuplicateTree->IsItemExpanded(Item));
		return;
	}
	BrowseToSelected();
}

TArray<FString> SPulseContentCleanupPanel::GetSelectedObjectPaths() const
{
	TArray<FString> Paths;
	if (!Report.IsValid())
	{
		return Paths;
	}

	if (View == EView::Unused && UnusedList.IsValid())
	{
		for (const TSharedPtr<FPulseUnusedItem>& Item : UnusedList->GetSelectedItems())
		{
			Paths.AddUnique(Report->Unused[Item->Index].ObjectPath);
		}
	}
	else if (View == EView::Duplicates && DuplicateTree.IsValid())
	{
		for (const TSharedPtr<FPulseDuplicateItem>& Item : DuplicateTree->GetSelectedItems())
		{
			const FPulseDuplicateGroup& Group = Report->Duplicates[Item->GroupIndex];
			if (Item->MemberIndex == INDEX_NONE)
			{
				for (const FPulseDuplicateMember& Member : Group.Members)
				{
					Paths.AddUnique(Member.ObjectPath);
				}
			}
			else
			{
				Paths.AddUnique(Group.Members[Item->MemberIndex].ObjectPath);
			}
		}
	}
	return Paths;
}

TArray<FName> SPulseContentCleanupPanel::GetSelectedPackageNames() const
{
	TArray<FName> Packages;
	for (const FString& Path : GetSelectedObjectPaths())
	{
		Packages.AddUnique(FSoftObjectPath(Path).GetLongPackageFName());
	}
	return Packages;
}

TSharedPtr<SWidget> SPulseContentCleanupPanel::OnUnusedContextMenu()
{
	const int32 NumSelected = GetSelectedObjectPaths().Num();
	if (NumSelected == 0)
	{
		return nullptr;
	}

	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	MenuBuilder.AddMenuEntry(
		NumSelected == 1 ? LOCTEXT("BrowseOne", "Browse to Asset") : FText::Format(LOCTEXT("BrowseMany", "Browse to {0} Assets"), FText::AsNumber(NumSelected)),
		LOCTEXT("BrowseTooltip", "Select them in the Content Browser, where they can be reviewed and deleted with the engine's own reference checks."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SystemWideCommands.FindInContentBrowser"),
		FUIAction(FExecuteAction::CreateSP(this, &SPulseContentCleanupPanel::BrowseToSelected)));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("ReferenceViewer", "Reference Viewer"),
		LOCTEXT("ReferenceViewerTooltip", "Open the engine's Reference Viewer on the selection."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ContentBrowser.ReferenceViewer"),
		FUIAction(FExecuteAction::CreateSP(this, &SPulseContentCleanupPanel::OpenReferenceViewerForSelected)));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("CopyPaths", "Copy Paths"),
		LOCTEXT("CopyPathsTooltip", "Copy the selected assets' object paths, one per line."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateSP(this, &SPulseContentCleanupPanel::CopySelectedPaths)));
	return MenuBuilder.MakeWidget();
}

TSharedPtr<SWidget> SPulseContentCleanupPanel::OnDuplicateContextMenu()
{
	return OnUnusedContextMenu();
}

void SPulseContentCleanupPanel::BrowseToSelected() const
{
	const IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	for (const FString& Path : GetSelectedObjectPaths())
	{
		// From the registry, never TryLoad: browsing to unused content must not load it.
		const FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
		if (AssetData.IsValid())
		{
			Assets.Add(AssetData);
		}
	}
	if (Assets.Num() > 0)
	{
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(Assets);
	}
}

void SPulseContentCleanupPanel::OpenReferenceViewerForSelected() const
{
	TArray<FAssetIdentifier> Identifiers;
	for (const FName& Package : GetSelectedPackageNames())
	{
		Identifiers.Add(FAssetIdentifier(Package));
	}
	if (Identifiers.Num() > 0)
	{
		FEditorDelegates::OnOpenReferenceViewer.Broadcast(Identifiers, FReferenceViewerParams());
	}
}

void SPulseContentCleanupPanel::CopySelectedPaths() const
{
	FPlatformApplicationMisc::ClipboardCopy(*FString::Join(GetSelectedObjectPaths(), LINE_TERMINATOR));
}

#undef LOCTEXT_NAMESPACE
