// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
// Included rather than forward declared: the report is held by shared pointer and its rows read it.
#include "MemoryAuditor/PulseMemoryResult.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class ITableRow;
class STableViewBase;

/**
 * One row in the results tree. A top-level row stands for a root; its children are every package
 * that root reaches, as a flat list rather than a nested tree — nesting repeats shared packages under
 * every path to them and buries the large leaf assets that are usually the actual problem.
 */
struct FPulseMemoryTreeItem
{
	int32 NodeIndex = INDEX_NONE;

	/** The root this row belongs to; on a top-level row, the root it stands for. */
	int32 RootIndex = INDEX_NONE;

	bool bIsRoot = false;

	/** False for a dependency only reachable through a soft reference: drawn greyed as "may load". */
	bool bHard = true;

	/** Stand-in child so an unexpanded root shows an expander without computing its reach. */
	bool bIsPlaceholder = false;

	/** Hops from the root. */
	int32 Depth = 0;

	/** "Root > A > B": the shortest reference chain that loads this row. */
	FString Via;

	/**
	 * Every package in this root's reach that references this row directly. Via is one of them; when
	 * there are others, cutting the via chain alone will not unload the asset.
	 */
	FString ReferencedBy;
	int32 NumReferencers = 0;

	bool bChildrenBuilt = false;
	TArray<TSharedPtr<FPulseMemoryTreeItem>> Children;
};

/**
 * The Memory Auditor section of the Tools > Pulse window: every top-level asset ranked by what it
 * keeps in RAM and VRAM, expandable to the dependencies responsible, each with the reference chain
 * that loads it.
 *
 * Runs the same FPulseMemoryAnalyzer as the commandlet. It reads only the Asset Registry, so a run
 * takes seconds and is wrapped in a slow-task dialog rather than ticked across frames.
 *
 * Assets dragged in from the Content Browser are inspected: added as top-level, pinned above the
 * ranking and expanded. Inspecting re-runs the analysis rather than patching the last result, because
 * a new top-level asset changes which packages the graph reaches and so the Unique figures around it.
 */
class SPulseMemoryAuditorPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPulseMemoryAuditorPanel)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Drop target for Content Browser assets.
	virtual void OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual void OnDragLeave(const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;

private:
	TSharedRef<SWidget> BuildControlsRow();
	TSharedRef<SWidget> BuildFilterRow();
	TSharedRef<SWidget> BuildResultsTree();

	FReply OnRunClicked();
	FReply OnExportClicked();
	FReply OnOpenSettingsClicked();
	bool IsExportEnabled() const;

	/** Drops the results and every inspected asset, back to the state the panel opened in. */
	FReply OnClearClicked();
	bool IsClearEnabled() const;

	/** Runs the analyzer with the current settings plus InspectedPackages. False when it stopped early. */
	bool RunAnalysis();

	/** Adds packages to the inspected set and re-runs; then selects and expands the first of them. */
	void InspectPackages(const TArray<FName>& PackageNames);

	/** Selects, expands and scrolls to a package's top-level row, if it has one. */
	void FocusRootForPackage(FName PackageName);

	FText GetStatusText() const;
	FText GetSummaryText() const;

	void OnSearchTextChanged(const FText& InText);
	void OnKindFilterChanged(ECheckBoxState NewState, EPulseMemoryRootKind Kind);
	ECheckBoxState GetKindFilterState(EPulseMemoryRootKind Kind) const;
	void OnShowMayLoadChanged(ECheckBoxState NewState);
	ECheckBoxState GetShowMayLoadState() const;

	/** Rebuilds every root item from the report. Expansion state is lost. */
	void RebuildRootItems();

	/** Re-applies the search and kind filters to the root items. */
	void ApplyFilters();

	/** Computes a root's reach and fills its children. */
	void BuildChildren(const TSharedPtr<FPulseMemoryTreeItem>& RootItem);

	/** Drops every built child list, e.g. after the may-load toggle changed what they contain. */
	void ResetChildren();

	void SortItems(TArray<TSharedPtr<FPulseMemoryTreeItem>>& Items) const;
	void SortAll();

	EColumnSortMode::Type GetColumnSortMode(const FName ColumnId) const;
	void OnColumnSortModeChanged(const EColumnSortPriority::Type SortPriority, const FName& ColumnId, const EColumnSortMode::Type NewSortMode);

	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FPulseMemoryTreeItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnGetChildren(TSharedPtr<FPulseMemoryTreeItem> Item, TArray<TSharedPtr<FPulseMemoryTreeItem>>& OutChildren);
	void OnExpansionChanged(TSharedPtr<FPulseMemoryTreeItem> Item, bool bExpanded);
	void OnItemDoubleClicked(TSharedPtr<FPulseMemoryTreeItem> Item);
	TSharedPtr<SWidget> OnContextMenuOpening();

	// By value: these are bound as FUIAction payloads, which store and pass a copy.
	void BrowseToItem(TSharedPtr<FPulseMemoryTreeItem> Item) const;
	void OpenReferenceViewer(TSharedPtr<FPulseMemoryTreeItem> Item) const;
	void CopyReferenceChain(TSharedPtr<FPulseMemoryTreeItem> Item) const;
	void InspectItem(TSharedPtr<FPulseMemoryTreeItem> Item);

	/** Writes the per-asset report (Summary.md and Dependencies.csv) for a top-level row, then opens its folder. */
	void ExportAssetReport(TSharedPtr<FPulseMemoryTreeItem> Item);
	void StopInspectingItem(TSharedPtr<FPulseMemoryTreeItem> Item);

	/** Shared with the rows, which read it every paint; null until the first run. */
	TSharedPtr<FPulseMemoryReport> Report;

	TArray<TSharedPtr<FPulseMemoryTreeItem>> AllRootItems;
	TArray<TSharedPtr<FPulseMemoryTreeItem>> VisibleRootItems;
	TSharedPtr<STreeView<TSharedPtr<FPulseMemoryTreeItem>>> TreeView;

	FString SearchText;
	TSet<EPulseMemoryRootKind> HiddenKinds;
	bool bShowMayLoad = true;

	/** NAME_None sorts by resident total (RAM + VRAM), which no single column shows. */
	FName SortColumn;
	EColumnSortMode::Type SortMode = EColumnSortMode::Descending;

	FString StatusLine;
	FString LastExportDir;

	/** Package names inspected this session, in the order they were added. Survive re-runs; Clear empties them. */
	TArray<FName> InspectedPackages;
};
