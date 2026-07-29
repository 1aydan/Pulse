// Copyright (c) 2026 Pulse contributors. MIT License.

#include "SPulseAuditPanel.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "Pulse.h"
#include "PulseReportWriter.h"
#include "PulseRunConfig.h"
#include "PulseScanDriver.h"
#include "PulseSettings.h"
#include "SPulseIssueRow.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PulseAuditPanel"

// Panel-local UI state lives in the per-project editor ini, matching how the sibling plugin's panel
// remembers its own inputs. It is preference, not project configuration — project thresholds belong
// in DefaultPulse.ini via UPulseSettings.
static const TCHAR* GPulsePanelConfigSection = TEXT("PulseAuditPanel");

/** Wall-clock budget per ticker step. One frame at 60 Hz, so the editor stays interactive. */
static const double GPulsePanelStepBudgetSeconds = 0.016;

/** Tiers in ascending cost order, used when restoring the persisted selection. */
static const TArray<EPulseTier>& PulsePanelTierOptions()
{
	static const TArray<EPulseTier> Options = { EPulseTier::Fast, EPulseTier::Deep, EPulseTier::ShaderStats };
	return Options;
}

static FText PulsePanelTierTooltip(EPulseTier Tier)
{
	switch (Tier)
	{
		case EPulseTier::Fast:
			return LOCTEXT("TierFastTooltip", "Asset Registry metadata only — never loads a package. Completes in seconds even on a large project.");
		case EPulseTier::Deep:
			return LOCTEXT("TierDeepTooltip", "Loads packages for metrics the registry cannot provide: real texture memory, Blueprint tick defaults, collision detail, Niagara emitters, and legacy-map actor counts. Minutes on a large project.");
		case EPulseTier::ShaderStats:
			return LOCTEXT("TierShaderStatsTooltip", "Deep, plus material shader compilation for instruction counts. Very slow on a cold shader cache — consider narrowing to the Material category first.");
	}
	return FText::GetEmpty();
}

void SPulseAuditPanel::Construct(const FArguments& InArgs)
{
	LoadPanelState();

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
		[
			BuildControlsRow()
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f)
		[
			SNew(SProgressBar)
			.Percent(this, &SPulseAuditPanel::GetProgressFraction)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 6.0f)
		[
			SAssignNew(StatusText, STextBlock)
			.Text(LOCTEXT("StatusIdle", "Idle. Choose a tier and press Run."))
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			SNew(SSeparator)
		]

		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			[
				SAssignNew(IssueListView, SListView<TSharedPtr<FPulseIssueListItem>>)
				.ListItemsSource(&IssueItems)
				.OnGenerateRow(this, &SPulseAuditPanel::OnGenerateIssueRow)
				.OnMouseButtonDoubleClick(this, &SPulseAuditPanel::OnIssueDoubleClicked)
				.SelectionMode(ESelectionMode::Single)
				.HeaderRow
				(
					SNew(SHeaderRow)
					+ SHeaderRow::Column(FPulseIssueColumns::Severity)
						.DefaultLabel(LOCTEXT("ColSeverity", "Severity")).FixedWidth(80.0f)
					+ SHeaderRow::Column(FPulseIssueColumns::Category)
						.DefaultLabel(LOCTEXT("ColCategory", "Category")).FixedWidth(110.0f)
					+ SHeaderRow::Column(FPulseIssueColumns::Asset)
						.DefaultLabel(LOCTEXT("ColAsset", "Asset")).FillWidth(0.25f)
					+ SHeaderRow::Column(FPulseIssueColumns::Rule)
						.DefaultLabel(LOCTEXT("ColRule", "Rule")).FillWidth(0.25f)
					+ SHeaderRow::Column(FPulseIssueColumns::Message)
						.DefaultLabel(LOCTEXT("ColMessage", "Detail")).FillWidth(0.5f)
				)
			]
		]
	];

	if (CategoryFilterBox.IsValid() && !PendingCategoryFilter.IsEmpty())
	{
		CategoryFilterBox->SetText(FText::FromString(PendingCategoryFilter));
	}
}

SPulseAuditPanel::~SPulseAuditPanel()
{
	// The ticker outlives the widget unless explicitly removed, and its delegate captures `this`.
	StopTicking();
}

