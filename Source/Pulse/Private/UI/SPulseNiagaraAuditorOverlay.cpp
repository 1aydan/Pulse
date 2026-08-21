// Copyright (c) 2026 Pulse contributors. MIT License.

#include "UI/SPulseNiagaraAuditorOverlay.h"

#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "SLevelViewport.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PulseNiagaraAuditorOverlay"

void SPulseNiagaraAuditorOverlay::Construct(const FArguments& InArgs)
{
	OnGetProgress = InArgs._OnGetProgress;
	OnCancel = InArgs._OnCancel;

	ChildSlot
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Top)
	.Padding(16.0f)
	[
		SNew(SBox)
		.WidthOverride(340.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(10.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
					.Text(this, &SPulseNiagaraAuditorOverlay::GetHeadlineText)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SProgressBar).Percent(this, &SPulseNiagaraAuditorOverlay::GetOverallFraction)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(this, &SPulseNiagaraAuditorOverlay::GetSystemText)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(this, &SPulseNiagaraAuditorOverlay::GetAxisText)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
					.Text(this, &SPulseNiagaraAuditorOverlay::GetLiveCostText)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(SProgressBar).Percent(this, &SPulseNiagaraAuditorOverlay::GetWindowFraction)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				.HAlign(HAlign_Right)
				[
					SNew(SButton)
					.Text(LOCTEXT("Cancel", "Cancel"))
					.OnClicked(this, &SPulseNiagaraAuditorOverlay::OnCancelClicked)
				]
			]
		]
	];
}

FPulseNiagaraAuditProgress SPulseNiagaraAuditorOverlay::FetchProgress() const
{
	return OnGetProgress.IsBound() ? OnGetProgress.Execute() : FPulseNiagaraAuditProgress();
}

FText SPulseNiagaraAuditorOverlay::GetHeadlineText() const
{
	const FPulseNiagaraAuditProgress Progress = FetchProgress();
	return FText::FromString(FString::Printf(TEXT("Pulse Niagara Auditor  -  %d / %d"),
		Progress.SamplesCompleted, Progress.SamplesTotal));
}

FText SPulseNiagaraAuditorOverlay::GetSystemText() const
{
	const FPulseNiagaraAuditProgress Progress = FetchProgress();
	return Progress.CurrentSystemName.IsEmpty()
		? LOCTEXT("Preparing", "Preparing...")
		: FText::FromString(Progress.CurrentSystemName);
}

FText SPulseNiagaraAuditorOverlay::GetAxisText() const
{
	const FPulseNiagaraAuditProgress Progress = FetchProgress();

	FString Axis = Progress.CurrentQualityName;
	if (Progress.CurrentDistance > GPulseNiagaraNoDistance)
	{
		Axis += FString::Printf(TEXT("  -  %.0f cm"), Progress.CurrentDistance);
	}
	if (Progress.LiveInstanceCount > 0)
	{
		Axis += FString::Printf(TEXT("  -  %d instances"), Progress.LiveInstanceCount);
	}
	return FText::FromString(Axis);
}

FText SPulseNiagaraAuditorOverlay::GetLiveCostText() const
{
	const FPulseNiagaraAuditProgress Progress = FetchProgress();

	// Settling deliberately shows no number rather than a rising one. Those frames are discarded, so
	// a figure here would be a value the report never contains and the viewer would watch it drop.
	if (!Progress.bMeasuring || Progress.LiveFramesRecorded == 0)
	{
		return LOCTEXT("Settling", "settling...");
	}

	return FText::FromString(FString::Printf(TEXT("%.4f ms  (%d/%d frames)"),
		Progress.LiveAvgMs, Progress.LiveFramesRecorded, Progress.MeasureFrames));
}

TOptional<float> SPulseNiagaraAuditorOverlay::GetOverallFraction() const
{
	const FPulseNiagaraAuditProgress Progress = FetchProgress();
	if (Progress.SamplesTotal <= 0)
	{
		return TOptional<float>(0.0f);
	}
	return TOptional<float>(static_cast<float>(Progress.SamplesCompleted) / static_cast<float>(Progress.SamplesTotal));
}

TOptional<float> SPulseNiagaraAuditorOverlay::GetWindowFraction() const
{
	const FPulseNiagaraAuditProgress Progress = FetchProgress();
	if (!Progress.bMeasuring || Progress.MeasureFrames <= 0)
	{
		return TOptional<float>(0.0f);
	}
	return TOptional<float>(FMath::Clamp(
		static_cast<float>(Progress.LiveFramesRecorded) / static_cast<float>(Progress.MeasureFrames), 0.0f, 1.0f));
}

FReply SPulseNiagaraAuditorOverlay::OnCancelClicked()
{
	OnCancel.ExecuteIfBound();
	return FReply::Handled();
}

namespace PulseNiagaraAuditorOverlay
{
	/** The single live overlay, plus the viewport it was attached to so removal targets the same one. */
	static TSharedPtr<SPulseNiagaraAuditorOverlay> GOverlay;
	static TWeakPtr<SLevelViewport> GHostViewport;

	void Show(FPulseNiagaraGetProgress OnGetProgress, FSimpleDelegate OnCancel)
	{
		Hide();

		FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
		if (LevelEditorModule == nullptr)
		{
			return;
		}

		const TSharedPtr<SLevelViewport> Viewport = LevelEditorModule->GetFirstActiveLevelViewport();
		if (!Viewport.IsValid())
		{
			// No viewport is not an error: the console commands can run with the tab closed, and the
			// audit itself does not need a widget. The overlay is simply unavailable.
			return;
		}

		GOverlay = SNew(SPulseNiagaraAuditorOverlay)
			.OnGetProgress(OnGetProgress)
			.OnCancel(OnCancel);

		Viewport->AddOverlayWidget(GOverlay.ToSharedRef());
		GHostViewport = Viewport;
	}

	void Hide()
	{
		if (!GOverlay.IsValid())
		{
			return;
		}

		// Removed from the viewport it was added to, not from whichever is active now — the user may
		// have switched viewports mid-run, and removing from the wrong one would strand the widget.
		if (const TSharedPtr<SLevelViewport> Viewport = GHostViewport.Pin())
		{
			Viewport->RemoveOverlayWidget(GOverlay.ToSharedRef());
		}

		GOverlay.Reset();
		GHostViewport.Reset();
	}
}

#undef LOCTEXT_NAMESPACE
