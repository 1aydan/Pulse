// Copyright (c) 2026 Pulse contributors. MIT License.

#include "UI/SPulseTickAuditorPanel.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "IContentBrowserSingleton.h"
#include "ISettingsModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Pulse.h"
#include "PulseTickAuditorSettings.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "TickAuditor/PulseTickReportWriter.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "PulseTickAuditorPanel"

static const FName GPulseTickColClass(TEXT("Class"));
static const FName GPulseTickColInstances(TEXT("Instances"));
static const FName GPulseTickColTotal(TEXT("Total"));
static const FName GPulseTickColPercent(TEXT("Percent"));
static const FName GPulseTickColPerInstance(TEXT("PerInstance"));
static const FName GPulseTickColActor(TEXT("Actor"));
static const FName GPulseTickColComponents(TEXT("Components"));
static const FName GPulseTickColFlags(TEXT("Flags"));

static FText PulseTickMsText(double Milliseconds)
{
	return FText::FromString(FString::Printf(TEXT("%.4f"), Milliseconds));
}

class SPulseTickRow : public SMultiColumnTableRow<TSharedPtr<FPulseTickListItem>>
{
public:
	SLATE_BEGIN_ARGS(SPulseTickRow)
	{
	}
	SLATE_ARGUMENT(TSharedPtr<FPulseTickListItem>, Item)
	SLATE_ARGUMENT(const FPulseTickReport*, Report)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Item = InArgs._Item;
		Report = InArgs._Report;
		SMultiColumnTableRow<TSharedPtr<FPulseTickListItem>>::Construct(FSuperRowType::FArguments(), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid() || Report == nullptr || !Report->Rows.IsValidIndex(Item->Index))
		{
			return SNullWidget::NullWidget;
		}
		const FPulseTickClassRow& Row = Report->Rows[Item->Index];
		const bool bOverBudget = Row.bOverClassBudget || Row.bOverInstanceBudget;

		if (ColumnName == GPulseTickColClass)
		{
			return SNew(STextBlock)
				.Text(FText::FromString(Row.ClassName))
				.ToolTipText(FText::FromString(Row.AssetPath.IsEmpty() ? FString::Printf(TEXT("Native class (%s)"), *Row.NativeParent) : Row.AssetPath))
				.ColorAndOpacity(bOverBudget ? FSlateColor(FStyleColors::Error) : FSlateColor::UseForeground());
		}
		if (ColumnName == GPulseTickColInstances)
		{
			return SNew(STextBlock).Text(FText::AsNumber(Row.NumInstances));
		}
		if (ColumnName == GPulseTickColTotal)
		{
			return SNew(STextBlock).Text(PulseTickMsText(Row.TotalMsPerFrame))
				.ToolTipText(FText::Format(LOCTEXT("TotalTooltip", "Every instance together, per frame. Worst single instance frame: {0} ms."),
					FText::FromString(FString::Printf(TEXT("%.4f"), Row.WorstInstanceFrameMs))));
		}
		if (ColumnName == GPulseTickColPercent)
		{
			const double Percent = Report->FrameBudgetMs > 0.0f ? Row.TotalMsPerFrame / Report->FrameBudgetMs * 100.0 : 0.0;
			return SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%.2f%%"), Percent)));
		}
		if (ColumnName == GPulseTickColPerInstance)
		{
			return SNew(STextBlock).Text(PulseTickMsText(Row.PerInstanceMsPerFrame));
		}
		if (ColumnName == GPulseTickColActor)
		{
			FString Text = Row.NumActorTicks > 0 ? FString::Printf(TEXT("%.4f"), Row.ActorMsPerFrame) : FString(TEXT("–"));
			const FString Tooltip = Row.NumActorTicks > 0
				? FString::Printf(TEXT("%d actor tick(s), shortest interval %s. Event Tick %s."), Row.NumActorTicks,
					Row.MinActorTickInterval > 0.0f ? *FString::Printf(TEXT("%.2fs"), Row.MinActorTickInterval) : TEXT("every frame"),
					Row.bHasEventTick ? TEXT("implemented") : TEXT("not implemented"))
				: FString(TEXT("The actor's own tick is off; only components tick."));
			return SNew(STextBlock).Text(FText::FromString(Text)).ToolTipText(FText::FromString(Tooltip));
		}
		if (ColumnName == GPulseTickColComponents)
		{
			TArray<FString> Lines;
			for (const FPulseTickComponentCost& Component : Row.Components)
			{
				Lines.Add(FString::Printf(TEXT("%s x%d: %.4f ms/frame%s"), *Component.ClassName, Component.Count, Component.MsPerFrame,
					Component.MinTickInterval > 0.0f ? *FString::Printf(TEXT(" (every %.2fs)"), Component.MinTickInterval) : TEXT("")));
			}
			const FString Summary = Row.Components.Num() > 0
				? FString::Printf(TEXT("%s %.4f%s"), *Row.Components[0].ClassName, Row.Components[0].MsPerFrame,
					Row.Components.Num() > 1 ? *FString::Printf(TEXT(" (+%d)"), Row.Components.Num() - 1) : TEXT(""))
				: FString(TEXT("–"));
			return SNew(STextBlock).Text(FText::FromString(Summary)).ToolTipText(FText::FromString(FString::Join(Lines, TEXT("\n"))));
		}
		if (ColumnName == GPulseTickColFlags)
		{
			TArray<FString> Flags;
			if (Row.bOverClassBudget)
			{
				Flags.Add(TEXT("Over class budget"));
			}
			if (Row.bOverInstanceBudget)
			{
				Flags.Add(TEXT("Over instance budget"));
			}
			if (Row.bIdleActorTick)
			{
				Flags.Add(TEXT("Idle tick"));
			}
			if (Row.bHasEventTick)
			{
				Flags.Add(TEXT("Event Tick"));
			}
			if (Row.bTicksEveryFrame)
			{
				Flags.Add(TEXT("Every frame"));
			}
			return SNew(STextBlock)
				.Text(FText::FromString(FString::Join(Flags, TEXT(", "))))
				.ToolTipText(LOCTEXT("FlagsTooltip",
					"Idle tick: the actor tick is on but does almost nothing and there is no Event Tick, so it can be turned off (Class Defaults > Actor Tick > Start with Tick Enabled).\n"
					"Every frame: at least one tick has no Tick Interval. Many gameplay ticks are fine at 0.1-0.2s.\n"
					"Over budget: past the share of the frame set in Pulse - Tick Auditor settings."))
				.ColorAndOpacity(bOverBudget ? FSlateColor(FStyleColors::Error) : Row.bIdleActorTick ? FSlateColor(FStyleColors::Warning) : FSlateColor::UseForeground());
		}
		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FPulseTickListItem> Item;