TSharedRef<SWidget> SPulseAuditPanel::BuildControlsRow()
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
		[
			SNew(STextBlock).Text(LOCTEXT("TierLabel", "Tier"))
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 12.0f, 0.0f)
		[
			// SSegmentedControl rather than SComboBox: its list traits reject bare enum option types,
			// and with three tiers the choice is better shown than hidden behind a dropdown anyway.
			SNew(SSegmentedControl<EPulseTier>)
			.Value_Lambda([this]() { return SelectedTier; })
			.OnValueChanged_Lambda([this](EPulseTier NewTier)
			{
				SelectedTier = NewTier;
				SavePanelState();
			})
			+ SSegmentedControl<EPulseTier>::Slot(EPulseTier::Fast)
				.Text(LOCTEXT("TierFast", "Fast"))
				.ToolTip(PulsePanelTierTooltip(EPulseTier::Fast))
			+ SSegmentedControl<EPulseTier>::Slot(EPulseTier::Deep)
				.Text(LOCTEXT("TierDeep", "Deep"))
				.ToolTip(PulsePanelTierTooltip(EPulseTier::Deep))
			+ SSegmentedControl<EPulseTier>::Slot(EPulseTier::ShaderStats)
				.Text(LOCTEXT("TierShaderStats", "Shader Stats"))
				.ToolTip(PulsePanelTierTooltip(EPulseTier::ShaderStats))
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
		[
			SNew(STextBlock).Text(LOCTEXT("MinSeverityLabel", "Min severity"))
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 12.0f, 0.0f)
		[
			SNew(SSegmentedControl<EPulseSeverity>)
			.ToolTipText(LOCTEXT("MinSeverityTooltip", "Filters which issues are listed and exported. Scores are always computed over every issue, so this never changes them."))
			.Value_Lambda([this]() { return SelectedMinSeverity; })
			.OnValueChanged_Lambda([this](EPulseSeverity NewSeverity)
			{
				SelectedMinSeverity = NewSeverity;
				SavePanelState();
				// Purely a display filter, so re-list immediately instead of demanding a rescan.
				RebuildIssueList();
			})
			+ SSegmentedControl<EPulseSeverity>::Slot(EPulseSeverity::Info).Text(LOCTEXT("SevInfo", "Info"))
			+ SSegmentedControl<EPulseSeverity>::Slot(EPulseSeverity::Low).Text(LOCTEXT("SevLow", "Low"))
			+ SSegmentedControl<EPulseSeverity>::Slot(EPulseSeverity::Medium).Text(LOCTEXT("SevMedium", "Medium"))
			+ SSegmentedControl<EPulseSeverity>::Slot(EPulseSeverity::High).Text(LOCTEXT("SevHigh", "High"))
			+ SSegmentedControl<EPulseSeverity>::Slot(EPulseSeverity::Critical).Text(LOCTEXT("SevCritical", "Critical"))
		]

		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 12.0f, 0.0f)
		[
			SAssignNew(CategoryFilterBox, SEditableTextBox)
			.HintText(LOCTEXT("CategoryHint", "Categories (comma separated; empty = all)"))
			.ToolTipText(LOCTEXT("CategoryTooltip", "For example: StaticMesh,Texture. Narrowing to one category is the practical way to run the ShaderStats tier."))
			.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type)
			{
				SavePanelState();
			})
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("RunButton", "Run"))
			.IsEnabled(this, &SPulseAuditPanel::IsRunEnabled)
			.OnClicked(this, &SPulseAuditPanel::OnRunClicked)
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("CancelButton", "Cancel"))
			.IsEnabled(this, &SPulseAuditPanel::IsCancelEnabled)
			.OnClicked(this, &SPulseAuditPanel::OnCancelClicked)
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text(LOCTEXT("ExportButton", "Export report"))
			.ToolTipText(LOCTEXT("ExportTooltip", "Writes the same CSV and JSON files the commandlet produces, into the report directory from Pulse settings."))
			.IsEnabled(this, &SPulseAuditPanel::IsExportEnabled)
			.OnClicked(this, &SPulseAuditPanel::OnExportClicked)
		];
}

bool SPulseAuditPanel::IsRunEnabled() const
{
	return !Driver.IsValid();
}

bool SPulseAuditPanel::IsCancelEnabled() const
{
	return Driver.IsValid();
}

bool SPulseAuditPanel::IsExportEnabled() const
{
	return bHasReport && !Driver.IsValid();
}

