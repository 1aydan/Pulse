// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
// Included rather than forward declared: TUniquePtr<FPulseTickCapture> needs the complete type where
// this widget is destroyed, and the report is held by value.
#include "TickAuditor/PulseTickCapture.h"
#include "TickAuditor/PulseTickResult.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class ITableRow;
class STableViewBase;

/** A results row: an index into the report's rows. */
struct FPulseTickListItem
{
	int32 Index = INDEX_NONE;
};

/**
 * The Tick Auditor section of the Tools > Pulse window. Start Play In Editor, play to the moment worth
 * measuring, press Capture: for a few seconds Pulse drives every actor and component tick in the game
 * and times each, then hands them back and ranks actor classes by what they cost the game thread.
 */
class SPulseTickAuditorPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPulseTickAuditorPanel)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual ~SPulseTickAuditorPanel() override;

private:
	TSharedRef<SWidget> BuildControlsRow();
	TSharedRef<SWidget> BuildResultsList();

	FReply OnCaptureClicked();
	FReply OnStopClicked();
	FReply OnExportClicked();
	FReply OnClearClicked();
	FReply OnOpenSettingsClicked();

	static bool IsPlaying();
	bool IsCapturing() const;
	bool CanCapture() const;
	bool HasReport() const { return bHasReport; }

	FText GetHintText() const;
	FText GetStatusText() const;
	FText GetSummaryText() const;
	TOptional<float> GetProgress() const;

	void OnCaptureFinished();
	void RebuildItems();

	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FPulseTickListItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnItemDoubleClicked(TSharedPtr<FPulseTickListItem> Item);

	TUniquePtr<FPulseTickCapture> Capture;

	FPulseTickReport Report;
	bool bHasReport = false;

	TArray<TSharedPtr<FPulseTickListItem>> Items;
	TSharedPtr<SListView<TSharedPtr<FPulseTickListItem>>> ListView;

	FString StatusLine;
};
