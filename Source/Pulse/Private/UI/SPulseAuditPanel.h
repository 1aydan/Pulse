// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "PulseResult.h"
#include "PulseTypes.h"
// Included rather than forward declared: TUniquePtr<FPulseScanDriver> needs the complete type
// wherever this widget is destroyed, and SNew sites live in other translation units.
#include "PulseScanDriver.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SEditableTextBox;
class STextBlock;
struct FPulseIssueListItem;

/**
 * The Tools > Pulse Audit tab. Runs the same FPulseScanDriver the commandlet uses, stepped from a
 * ticker at frame budget so a Deep scan of a large project never blocks the editor. Results are
 * clickable through to the Content Browser, and Export writes the identical files the commandlet
 * produces.
 */
class SPulseAuditPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPulseAuditPanel)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual ~SPulseAuditPanel() override;

private:
	TSharedRef<SWidget> BuildControlsRow();

	FReply OnRunClicked();
	FReply OnCancelClicked();
	FReply OnExportClicked();

	/** Opens Project Settings focused on Pulse, so thresholds are one click from the findings. */
	FReply OnOpenSettingsClicked();

	bool IsRunEnabled() const;
	bool IsCancelEnabled() const;
	bool IsExportEnabled() const;

	/** Ticker callback. Steps the driver for one frame budget; returns false to unregister itself. */
	bool TickScan(float DeltaTime);

	void StopTicking();

	/** Finalizes the driver's report (partial runs included) and rebuilds the list. */
	void AdoptDriverReport();

	void RebuildIssueList();

	TSharedRef<ITableRow> OnGenerateIssueRow(TSharedPtr<FPulseIssueListItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnIssueDoubleClicked(TSharedPtr<FPulseIssueListItem> Item);

	TOptional<float> GetProgressFraction() const;

	void LoadPanelState();
	void SavePanelState() const;

	// ---- Controls state -------------------------------------------------------------------------

	EPulseTier SelectedTier = EPulseTier::Fast;
	EPulseSeverity SelectedMinSeverity = EPulseSeverity::Info;

	TSharedPtr<SEditableTextBox> CategoryFilterBox;
	TSharedPtr<STextBlock> StatusText;

	/** Restored filter text, held until the text box exists to receive it. */
	FString PendingCategoryFilter;

	// ---- Scan state -----------------------------------------------------------------------------

	/** Non-null only while a scan is live; reset once its report has been adopted. */
	TUniquePtr<FPulseScanDriver> Driver;

	/** FTSTicker uses its own weak handle type, not the generic FDelegateHandle. */
	FTSTicker::FDelegateHandle TickerHandle;

	/** Copied out of the driver at finalize time so the list outlives the driver. */
	FPulseReport Report;
	bool bHasReport = false;

	/** The resolved report directory of the last run, reused by Export. */
	FString LastReportDir;

	int32 ProgressProcessed = 0;
	int32 ProgressTotal = 0;

	TArray<TSharedPtr<FPulseIssueListItem>> IssueItems;
	TSharedPtr<SListView<TSharedPtr<FPulseIssueListItem>>> IssueListView;
};