FReply SPulseAuditPanel::OnRunClicked()
{
	if (Driver.IsValid())
	{
		return FReply::Handled();
	}

	const UPulseSettings& Settings = *GetDefault<UPulseSettings>();

	FPulseRunConfig Config;
	Config.RunTier = SelectedTier;
	// Keep the request flags consistent with the tier: the report header reports what was asked for,
	// and -shaderstats implying -deep is a property of the tier ordering, not of the command line.
	Config.bDeepRequested = SelectedTier >= EPulseTier::Deep;
	Config.bShaderStatsRequested = SelectedTier == EPulseTier::ShaderStats;
	Config.MinSeverity = SelectedMinSeverity;

	if (CategoryFilterBox.IsValid())
	{
		const FString CategoryText = CategoryFilterBox->GetText().ToString();
		CategoryText.ParseIntoArray(Config.CategoryFilter, TEXT(","), /*bCullEmpty*/ true);
		for (FString& Category : Config.CategoryFilter)
		{
			Category.TrimStartAndEndInline();
		}
		Config.CategoryFilter.RemoveAll([](const FString& Category) { return Category.IsEmpty(); });
	}

	Config.ApplySettingsDefaults(Settings);
	LastReportDir = Config.ReportDir;

	Driver = MakeUnique<FPulseScanDriver>(Config, Settings);

	// Initialize performs a synchronous SearchAllAssets. In the editor the registry is already warm,
	// so this is effectively instant; it is the one part of a scan that is not frame-budgeted.
	FString Error;
	if (!Driver->Initialize(Error))
	{
		Driver.Reset();
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(LOCTEXT("StatusInitFailed", "Scan could not start: {0}"), FText::FromString(Error)));
		}
		UE_LOG(LogPulse, Warning, TEXT("Pulse panel: scan initialization failed: %s"), *Error);
		return FReply::Handled();
	}

	IssueItems.Reset();
	if (IssueListView.IsValid())
	{
		IssueListView->RequestListRefresh();
	}
	bHasReport = false;
	ProgressProcessed = 0;
	ProgressTotal = Driver->GetProgress().AssetsTotal;

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			LOCTEXT("StatusStarted", "Scanning {0} assets at tier {1}..."),
			FText::AsNumber(ProgressTotal),
			FText::FromString(PulseTierToString(SelectedTier))));
	}

	StopTicking();
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(this, &SPulseAuditPanel::TickScan));

	return FReply::Handled();
}

FReply SPulseAuditPanel::OnCancelClicked()
{
	if (!Driver.IsValid())
	{
		return FReply::Handled();
	}

	Driver->Cancel();
	StopTicking();

	// A cancelled scan still has a valid partial report, and showing it is more useful than
	// discarding the work — the header records the truncation for anyone comparing runs.
	AdoptDriverReport();

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			LOCTEXT("StatusCancelled", "Cancelled after {0} of {1} assets. Showing partial results."),
			FText::AsNumber(ProgressProcessed),
			FText::AsNumber(ProgressTotal)));
	}

	return FReply::Handled();
}

FReply SPulseAuditPanel::OnExportClicked()
{
	if (!bHasReport)
	{
		return FReply::Handled();
	}

	const UPulseSettings& Settings = *GetDefault<UPulseSettings>();
	const FPulseReportWriter Writer(LastReportDir, /*bWriteCsv*/ true, /*bWriteJson*/ true, Settings.Scan.MaxHistoricalSnapshots);

	FString Error;
	if (!Writer.Write(Report, SelectedMinSeverity, Error))
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(LOCTEXT("StatusExportFailed", "Export failed: {0}"), FText::FromString(Error)));
		}
		UE_LOG(LogPulse, Warning, TEXT("Pulse panel: report export failed: %s"), *Error);
		return FReply::Handled();
	}

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(LOCTEXT("StatusExported", "Report written to {0}"), FText::FromString(LastReportDir)));
	}
	UE_LOG(LogPulse, Display, TEXT("Pulse panel: report written to %s"), *LastReportDir);

	return FReply::Handled();
}

bool SPulseAuditPanel::TickScan(float DeltaTime)
{
	if (!Driver.IsValid())
	{
		TickerHandle.Reset();
		return false;
	}

	const bool bMoreWork = Driver->Step(GPulsePanelStepBudgetSeconds);

	const FPulseScanProgress Progress = Driver->GetProgress();
	ProgressProcessed = Progress.AssetsProcessed;
	ProgressTotal = Progress.AssetsTotal;

	if (bMoreWork)
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(
				LOCTEXT("StatusScanning", "Scanning {0} of {1}: {2}"),
				FText::AsNumber(ProgressProcessed),
				FText::AsNumber(ProgressTotal),
				FText::FromName(Progress.CurrentCategory)));
		}
		return true;
	}

	AdoptDriverReport();

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			LOCTEXT("StatusComplete", "Complete. Overall score {0} across {1} assets; {2} issues listed."),
			FText::AsNumber(Report.OverallScore),
			FText::AsNumber(ProgressProcessed),
			FText::AsNumber(IssueItems.Num())));
	}

	// Returning false unregisters this ticker; clear the handle so StopTicking does not double-remove.
	TickerHandle.Reset();
	return false;
}

void SPulseAuditPanel::StopTicking()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

