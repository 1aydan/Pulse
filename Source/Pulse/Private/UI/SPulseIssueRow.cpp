// Copyright (c) 2026 Pulse contributors. MIT License.

#include "SPulseIssueRow.h"

#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PulseAuditPanel"

const FName FPulseIssueColumns::Severity(TEXT("Severity"));
const FName FPulseIssueColumns::Category(TEXT("Category"));
const FName FPulseIssueColumns::Asset(TEXT("Asset"));
const FName FPulseIssueColumns::Rule(TEXT("Rule"));
const FName FPulseIssueColumns::Message(TEXT("Message"));

// Severity colouring. Deliberately not FAppStyle lookups: these are semantic (worse = hotter) and
// must stay legible whichever editor theme is active.
static FSlateColor PulseSeverityColor(EPulseSeverity Severity)
{
	switch (Severity)
	{
		case EPulseSeverity::Critical: return FSlateColor(FLinearColor(1.0f, 0.25f, 0.25f));
		case EPulseSeverity::High:     return FSlateColor(FLinearColor(1.0f, 0.55f, 0.15f));
		case EPulseSeverity::Medium:   return FSlateColor(FLinearColor(1.0f, 0.85f, 0.25f));
		case EPulseSeverity::Low:      return FSlateColor(FLinearColor(0.65f, 0.80f, 1.0f));
		case EPulseSeverity::Info:     return FSlateColor::UseSubduedForeground();
	}
	return FSlateColor::UseForeground();
}

void SPulseIssueRow::Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
{
	Item = InArgs._Item;

	SMultiColumnTableRow<TSharedPtr<FPulseIssueListItem>>::Construct(
		FSuperRowType::FArguments(), InOwnerTable);
}

TSharedRef<SWidget> SPulseIssueRow::GenerateWidgetForColumn(const FName& ColumnName)
{
	if (!Item.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	if (ColumnName == FPulseIssueColumns::Severity)
	{
		return SNew(STextBlock)
			.Text(FText::FromString(PulseSeverityToString(Item->Severity)))
			.ColorAndOpacity(PulseSeverityColor(Item->Severity))
			.Margin(FMargin(4.0f, 2.0f));
	}

	if (ColumnName == FPulseIssueColumns::Category)
	{
		return SNew(STextBlock)
			.Text(FText::FromName(Item->Category))
			.Margin(FMargin(4.0f, 2.0f));
	}

	if (ColumnName == FPulseIssueColumns::Asset)
	{
		// Short name in the cell, full path in the tooltip: asset paths are far too long for a
		// column, but the full path is what the user needs when copying or searching.
		const FString FullPath = Item->AssetPath.ToString();
		FString ShortName = FullPath;
		int32 DotIndex = INDEX_NONE;
		if (FullPath.FindLastChar(TEXT('.'), DotIndex))
		{
			ShortName = FullPath.Mid(DotIndex + 1);
		}

		return SNew(STextBlock)
			.Text(FText::FromString(ShortName))
			.ToolTipText(FText::Format(
				LOCTEXT("AssetTooltip", "{0}\nAsset score: {1}\nDouble-click to show in the Content Browser."),
				FText::FromString(FullPath),
				FText::AsNumber(Item->AssetScore)))
			.Margin(FMargin(4.0f, 2.0f));
	}

	if (ColumnName == FPulseIssueColumns::Rule)
	{
		return SNew(STextBlock)
			.Text(FText::FromName(Item->RuleId))
			.ToolTipText(FText::Format(
				LOCTEXT("RuleTooltip", "Detected at tier: {0}"),
				FText::FromString(PulseTierToString(Item->DetectedAtTier))))
			.Margin(FMargin(4.0f, 2.0f));
	}

	if (ColumnName == FPulseIssueColumns::Message)
	{
		// The recommendation rides in the tooltip rather than its own column: it is a sentence or
		// two, which would crush every other column if given horizontal space.
		return SNew(STextBlock)
			.Text(FText::FromString(Item->Message))
			.ToolTipText(Item->Recommendation.IsEmpty()
				? FText::FromString(Item->Message)
				: FText::Format(
					LOCTEXT("MessageTooltip", "{0}\n\nRecommendation: {1}"),
					FText::FromString(Item->Message),
					FText::FromString(Item->Recommendation)))
			.Margin(FMargin(4.0f, 2.0f));
	}

	return SNullWidget::NullWidget;
}

#undef LOCTEXT_NAMESPACE
