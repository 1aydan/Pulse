// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
// Included rather than forward declared: the report is held by shared pointer and rows read it.
#include "ContentCleanup/PulseCleanupResult.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"

class ITableRow;
class STableViewBase;

/** A row of the Unused list: an index into the report's Unused array. */
struct FPulseUnusedItem
{
	int32 Index = INDEX_NONE;
};

/** A row of the Duplicates tree: a group, or one copy within it. */
struct FPulseDuplicateItem
{
	int32 GroupIndex = INDEX_NONE;

	/** INDEX_NONE on a group row. */
	int32 MemberIndex = INDEX_NONE;

	TArray<TSharedPtr<FPulseDuplicateItem>> Children;
};

/**
 * The Content Cleanup section of the Tools > Pulse window: assets nothing that ships can load, and
 * imports that exist more than once. Read-only like every Pulse tool — it selects assets in the
 * Content Browser, and deleting them stays a deliberate act there, where the engine's own reference
 * checks run.
 */
class SPulseContentCleanupPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPulseContentCleanupPanel)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	enum class EView : uint8
	{
		Unused,
		Duplicates
	};

	TSharedRef<SWidget> BuildControlsRow();
	TSharedRef<SWidget> BuildUnusedFilterRow();
	TSharedRef<SWidget> BuildUnusedList();
	TSharedRef<SWidget> BuildDuplicatesTree();

	FReply OnRunClicked();
	FReply OnExportClicked();
	FReply OnClearClicked();
	FReply OnOpenSettingsClicked();
	bool HasReport() const { return Report.IsValid(); }

	FText GetStatusText() const;
	FText GetSummaryText() const;
	FText GetViewLabel(EView InView) const;

	EView GetView() const { return View; }
	void SetView(EView InView) { View = InView; }
	int32 GetViewIndex() const { return static_cast<int32>(View); }
	EVisibility GetUnusedFilterVisibility() const;

	void OnSearchTextChanged(const FText& InText);
	ECheckBoxState GetStatusFilterState(EPulseCleanupStatus Status) const;
	void OnStatusFilterChanged(ECheckBoxState NewState, EPulseCleanupStatus Status);
	ECheckBoxState GetHideMentionedState() const;
	void OnHideMentionedChanged(ECheckBoxState NewState);

	void RebuildItems();
	void ApplyUnusedFilters();

	TSharedRef<ITableRow> OnGenerateUnusedRow(TSharedPtr<FPulseUnusedItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> OnGenerateDuplicateRow(TSharedPtr<FPulseDuplicateItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnGetDuplicateChildren(TSharedPtr<FPulseDuplicateItem> Item, TArray<TSharedPtr<FPulseDuplicateItem>>& OutChildren);
	void OnUnusedDoubleClicked(TSharedPtr<FPulseUnusedItem> Item);
	void OnDuplicateDoubleClicked(TSharedPtr<FPulseDuplicateItem> Item);
	TSharedPtr<SWidget> OnUnusedContextMenu();
	TSharedPtr<SWidget> OnDuplicateContextMenu();

	/** Object paths of whatever is selected in the current view. A duplicate group means all its copies. */
	TArray<FString> GetSelectedObjectPaths() const;
	TArray<FName> GetSelectedPackageNames() const;

	void BrowseToSelected() const;
	void OpenReferenceViewerForSelected() const;
	void CopySelectedPaths() const;

	TSharedPtr<FPulseCleanupReport> Report;

	TArray<TSharedPtr<FPulseUnusedItem>> AllUnusedItems;
	TArray<TSharedPtr<FPulseUnusedItem>> VisibleUnusedItems;
	TSharedPtr<SListView<TSharedPtr<FPulseUnusedItem>>> UnusedList;

	TArray<TSharedPtr<FPulseDuplicateItem>> DuplicateItems;
	TSharedPtr<STreeView<TSharedPtr<FPulseDuplicateItem>>> DuplicateTree;

	EView View = EView::Unused;
	FString SearchText;
	TSet<EPulseCleanupStatus> HiddenStatuses;
	bool bHideMentioned = false;

	FString StatusLine;
};
