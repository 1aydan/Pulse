// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "NiagaraAuditor/PulseNiagaraAuditDriver.h"
#include "Widgets/SCompoundWidget.h"

/** Pulled every frame while the overlay is up, so the overlay owns no state of its own. */
DECLARE_DELEGATE_RetVal(FPulseNiagaraAuditProgress, FPulseNiagaraGetProgress);

/**
 * Heads-up display drawn over the level viewport while an audit runs, in the spirit of
 * SPSOForgeOverlay.
 *
 * It exists because an editor run is something you WATCH: systems spawn in front of the camera and
 * the interesting question — is this the one that is expensive? — is asked while looking at the
 * viewport, not at a docked tab that may not even be visible. It shows the system currently on the
 * stage, the axis point being measured, and the cost accumulating live.
 */
class SPulseNiagaraAuditorOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPulseNiagaraAuditorOverlay)
	{
	}
	SLATE_EVENT(FPulseNiagaraGetProgress, OnGetProgress)
	SLATE_EVENT(FSimpleDelegate, OnCancel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FPulseNiagaraAuditProgress FetchProgress() const;

	FText GetHeadlineText() const;
	FText GetSystemText() const;
	FText GetAxisText() const;
	FText GetLiveCostText() const;
	TOptional<float> GetOverallFraction() const;
	TOptional<float> GetWindowFraction() const;

	FReply OnCancelClicked();

	FPulseNiagaraGetProgress OnGetProgress;
	FSimpleDelegate OnCancel;
};

/**
 * Attaches and detaches the overlay on the active level viewport.
 *
 * Free functions rather than widget statics because both front ends need them — the panel and the
 * Pulse.Niagara.* console commands — and neither owns the other.
 */
namespace PulseNiagaraAuditorOverlay
{
	/** Idempotent. Does nothing when no level viewport is available. */
	void Show(FPulseNiagaraGetProgress OnGetProgress, FSimpleDelegate OnCancel);

	/** Idempotent, and safe to call when nothing was ever shown. */
	void Hide();
}