void SPulseAuditPanel::AdoptDriverReport()
{
	if (!Driver.IsValid())
	{
		return;
	}

	// Copy the report out before releasing the driver: the list items reference nothing the driver
	// owns, so the results stay browsable while the next scan is configured.
	Report = Driver->FinalizeReport();
	bHasReport = true;
	Driver.Reset();

	RebuildIssueList();
}

void SPulseAuditPanel::RebuildIssueList()
{
	IssueItems.Reset();

	if (bHasReport)
	{
		for (const FPulseCategoryResult& Category : Report.Categories)
		{
			for (const FPulseAssetResult& Asset : Category.Assets)
			{
				for (const FPulseIssue& Issue : Asset.Issues)
				{
					if (Issue.Severity < SelectedMinSeverity)
					{
						continue;
					}

					TSharedPtr<FPulseIssueListItem> Item = MakeShared<FPulseIssueListItem>();
					Item->Severity = Issue.Severity;
					Item->Category = Category.Category;
					Item->AssetPath = Asset.AssetPath;
					Item->RuleId = Issue.RuleId;
					Item->Message = Issue.Message;
					Item->Recommendation = Issue.Recommendation;
					Item->DetectedAtTier = Issue.DetectedAtTier;
					Item->AssetScore = Asset.Score;
					IssueItems.Add(MoveTemp(Item));
				}
			}
		}
	}

	// Worst first, then by asset so an asset's issues stay together. Case-sensitive compare, matching
	// the report's own ordering rule.
	IssueItems.StableSort([](const TSharedPtr<FPulseIssueListItem>& A, const TSharedPtr<FPulseIssueListItem>& B)
	{
		if (A->Severity != B->Severity)
		{
			return A->Severity > B->Severity;
		}
		return A->AssetPath.ToString().Compare(B->AssetPath.ToString(), ESearchCase::CaseSensitive) < 0;
	});

	if (IssueListView.IsValid())
	{
		IssueListView->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SPulseAuditPanel::OnGenerateIssueRow(TSharedPtr<FPulseIssueListItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SPulseIssueRow, OwnerTable).Item(Item);
}

void SPulseAuditPanel::OnIssueDoubleClicked(TSharedPtr<FPulseIssueListItem> Item)
{
	if (!Item.IsValid())
	{
		return;
	}

	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	const FAssetData AssetData = AssetRegistryModule.Get().GetAssetByObjectPath(Item->AssetPath);
	if (!AssetData.IsValid())
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(
				LOCTEXT("StatusAssetGone", "{0} is no longer in the Asset Registry — it may have been deleted or renamed since the scan."),
				FText::FromString(Item->AssetPath.ToString())));
		}
		return;
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	ContentBrowserModule.Get().SyncBrowserToAssets(TArray<FAssetData>{ AssetData });
}

TOptional<float> SPulseAuditPanel::GetProgressFraction() const
{
	if (ProgressTotal <= 0)
	{
		return TOptional<float>(0.0f);
	}
	return TOptional<float>(static_cast<float>(ProgressProcessed) / static_cast<float>(ProgressTotal));
}

void SPulseAuditPanel::LoadPanelState()
{
	FString TierText;
	if (GConfig->GetString(GPulsePanelConfigSection, TEXT("Tier"), TierText, GEditorPerProjectIni))
	{
		for (const EPulseTier Tier : PulsePanelTierOptions())
		{
			if (TierText.Equals(PulseTierToString(Tier), ESearchCase::IgnoreCase))
			{
				SelectedTier = Tier;
				break;
			}
		}
	}

	FString SeverityText;
	if (GConfig->GetString(GPulsePanelConfigSection, TEXT("MinSeverity"), SeverityText, GEditorPerProjectIni))
	{
		PulseParseSeverity(SeverityText, SelectedMinSeverity);
	}

	// LoadPanelState runs before ChildSlot is built, so the box does not exist yet — Construct
	// applies this once the widget is live.
	GConfig->GetString(GPulsePanelConfigSection, TEXT("CategoryFilter"), PendingCategoryFilter, GEditorPerProjectIni);
}

void SPulseAuditPanel::SavePanelState() const
{
	GConfig->SetString(GPulsePanelConfigSection, TEXT("Tier"), PulseTierToString(SelectedTier), GEditorPerProjectIni);
	GConfig->SetString(GPulsePanelConfigSection, TEXT("MinSeverity"), PulseSeverityToString(SelectedMinSeverity), GEditorPerProjectIni);
	if (CategoryFilterBox.IsValid())
	{
		GConfig->SetString(GPulsePanelConfigSection, TEXT("CategoryFilter"), *CategoryFilterBox->GetText().ToString(), GEditorPerProjectIni);
	}
	GConfig->Flush(/*bRead*/ false, GEditorPerProjectIni);
}

#undef LOCTEXT_NAMESPACE
