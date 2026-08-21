// Copyright (c) 2026 Pulse contributors. MIT License.

#include "UI/SPulseNiagaraAuditorPanel.h"

#include "UI/SPulseNiagaraAuditorOverlay.h"

#include "ContentBrowserModule.h"
#include "Editor.h"
#include "IContentBrowserSingleton.h"
#include "ISettingsModule.h"
#include "FileHelpers.h"
#include "LevelEditorViewport.h"
#include "NiagaraAuditor/PulseNiagaraCostStage.h"
#include "Modules/ModuleManager.h"
#include "NiagaraAuditor/PulseNiagaraAuditorConfig.h"
#include "NiagaraAuditor/PulseNiagaraCostReportWriter.h"
#include "Pulse.h"
#include "PulseNiagaraAuditorSettings.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "PulseNiagaraAuditorPanel"

static const FName GPulseColumnSystem(TEXT("System"));
static const FName GPulseColumnQuality(TEXT("Quality"));
static const FName GPulseColumnDistance(TEXT("Distance"));
static const FName GPulseColumnAvg(TEXT("Avg"));
static const FName GPulseColumnMin(TEXT("Min"));
static const FName GPulseColumnMax(TEXT("Max"));
static const FName GPulseColumnPerInstance(TEXT("PerInstance"));

/** One results row. Unmeasured points show their skip reason instead of empty cost cells. */
class SPulseNiagaraResultRow : public SMultiColumnTableRow<TSharedPtr<FPulseNiagaraResultItem>>
{
public:
	SLATE_BEGIN_ARGS(SPulseNiagaraResultRow)
	{
	}
	SLATE_ARGUMENT(TSharedPtr<FPulseNiagaraResultItem>, Item)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Item = InArgs._Item;
		SMultiColumnTableRow<TSharedPtr<FPulseNiagaraResultItem>>::Construct(FSuperRowType::FArguments(), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		if (ColumnName == GPulseColumnSystem)
		{
			return SNew(STextBlock).Text(FText::FromString(Item->SystemName)).ToolTipText(FText::FromString(Item->AssetPath));
		}
		if (ColumnName == GPulseColumnQuality)
		{
			return SNew(STextBlock).Text(FText::FromString(Item->QualityName));
		}
		if (ColumnName == GPulseColumnDistance)
		{
			const FText DistanceText = Item->Distance > GPulseNiagaraNoDistance
				? FText::FromString(FString::Printf(TEXT("%.0f cm"), Item->Distance))
				: LOCTEXT("NoDistance", "n/a");
			return SNew(STextBlock).Text(DistanceText);
		}

		if (!Item->bMeasured)
		{
			// The reason belongs in the first cost column rather than nowhere: a blank row invites
			// the reader to assume zero, which is the one conclusion this panel must never suggest.
			return ColumnName == GPulseColumnAvg
				? StaticCastSharedRef<SWidget>(SNew(STextBlock)
					.Text(LOCTEXT("NotMeasured", "not measured"))
					.ToolTipText(FText::FromString(Item->SkipReason)))
				: SNullWidget::NullWidget;
		}

		double Value = 0.0;
		if (ColumnName == GPulseColumnAvg)
		{
			Value = Item->TotalAvgMs;
		}
		else if (ColumnName == GPulseColumnMin)
		{
			Value = Item->TotalMinMs;
		}
		else if (ColumnName == GPulseColumnMax)
		{
			Value = Item->TotalMaxMs;
		}
		else if (ColumnName == GPulseColumnPerInstance)
		{
			Value = Item->PerInstanceAvgMs;
		}
		else
		{
			return SNullWidget::NullWidget;
		}

		return SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%.4f"), Value)));
	}

private:
	TSharedPtr<FPulseNiagaraResultItem> Item;
};

void SPulseNiagaraAuditorPanel::Construct(const FArguments& InArgs)
{
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
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Visibility(this, &SPulseNiagaraAuditorPanel::GetWarningVisibility)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(this, &SPulseNiagaraAuditorPanel::GetWarningText)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SProgressBar).Percent(this, &SPulseNiagaraAuditorPanel::GetProgressFraction)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 0.0f)
		[
			SNew(STextBlock).Text(this, &SPulseNiagaraAuditorPanel::GetStatusText)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SSeparator)
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(4.0f)
		[
			BuildResultsList()
		]
	];
}

