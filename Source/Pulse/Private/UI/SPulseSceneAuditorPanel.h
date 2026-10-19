// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
// Included rather than forward declared: the report is held by value and rows read it.
#include "SceneAuditor/PulseSceneResult.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class ITableRow;
class STableViewBase;

/** A map, or one finding within it. */
struct FPulseSceneTreeItem
{
	int32 MapIndex = INDEX_NONE;

	/** INDEX_NONE on a map row. */
	int32 FindingIndex = INDEX_NONE;

	TArray<TSharedPtr<FPulseSceneTreeItem>> Children;
};

/**
 * The Scene Auditor section of the Tools > Pulse window. "Audit open map" inspects the level being
 * edited, in place, so a finding can select its actors in the viewport; "Audit maps" loads Maps to
 * Cook (or every map) one at a time and inspects each.
 */
class SPulseSceneAuditorPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPulseSceneAuditorPanel)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> BuildControlsRow();
	TSharedRef<SWidget> BuildTree();

	FReply OnAuditOpenMapClicked();
	FReply OnAuditMapsClicked();
	FReply OnExportClicked();
	FReply OnClearClicked();
	FReply OnOpenSettingsClicked();
	bool HasReport() const { return bHasReport; }
	static bool HasOpenMap();

	FText GetStatusText() const;

	void RebuildItems();

	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FPulseSceneTreeItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnGetChildren(TSharedPtr<FPulseSceneTreeItem> Item, TArray<TSharedPtr<FPulseSceneTreeItem>>& OutChildren);
	void OnItemDoubleClicked(TSharedPtr<FPulseSceneTreeItem> Item);
	TSharedPtr<SWidget> OnContextMenuOpening();

	/** True when the finding's map is the one open in the editor, so its actors can be selected there. */
	bool IsMapOpen(int32 MapIndex) const;

	// By value: bound as FUIAction payloads.
	void SelectActors(TSharedPtr<FPulseSceneTreeItem> Item) const;
	void BrowseToAsset(TSharedPtr<FPulseSceneTreeItem> Item) const;
	void CopyActorNames(TSharedPtr<FPulseSceneTreeItem> Item) const;
	void OpenMap(TSharedPtr<FPulseSceneTreeItem> Item);

	FPulseSceneReport Report;
	bool bHasReport = false;

	TArray<TSharedPtr<FPulseSceneTreeItem>> Items;
	TSharedPtr<STreeView<TSharedPtr<FPulseSceneTreeItem>>> TreeView;

	FString StatusLine;
};
