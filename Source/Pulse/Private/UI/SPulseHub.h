// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/** The tools the Pulse window switches between, in button order. */
enum class EPulseHubSection : uint8
{
	AssetAudit,
	NiagaraAuditor,
	MemoryAuditor
};

/**
 * The Tools > Pulse window: one tab holding every Pulse tool, switched by a row of segmented
 * buttons. Each tool is still its own panel, unchanged; this widget only hosts them.
 *
 * All panels are built once and kept alive, and switching only changes which one is visible. That
 * is a requirement, not a convenience: the Niagara auditor's run lives in a PIE session that its
 * panel's destructor cancels, so destroying the panel on a switch would kill a measurement because
 * someone glanced at the memory results. It also means each tool keeps its results across switches.
 */
class SPulseHub : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPulseHub)
	{
	}
	/** Section to open on. Unset reopens whichever section was showing last time. */
	SLATE_ARGUMENT(TOptional<EPulseHubSection>, InitialSection)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows a section, e.g. when an old per-tool tab id asks for one. */
	void SetActiveSection(EPulseHubSection Section);

private:
	EPulseHubSection GetActiveSection() const { return ActiveSection; }
	int32 GetActiveIndex() const { return static_cast<int32>(ActiveSection); }

	/** Last section shown, per user and per project, so the window reopens where it was left. */
	static EPulseHubSection LoadLastSection();
	static void SaveLastSection(EPulseHubSection Section);

	EPulseHubSection ActiveSection = EPulseHubSection::AssetAudit;
};
