// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
// Included rather than forward declared: TUniquePtr<FPulseNiagaraAuditDriver> needs the complete
// type wherever this widget is destroyed, and SNew sites live in other translation units.
#include "NiagaraAuditor/PulseNiagaraAuditDriver.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class STextBlock;

/** One measured (system, quality, distance) point, flattened for the results list. */
struct FPulseNiagaraResultItem
{
	FString SystemName;
	FString AssetPath;
	FString QualityName;
	float Distance = GPulseNiagaraNoDistance;
	bool bMeasured = false;
	FString SkipReason;
	double TotalAvgMs = 0.0;
	double TotalMinMs = 0.0;
	double TotalMaxMs = 0.0;
	double PerInstanceAvgMs = 0.0;
};

/**
 * The Tools > Pulse Niagara Auditor tab. Runs the same FPulseNiagaraAuditDriver the commandlet
 * uses, stepped from a ticker so the editor stays responsive.
 *
 * The panel exists for more than convenience: it measures in the level editor's own world, which
 * the viewport renders every frame, and that is the only place Niagara can cache the views its
 * distance culling needs. The commandlet has to pause culling entirely; here it runs for real, so
 * the distance axis is available and GPU systems can be measured rather than skipped.
 */
class SPulseNiagaraAuditorPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPulseNiagaraAuditorPanel)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual ~SPulseNiagaraAuditorPanel() override;

private:
	TSharedRef<SWidget> BuildControlsRow();
	TSharedRef<SWidget> BuildResultsList();

	FReply OnRunClicked();
	FReply OnCancelClicked();
	FReply OnExportClicked();

	/** Opens Project Settings focused on the auditor, so thresholds are one click from the numbers. */
	FReply OnOpenSettingsClicked();

	bool IsRunEnabled() const;
	bool IsCancelEnabled() const;
	bool IsExportEnabled() const;

	/** True when a level editor viewport exists; without one the run cannot cache views. */
	static bool HasUsableViewport();

	/** True when a test map is configured and the open level is not it. */
	static bool ShouldOfferTestMap();

	EVisibility GetOpenTestMapVisibility() const;
	FReply OnOpenTestMapClicked();

	/** Toggles Niagara's own in-viewport debug HUD, which lists what is actually alive and ticking. */
	FReply OnToggleNiagaraHudClicked();
	FText GetNiagaraHudButtonText() const;

	/** Pulled by the viewport overlay each frame. Falls back to the last snapshot once the run ends. */
	FPulseNiagaraAuditProgress GetLiveProgress() const;

	/** Overlay cancel button. Same path as the panel's own Cancel. */
	void CancelFromOverlay();

	EVisibility GetWarningVisibility() const;
	FText GetWarningText() const;
	FText GetStatusText() const;
	TOptional<float> GetProgressFraction() const;

	/** Ticker callback. Steps the driver once per editor frame; false unregisters it. */
	bool TickAudit(float DeltaTime);

	void StopTicking();

	/** Finalizes the driver's report (partial runs included) and rebuilds the list. */
	void AdoptDriverReport();

	void RebuildResultList();

	TSharedRef<ITableRow> OnGenerateResultRow(TSharedPtr<FPulseNiagaraResultItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnResultDoubleClicked(TSharedPtr<FPulseNiagaraResultItem> Item);

	/** Non-null only while a run is live; reset once its report has been adopted. */
	TUniquePtr<FPulseNiagaraAuditDriver> Driver;

	/** FTSTicker uses its own weak handle type, not the generic FDelegateHandle. */
	FTSTicker::FDelegateHandle TickerHandle;

	/** Copied out of the driver at finalize time so the list outlives the driver. */
	FPulseNiagaraCostReport Report;
	bool bHasReport = false;

	FString StatusLine;
	FString LastExportDir;

	/** Mirrors the console command's state; the HUD itself has no queryable "is it on" accessor. */
	bool bNiagaraHudVisible = false;

	FPulseNiagaraAuditProgress Progress;

	TArray<TSharedPtr<FPulseNiagaraResultItem>> ResultItems;
	TSharedPtr<SListView<TSharedPtr<FPulseNiagaraResultItem>>> ResultListView;
};