SPulseNiagaraAuditorPanel::~SPulseNiagaraAuditorPanel()
{
	// Order matters: the ticker must stop before the driver dies, or a queued tick would step a
	// destroyed driver. Finalizing also releases the viewport realtime override the stage took.
	StopTicking();
	PulseNiagaraAuditorOverlay::Hide();
	if (Driver.IsValid())
	{
		Driver->Cancel();
		Driver->FinalizeReport();
		Driver.Reset();
	}
}

TSharedRef<SWidget> SPulseNiagaraAuditorPanel::BuildControlsRow()
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Run", "Run"))
			.ToolTipText(LOCTEXT("RunTooltip", "Spawn every Niagara system under the configured paths in this level and measure what it costs."))
			.IsEnabled(this, &SPulseNiagaraAuditorPanel::IsRunEnabled)
			.OnClicked(this, &SPulseNiagaraAuditorPanel::OnRunClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Cancel", "Cancel"))
			.IsEnabled(this, &SPulseNiagaraAuditorPanel::IsCancelEnabled)
			.OnClicked(this, &SPulseNiagaraAuditorPanel::OnCancelClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Export", "Export report"))
			.ToolTipText(LOCTEXT("ExportTooltip", "Write the per-quality CSVs, Comparison.csv and Run.json, identical to the commandlet's output."))
			.IsEnabled(this, &SPulseNiagaraAuditorPanel::IsExportEnabled)
			.OnClicked(this, &SPulseNiagaraAuditorPanel::OnExportClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("OpenTestMap", "Open test map"))
			.ToolTipText(LOCTEXT("OpenTestMapTooltip", "Prompts to save, then opens the map configured as TestMap. Measuring there keeps the level's own content from competing for the game thread."))
			.Visibility(this, &SPulseNiagaraAuditorPanel::GetOpenTestMapVisibility)
			.OnClicked(this, &SPulseNiagaraAuditorPanel::OnOpenTestMapClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.Text(this, &SPulseNiagaraAuditorPanel::GetNiagaraHudButtonText)
			.ToolTipText(LOCTEXT("NiagaraHudTooltip", "Toggles Niagara's own debug HUD in the viewport. Pulse reports cost; the HUD shows what is actually alive, its scalability state, and whether it was culled — which is what you want when a number looks wrong."))
			.OnClicked(this, &SPulseNiagaraAuditorPanel::OnToggleNiagaraHudClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Settings", "Settings"))
			.OnClicked(this, &SPulseNiagaraAuditorPanel::OnOpenSettingsClicked)
		];
}

TSharedRef<SWidget> SPulseNiagaraAuditorPanel::BuildResultsList()
{
	return SAssignNew(ResultListView, SListView<TSharedPtr<FPulseNiagaraResultItem>>)
		.ListItemsSource(&ResultItems)
		.OnGenerateRow(this, &SPulseNiagaraAuditorPanel::OnGenerateResultRow)
		.OnMouseButtonDoubleClick(this, &SPulseNiagaraAuditorPanel::OnResultDoubleClicked)
		.HeaderRow(
			SNew(SHeaderRow)
			+ SHeaderRow::Column(GPulseColumnSystem).DefaultLabel(LOCTEXT("ColSystem", "System")).FillWidth(0.30f)
			+ SHeaderRow::Column(GPulseColumnQuality).DefaultLabel(LOCTEXT("ColQuality", "Quality")).FillWidth(0.12f)
			+ SHeaderRow::Column(GPulseColumnDistance).DefaultLabel(LOCTEXT("ColDistance", "Distance")).FillWidth(0.12f)
			+ SHeaderRow::Column(GPulseColumnAvg).DefaultLabel(LOCTEXT("ColAvg", "Avg ms")).FillWidth(0.12f)
			+ SHeaderRow::Column(GPulseColumnMin).DefaultLabel(LOCTEXT("ColMin", "Min ms")).FillWidth(0.11f)
			+ SHeaderRow::Column(GPulseColumnMax).DefaultLabel(LOCTEXT("ColMax", "Max ms")).FillWidth(0.11f)
			+ SHeaderRow::Column(GPulseColumnPerInstance).DefaultLabel(LOCTEXT("ColPerInstance", "Per inst.")).FillWidth(0.12f)
		);
}

bool SPulseNiagaraAuditorPanel::HasUsableViewport()
{
	return GCurrentLevelEditingViewportClient != nullptr && GCurrentLevelEditingViewportClient->GetWorld() != nullptr;
}

bool SPulseNiagaraAuditorPanel::ShouldOfferTestMap()
{
	const UPulseNiagaraAuditorSettings& Settings = *GetDefault<UPulseNiagaraAuditorSettings>();
	if (Settings.Measurement.TestMap.IsNull() || !HasUsableViewport())
	{
		return false;
	}
	return !FPulseNiagaraCostStage::IsWorldTheTestMap(
		GCurrentLevelEditingViewportClient->GetWorld(), Settings.Measurement.TestMap);
}

EVisibility SPulseNiagaraAuditorPanel::GetWarningVisibility() const
{
	return (!HasUsableViewport() || ShouldOfferTestMap()) ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SPulseNiagaraAuditorPanel::GetOpenTestMapVisibility() const
{
	return ShouldOfferTestMap() ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SPulseNiagaraAuditorPanel::GetWarningText() const
{
	if (!HasUsableViewport())
	{
		return LOCTEXT("NoViewportWarning",
			"No level editor viewport is available. Niagara takes the views its distance culling needs from the level viewport, so open a level before running.");
	}

	// Deliberately not a blocker. Measuring where you are is legitimate and sometimes what you want;
	// it just is not comparable with a test-map run, and that is worth saying before the numbers land.
	return LOCTEXT("NotInTestMapWarning",
		"Not in the configured test map. This level's own content competes for the game thread, so these costs will not be comparable with test-map runs.");
}

FReply SPulseNiagaraAuditorPanel::OnOpenTestMapClicked()
{
	const UPulseNiagaraAuditorSettings& Settings = *GetDefault<UPulseNiagaraAuditorSettings>();
	const FString PackageName = Settings.Measurement.TestMap.ToSoftObjectPath().GetLongPackageName();
	if (PackageName.IsEmpty())
	{
		return FReply::Handled();
	}

	// Prompt first, always. Loading a map discards unsaved work in the current one, and this button
	// is one click away from a results list — far too easy to hit without meaning to.
	if (!FEditorFileUtils::SaveDirtyPackages(/*bPromptUserToSave*/ true, /*bSaveMapPackages*/ true, /*bSaveContentPackages*/ true))
	{
		StatusLine = TEXT("Test map not opened: unsaved changes were kept.");
		return FReply::Handled();
	}

	FString Filename;
	if (!FPackageName::DoesPackageExist(PackageName, &Filename))
	{
		StatusLine = FString::Printf(TEXT("Test map package '%s' does not exist."), *PackageName);
		return FReply::Handled();
	}

	FEditorFileUtils::LoadMap(Filename, /*bLoadAsTemplate*/ false, /*bShowProgress*/ true);
	return FReply::Handled();
}

FText SPulseNiagaraAuditorPanel::GetStatusText() const
{
	return FText::FromString(StatusLine);
}

TOptional<float> SPulseNiagaraAuditorPanel::GetProgressFraction() const
{
	if (Progress.SamplesTotal <= 0)
	{
		return TOptional<float>(0.0f);
	}
	return TOptional<float>(static_cast<float>(Progress.SamplesCompleted) / static_cast<float>(Progress.SamplesTotal));
}

bool SPulseNiagaraAuditorPanel::IsRunEnabled() const
{
	return !Driver.IsValid() && HasUsableViewport();
}

bool SPulseNiagaraAuditorPanel::IsCancelEnabled() const
{
	return Driver.IsValid();
}

bool SPulseNiagaraAuditorPanel::IsExportEnabled() const
{
	return bHasReport && !Driver.IsValid();
}

FReply SPulseNiagaraAuditorPanel::OnRunClicked()
{
	const UPulseNiagaraAuditorSettings& Settings = *GetDefault<UPulseNiagaraAuditorSettings>();

	FPulseNiagaraAuditorConfig Config;
	Config.ApplySettingsDefaults(Settings);

	// PIE: the driver starts a play session and puts its player controller into spectator state.
	// That controller is what gives Niagara a view to cull against, and it is the same code path a
	// shipping build takes — which is the whole reason this panel exists alongside the commandlet.
	Driver = MakeUnique<FPulseNiagaraAuditDriver>(Config, Settings, EPulseNiagaraStageMode::PIEWorld);

	FString Error;
	if (!Driver->Initialize(Error))
	{
		StatusLine = FString::Printf(TEXT("Could not start: %s"), *Error);
		UE_LOG(LogPulse, Error, TEXT("%s"), *Error);
		Driver.Reset();
		return FReply::Handled();
	}

	bHasReport = false;
	ResultItems.Reset();
	if (ResultListView.IsValid())
	{
		ResultListView->RequestListRefresh();
	}

	Progress = Driver->GetProgress();
	StatusLine = TEXT("Running...");

	// Over the viewport, not just in this tab: the systems spawn in front of the camera, and the
	// question being asked while they do is answered by looking there.
	PulseNiagaraAuditorOverlay::Show(
		FPulseNiagaraGetProgress::CreateSP(this, &SPulseNiagaraAuditorPanel::GetLiveProgress),
		FSimpleDelegate::CreateSP(this, &SPulseNiagaraAuditorPanel::CancelFromOverlay));

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(this, &SPulseNiagaraAuditorPanel::TickAudit));
	return FReply::Handled();
}

FReply SPulseNiagaraAuditorPanel::OnCancelClicked()
{
	if (Driver.IsValid())
	{
		Driver->Cancel();
		StopTicking();
		AdoptDriverReport();
		StatusLine = TEXT("Cancelled. The partial report is still exportable.");
	}
	return FReply::Handled();
}

bool SPulseNiagaraAuditorPanel::TickAudit(float DeltaTime)
{
	if (!Driver.IsValid())
	{
		return false;
	}

	// The budget is nominal: in PIE the stage caps a Step at one measurement frame
	// regardless, because a sample is only valid once per rendered frame. The budget still bounds
	// the setup work — loading and compiling — that is not frame-locked.
	const bool bMoreWork = Driver->Step(/*TimeBudgetSeconds*/ 0.008);
	Progress = Driver->GetProgress();

	if (!Driver->GetError().IsEmpty())
	{
		StatusLine = Driver->GetError();
		UE_LOG(LogPulse, Error, TEXT("%s"), *Driver->GetError());
		AdoptDriverReport();
		TickerHandle.Reset();
		return false;
	}

	if (bMoreWork)
	{
		StatusLine = FString::Printf(TEXT("%d/%d - %s (%s%s)"),
			Progress.SamplesCompleted, Progress.SamplesTotal,
			*Progress.CurrentSystemName, *Progress.CurrentQualityName,
			Progress.CurrentDistance > GPulseNiagaraNoDistance
				? *FString::Printf(TEXT(", %.0f cm"), Progress.CurrentDistance)
				: TEXT(""));
		return true;
	}

	AdoptDriverReport();
	StatusLine = FString::Printf(TEXT("Done. %d measured, %d skipped, of %d discovered."),
		Report.NumSystemsMeasured, Report.NumSystemsSkipped, Report.NumSystemsDiscovered);

	TickerHandle.Reset();
	return false;
}

void SPulseNiagaraAuditorPanel::StopTicking()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

void SPulseNiagaraAuditorPanel::AdoptDriverReport()
{
	if (!Driver.IsValid())
	{
		return;
	}

	Report = Driver->FinalizeReport();
	bHasReport = true;
	Driver.Reset();

	PulseNiagaraAuditorOverlay::Hide();

	RebuildResultList();
}

void SPulseNiagaraAuditorPanel::RebuildResultList()
{
	ResultItems.Reset();

	for (const FPulseNiagaraCostRow& Row : Report.Rows)
	{
		for (const FPulseNiagaraCostSample& Sample : Row.Samples)
		{
			TSharedPtr<FPulseNiagaraResultItem> Item = MakeShared<FPulseNiagaraResultItem>();
			Item->SystemName = Row.AssetName;
			Item->AssetPath = Row.AssetPath;

			const int32 QualityIndex = Report.QualityLevels.Find(Sample.QualityLevel);
			Item->QualityName = Report.QualityLevelNames.IsValidIndex(QualityIndex)
				? Report.QualityLevelNames[QualityIndex]
				: FString::FromInt(Sample.QualityLevel);

			Item->Distance = Sample.Distance;
			Item->bMeasured = Sample.bMeasured;
			Item->SkipReason = Sample.SkipReason;
			Item->TotalAvgMs = Sample.TotalAvgMs;
			Item->TotalMinMs = Sample.TotalMinMs;
			Item->TotalMaxMs = Sample.TotalMaxMs;
			Item->PerInstanceAvgMs = Sample.PerInstanceAvgMs;

			ResultItems.Add(MoveTemp(Item));
		}
	}

	// Most expensive first: the list exists to answer "what should I look at", and alphabetical
	// order answers a question nobody asked. Unmeasured points sink to the bottom.
	ResultItems.Sort([](const TSharedPtr<FPulseNiagaraResultItem>& Left, const TSharedPtr<FPulseNiagaraResultItem>& Right)
	{
		if (Left->bMeasured != Right->bMeasured)
		{
			return Left->bMeasured;
		}
		return Left->TotalAvgMs > Right->TotalAvgMs;
	});

	if (ResultListView.IsValid())
	{
		ResultListView->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SPulseNiagaraAuditorPanel::OnGenerateResultRow(TSharedPtr<FPulseNiagaraResultItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SPulseNiagaraResultRow, OwnerTable).Item(Item);
}

void SPulseNiagaraAuditorPanel::OnResultDoubleClicked(TSharedPtr<FPulseNiagaraResultItem> Item)
{
	if (!Item.IsValid() || Item->AssetPath.IsEmpty())
	{
		return;
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowserModule.Get().SyncBrowserToAssets(TArray<FAssetData>{ FAssetData(FSoftObjectPath(Item->AssetPath).TryLoad()) });
}

FReply SPulseNiagaraAuditorPanel::OnExportClicked()
{
	const UPulseNiagaraAuditorSettings& Settings = *GetDefault<UPulseNiagaraAuditorSettings>();

	FPulseNiagaraAuditorConfig Config;
	Config.ApplySettingsDefaults(Settings);

	FPulseNiagaraCostReportWriter Writer(Config.ReportDir, Config.Budget, Settings.Scan.MaxHistoricalRuns);

	FString Error;
	if (!Writer.Write(Report, Error))
	{
		StatusLine = FString::Printf(TEXT("Export failed: %s"), *Error);
		UE_LOG(LogPulse, Error, TEXT("%s"), *Error);
		return FReply::Handled();
	}

	LastExportDir = Writer.GetRunDir();
	StatusLine = FString::Printf(TEXT("Exported to %s"), *LastExportDir);
	UE_LOG(LogPulse, Display, TEXT("Niagara audit report written to %s"), *LastExportDir);
	return FReply::Handled();
}

FPulseNiagaraAuditProgress SPulseNiagaraAuditorPanel::GetLiveProgress() const
{
	return Driver.IsValid() ? Driver->GetProgress() : Progress;
}

void SPulseNiagaraAuditorPanel::CancelFromOverlay()
{
	OnCancelClicked();
}

FText SPulseNiagaraAuditorPanel::GetNiagaraHudButtonText() const
{
	return bNiagaraHudVisible
		? LOCTEXT("HideNiagaraHud", "Hide Niagara HUD")
		: LOCTEXT("ShowNiagaraHud", "Niagara HUD");
}

FReply SPulseNiagaraAuditorPanel::OnToggleNiagaraHudClicked()
{
	// The engine's own HUD rather than a reimplementation of it. It already answers the questions a
	// cost number raises — is this system even alive, was it culled, how many particles — and the
	// Niagara Debugger's editor half is private to NiagaraEditor, so this console command is the
	// supported way to reach the same display.
	if (GEditor == nullptr)
	{
		return FReply::Handled();
	}

	bNiagaraHudVisible = !bNiagaraHudVisible;
	const FString Command = FString::Printf(TEXT("fx.Niagara.Debug.Hud Enabled=%d"), bNiagaraHudVisible ? 1 : 0);
	GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Command);
	return FReply::Handled();
}

FReply SPulseNiagaraAuditorPanel::OnOpenSettingsClicked()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
	{
		SettingsModule->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("PulseNiagaraAuditorSettings"));
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
