// Copyright (c) 2026 Pulse contributors. MIT License.

#include "UI/SPulseHub.h"

#include "Misc/ConfigCacheIni.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateIconFinder.h"
#include "UI/SPulseAuditPanel.h"
#include "UI/SPulseMemoryAuditorPanel.h"
#include "UI/SPulseNiagaraAuditorPanel.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWidgetSwitcher.h"

#define LOCTEXT_NAMESPACE "PulseHub"

static const TCHAR* GPulseHubConfigSection = TEXT("Pulse.Hub");
static const TCHAR* GPulseHubConfigKey = TEXT("ActiveSection");

/**
 * The Niagara icon lives in the Niagara editor's own style set, not the app style, so it has to be
 * searched for. Resolved when the button paints rather than at construction, since NiagaraEditor
 * may register its style after this window first opens.
 */
static const FSlateBrush* PulseFindNiagaraIcon()
{
	const FSlateIcon Icon = FSlateIconFinder::FindIcon(TEXT("ClassIcon.NiagaraSystem"));
	return Icon.IsSet() ? Icon.GetIcon() : FAppStyle::GetBrush("ClassIcon.ParticleSystem");
}

void SPulseHub::Construct(const FArguments& InArgs)
{
	ActiveSection = InArgs._InitialSection.IsSet() ? InArgs._InitialSection.GetValue() : LoadLastSection();

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Left)
		.Padding(6.0f, 6.0f, 6.0f, 4.0f)
		[
			SNew(SSegmentedControl<EPulseHubSection>)
			.Value(this, &SPulseHub::GetActiveSection)
			.OnValueChanged(this, &SPulseHub::SetActiveSection)

			+ SSegmentedControl<EPulseHubSection>::Slot(EPulseHubSection::AssetAudit)
			.Text(LOCTEXT("AssetAudit", "Asset Audit"))
			.ToolTip(LOCTEXT("AssetAuditTooltip", "Audit project assets for performance issues and export deterministic CSV/JSON reports."))
			.Icon(FAppStyle::GetBrush("MaterialEditor.TogglePlatformStats.Tab"))

			+ SSegmentedControl<EPulseHubSection>::Slot(EPulseHubSection::NiagaraAuditor)
			.Text(LOCTEXT("NiagaraAuditor", "Niagara Auditor"))
			.ToolTip(LOCTEXT("NiagaraAuditorTooltip", "Spawn Niagara systems in a play session and measure their game-thread cost across quality levels and distances."))
			.Icon_Static(&PulseFindNiagaraIcon)

			+ SSegmentedControl<EPulseHubSection>::Slot(EPulseHubSection::MemoryAuditor)
			.Text(LOCTEXT("MemoryAuditor", "Memory Auditor"))
			.ToolTip(LOCTEXT("MemoryAuditorTooltip", "Rank maps, primary assets and always-loaded classes by the RAM and VRAM their hard references keep resident."))
			.Icon(FAppStyle::GetBrush("ContentBrowser.SizeMap"))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]

		// Order must match EPulseHubSection: the switcher index is the enum value.
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex(this, &SPulseHub::GetActiveIndex)

			+ SWidgetSwitcher::Slot()
			[
				SNew(SPulseAuditPanel)
			]

			+ SWidgetSwitcher::Slot()
			[
				SNew(SPulseNiagaraAuditorPanel)
			]

			+ SWidgetSwitcher::Slot()
			[
				SNew(SPulseMemoryAuditorPanel)
			]
		]
	];
}

void SPulseHub::SetActiveSection(EPulseHubSection Section)
{
	ActiveSection = Section;
	SaveLastSection(Section);
}

EPulseHubSection SPulseHub::LoadLastSection()
{
	int32 Stored = 0;
	if (GConfig != nullptr)
	{
		GConfig->GetInt(GPulseHubConfigSection, GPulseHubConfigKey, Stored, GEditorPerProjectIni);
	}

	// A value written by a later version with more sections must not index past the switcher.
	return Stored >= 0 && Stored <= static_cast<int32>(EPulseHubSection::MemoryAuditor)
		? static_cast<EPulseHubSection>(Stored)
		: EPulseHubSection::AssetAudit;
}

void SPulseHub::SaveLastSection(EPulseHubSection Section)
{
	if (GConfig != nullptr)
	{
		GConfig->SetInt(GPulseHubConfigSection, GPulseHubConfigKey, static_cast<int32>(Section), GEditorPerProjectIni);
	}
}

#undef LOCTEXT_NAMESPACE
