// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "PulseTypes.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Views/STableRow.h"

/**
 * One row of the panel's results list: a single rule firing, flattened out of the nested report so
 * the list view can sort and display it without walking categories and assets on every paint.
 */
struct FPulseIssueListItem
{
	EPulseSeverity Severity = EPulseSeverity::Info;
	FName Category;
	FSoftObjectPath AssetPath;
	FName RuleId;
	FString Message;
	FString Recommendation;
	EPulseTier DetectedAtTier = EPulseTier::Fast;

	/** Score of the asset this issue belongs to, shown so a row carries its own context. */
	float AssetScore = 100.0f;
};

/** Column identifiers, shared by the header row and the row widget. */
struct FPulseIssueColumns
{
	static const FName Severity;
	static const FName Category;
	static const FName Asset;
	static const FName Rule;
	static const FName Message;
};

/** Displays one FPulseIssueListItem. Severity is colour-coded so a long list scans at a glance. */
class SPulseIssueRow : public SMultiColumnTableRow<TSharedPtr<FPulseIssueListItem>>
{
public:
	SLATE_BEGIN_ARGS(SPulseIssueRow)
	{
	}
	SLATE_ARGUMENT(TSharedPtr<FPulseIssueListItem>, Item)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable);

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override;

private:
	TSharedPtr<FPulseIssueListItem> Item;
};