	/** Owned by the panel, which outlives its rows. */
	const FPulseTickReport* Report = nullptr;
};

void SPulseTickAuditorPanel::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			BuildControlsRow()
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(6.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(this, &SPulseTickAuditorPanel::GetHintText)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			SNew(SProgressBar).Percent(this, &SPulseTickAuditorPanel::GetProgress)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 0.0f)
		[
			SNew(STextBlock).Text(this, &SPulseTickAuditorPanel::GetStatusText)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			SNew(STextBlock).AutoWrapText(true).Text(this, &SPulseTickAuditorPanel::GetSummaryText)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			SNew(SSeparator)
		]

		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f)
		[
			BuildResultsList()
		]
	];
}

SPulseTickAuditorPanel::~SPulseTickAuditorPanel()
{
	// Closing the window mid-capture must hand every tick back to the engine. The destructor does
	// that without broadcasting, so nothing calls back into this half-destroyed widget.
	Capture.Reset();
}

TSharedRef<SWidget> SPulseTickAuditorPanel::BuildControlsRow()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Capture", "Capture"))
			.ToolTipText(LOCTEXT("CaptureTooltip", "Time every actor and component tick in the running PIE session for the configured number of frames."))
			.IsEnabled(this, &SPulseTickAuditorPanel::CanCapture)
			.OnClicked(this, &SPulseTickAuditorPanel::OnCaptureClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Stop", "Stop"))
			.ToolTipText(LOCTEXT("StopTooltip", "End the capture early. The frames captured so far are kept."))
			.IsEnabled(this, &SPulseTickAuditorPanel::IsCapturing)
			.OnClicked(this, &SPulseTickAuditorPanel::OnStopClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Export", "Export report"))
			.ToolTipText(LOCTEXT("ExportTooltip", "Write Classes.csv, Components.csv and Run.json."))
			.IsEnabled(this, &SPulseTickAuditorPanel::HasReport)
			.OnClicked(this, &SPulseTickAuditorPanel::OnExportClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Clear", "Clear"))
			.IsEnabled(this, &SPulseTickAuditorPanel::HasReport)
			.OnClicked(this, &SPulseTickAuditorPanel::OnClearClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Settings", "Settings"))
			.OnClicked(this, &SPulseTickAuditorPanel::OnOpenSettingsClicked)
		];
}

TSharedRef<SWidget> SPulseTickAuditorPanel::BuildResultsList()
{
	return SAssignNew(ListView, SListView<TSharedPtr<FPulseTickListItem>>)
		.ListItemsSource(&Items)
		.OnGenerateRow(this, &SPulseTickAuditorPanel::OnGenerateRow)
		.OnMouseButtonDoubleClick(this, &SPulseTickAuditorPanel::OnItemDoubleClicked)
		.HeaderRow(
			SNew(SHeaderRow)
			+ SHeaderRow::Column(GPulseTickColClass).DefaultLabel(LOCTEXT("ColClass", "Actor class")).FillWidth(0.22f)
			+ SHeaderRow::Column(GPulseTickColInstances).DefaultLabel(LOCTEXT("ColInstances", "Instances")).FillWidth(0.07f)
			+ SHeaderRow::Column(GPulseTickColTotal).DefaultLabel(LOCTEXT("ColTotal", "Total ms")).FillWidth(0.08f)
				.DefaultTooltip(LOCTEXT("ColTotalTooltip", "Every instance's ticks together, average milliseconds per frame on the game thread."))
			+ SHeaderRow::Column(GPulseTickColPercent).DefaultLabel(LOCTEXT("ColPercent", "% frame")).FillWidth(0.07f)
			+ SHeaderRow::Column(GPulseTickColPerInstance).DefaultLabel(LOCTEXT("ColPerInstance", "Per instance")).FillWidth(0.08f)
			+ SHeaderRow::Column(GPulseTickColActor).DefaultLabel(LOCTEXT("ColActor", "Actor tick")).FillWidth(0.08f)
				.DefaultTooltip(LOCTEXT("ColActorTooltip", "The actor's own tick (Event Tick and native Tick), all instances, per frame. The rest is components."))
			+ SHeaderRow::Column(GPulseTickColComponents).DefaultLabel(LOCTEXT("ColComponents", "Top component")).FillWidth(0.22f)
			+ SHeaderRow::Column(GPulseTickColFlags).DefaultLabel(LOCTEXT("ColFlags", "Flags")).FillWidth(0.18f)
		);
}

bool SPulseTickAuditorPanel::IsPlaying()
{
	return GEditor != nullptr && GEditor->PlayWorld != nullptr;
}

bool SPulseTickAuditorPanel::IsCapturing() const
{
	return Capture.IsValid() && Capture->IsRunning();
}

bool SPulseTickAuditorPanel::CanCapture() const
{
	return IsPlaying() && !IsCapturing();
}

FText SPulseTickAuditorPanel::GetHintText() const
{
	const UPulseTickAuditorSettings& Settings = *GetDefault<UPulseTickAuditorSettings>();
	if (!IsPlaying())
	{
		return FText::Format(
			LOCTEXT("HintNotPlaying", "Start Play In Editor and play to the moment worth measuring, then press Capture. For {0} frames Pulse runs every actor and component tick itself and times it; play may stutter slightly while it does."),
			FText::AsNumber(Settings.CaptureFrames));
	}
	return FText::Format(
		LOCTEXT("HintPlaying", "Playing. Capture times every tick for {0} frames, then hands them back to the engine. Keep playing while it runs: the numbers are whatever the game does in that window."),
		FText::AsNumber(Settings.CaptureFrames));
}

FText SPulseTickAuditorPanel::GetStatusText() const
{
	if (IsCapturing())
	{
		return FText::Format(LOCTEXT("Capturing", "Capturing... {0} / {1} frames"),
			FText::AsNumber(Capture->GetFramesCompleted()), FText::AsNumber(Capture->GetFramesToCapture()));
	}
	return FText::FromString(StatusLine);
}

FText SPulseTickAuditorPanel::GetSummaryText() const
{
	if (!bHasReport)
	{
		return FText::GetEmpty();
	}
	const double Percent = Report.FrameBudgetMs > 0.0f ? Report.TotalMsPerFrame / Report.FrameBudgetMs * 100.0 : 0.0;
	return FText::Format(
		LOCTEXT("Summary", "{0}: {1} frames ({2}s). {3} tick functions on {4} actor classes cost {5} ms per frame on the game thread, {6}% of a {7} ms frame."),
		FText::FromString(Report.WorldName),
		FText::AsNumber(Report.FramesCaptured),
		FText::FromString(FString::Printf(TEXT("%.1f"), Report.CapturedSeconds)),
		FText::AsNumber(Report.NumTickFunctions),
		FText::AsNumber(Report.Rows.Num()),
		FText::FromString(FString::Printf(TEXT("%.3f"), Report.TotalMsPerFrame)),
		FText::FromString(FString::Printf(TEXT("%.1f"), Percent)),
		FText::FromString(FString::Printf(TEXT("%.2f"), Report.FrameBudgetMs)));
}

TOptional<float> SPulseTickAuditorPanel::GetProgress() const
{
	if (!Capture.IsValid() || Capture->GetFramesToCapture() <= 0)
	{
		return TOptional<float>(0.0f);
	}
	return TOptional<float>(static_cast<float>(Capture->GetFramesCompleted()) / static_cast<float>(Capture->GetFramesToCapture()));
}

FReply SPulseTickAuditorPanel::OnCaptureClicked()
{
	const UPulseTickAuditorSettings& Settings = *GetDefault<UPulseTickAuditorSettings>();

	Capture = MakeUnique<FPulseTickCapture>();
	Capture->OnFinished.AddSP(this, &SPulseTickAuditorPanel::OnCaptureFinished);

	FString Error;
	if (!Capture->Start(GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr, Settings.CaptureFrames, Error))
	{
		StatusLine = Error;
		Capture.Reset();
	}
	return FReply::Handled();
}

FReply SPulseTickAuditorPanel::OnStopClicked()
{
	if (Capture.IsValid())
	{
		Capture->Stop();
	}
	return FReply::Handled();
}

void SPulseTickAuditorPanel::OnCaptureFinished()
{
	if (!Capture.IsValid() || !Capture->HasData())
	{
		StatusLine = TEXT("The capture ended before a whole frame was recorded.");
		return;
	}
	Report = Capture->BuildReport(*GetDefault<UPulseTickAuditorSettings>());
	bHasReport = true;
	RebuildItems();
	StatusLine = TEXT("Done. Every tick is back with the engine. Double-click a Blueprint to browse to it.");
}

FReply SPulseTickAuditorPanel::OnExportClicked()
{
	const UPulseTickAuditorSettings& Settings = *GetDefault<UPulseTickAuditorSettings>();
	FString ReportDir = Settings.ReportDirectory;
	if (FPaths::IsRelative(ReportDir))
	{
		ReportDir = FPaths::ProjectDir() / ReportDir;
	}

	FPulseTickReportWriter Writer(FPaths::ConvertRelativePathToFull(ReportDir), Settings.MaxHistoricalRuns);
	FString Error;
	if (!Writer.Write(Report, Error))
	{
		StatusLine = FString::Printf(TEXT("Export failed: %s"), *Error);
		return FReply::Handled();
	}
	StatusLine = FString::Printf(TEXT("Exported to %s"), *Writer.GetRunDir());
	UE_LOG(LogPulse, Display, TEXT("Tick capture report written to %s"), *Writer.GetRunDir());
	return FReply::Handled();
}

FReply SPulseTickAuditorPanel::OnClearClicked()
{
	Report = FPulseTickReport();
	bHasReport = false;
	StatusLine.Reset();
	RebuildItems();
	return FReply::Handled();
}

FReply SPulseTickAuditorPanel::OnOpenSettingsClicked()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
	{
		SettingsModule->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("PulseTickAuditorSettings"));
	}
	return FReply::Handled();
}

void SPulseTickAuditorPanel::RebuildItems()
{
	Items.Reset();
	if (bHasReport)
	{
		for (int32 Index = 0; Index < Report.Rows.Num(); ++Index)
		{
			TSharedPtr<FPulseTickListItem> Item = MakeShared<FPulseTickListItem>();
			Item->Index = Index;
			Items.Add(MoveTemp(Item));
		}
	}
	if (ListView.IsValid())
	{
		ListView->RebuildList();
	}
}

TSharedRef<ITableRow> SPulseTickAuditorPanel::OnGenerateRow(TSharedPtr<FPulseTickListItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SPulseTickRow, OwnerTable).Item(Item).Report(&Report);
}

void SPulseTickAuditorPanel::OnItemDoubleClicked(TSharedPtr<FPulseTickListItem> Item)
{
	if (!Item.IsValid() || !Report.Rows.IsValidIndex(Item->Index) || Report.Rows[Item->Index].AssetPath.IsEmpty())
	{
		return;
	}
	const IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(Report.Rows[Item->Index].AssetPath));
	if (AssetData.IsValid())
	{
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(TArray<FAssetData>{ AssetData });
	}
}

#undef LOCTEXT_NAMESPACE
