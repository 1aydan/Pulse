// Copyright (c) 2026 Pulse contributors. MIT License.

#include "Pulse.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "PulseCollector.h"
#include "PulseCollectorRegistry.h"
#include "Styling/AppStyle.h"
#include "UI/SPulseAuditPanel.h"
#include "UI/SPulseNiagaraAuditorPanel.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "Pulse"

DEFINE_LOG_CATEGORY(LogPulse);

static const FName GPulseAuditTabName(TEXT("PulseAudit"));
static const FName GPulseNiagaraAuditorTabName(TEXT("PulseNiagaraAuditor"));

static TSharedRef<SDockTab> SpawnPulseAuditTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SPulseAuditPanel)
		];
}

static TSharedRef<SDockTab> SpawnPulseNiagaraAuditorTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SPulseNiagaraAuditorPanel)
		];
}

FPulseModule::~FPulseModule() = default;

void FPulseModule::StartupModule()
{
	// Built-in collectors registered themselves as factories during static init; instantiate and
	// publish them now, once FName and the module system are fully up.
	for (const TFunction<TUniquePtr<IPulseCollector>()>& Factory : FPulseCollectorRegistry::GetBuiltInFactories())
	{
		TUniquePtr<IPulseCollector> Collector = Factory();
		FPulseCollectorRegistry::Register(Collector.Get());
		OwnedCollectors.Add(MoveTemp(Collector));
	}

	UE_LOG(LogPulse, Log, TEXT("Pulse module loaded with %d built-in collectors."), OwnedCollectors.Num());

	// Tab registration comes after the collectors: the panel queries the registry as soon as a user
	// presses Run, and a tab that could spawn before its collectors exist would audit nothing.
	MenuGroup = WorkspaceMenu::GetMenuStructure().GetToolsCategory()->AddGroup(
		FName(TEXT("Pulse")),
		LOCTEXT("MenuGroup", "Pulse"),
		LOCTEXT("MenuGroupTooltip", "Read-only asset performance auditing"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "MaterialEditor.TogglePlatformStats.Tab"));

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(GPulseAuditTabName,
		FOnSpawnTab::CreateStatic(&SpawnPulseAuditTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Pulse Audit"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Audit project assets for performance issues and export deterministic CSV/JSON reports."))
		.SetGroup(MenuGroup.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "MaterialEditor.TogglePlatformStats.Tab"));

	// Separate tab, not a mode of the audit panel: this one spawns and ticks content in the user's
	// open level, which is a different kind of act from a read-only scan and should look like one.
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(GPulseNiagaraAuditorTabName,
		FOnSpawnTab::CreateStatic(&SpawnPulseNiagaraAuditorTab))
		.SetDisplayName(LOCTEXT("NiagaraTabTitle", "Pulse Niagara Auditor"))
		.SetTooltipText(LOCTEXT("NiagaraTabTooltip", "Spawn Niagara systems in the current level and measure their game-thread cost across quality levels and distances."))
		.SetGroup(MenuGroup.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.NiagaraSystem"));
}

void FPulseModule::ShutdownModule()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GPulseAuditTabName);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GPulseNiagaraAuditorTabName);
	}
	if (MenuGroup.IsValid())
	{
		WorkspaceMenu::GetMenuStructure().GetToolsCategory()->RemoveItem(MenuGroup.ToSharedRef());
		MenuGroup.Reset();
	}

	for (const TUniquePtr<IPulseCollector>& Collector : OwnedCollectors)
	{
		FPulseCollectorRegistry::Unregister(Collector.Get());
	}
	OwnedCollectors.Empty();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPulseModule, Pulse)
