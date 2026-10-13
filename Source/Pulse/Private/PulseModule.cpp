// Copyright (c) 2026 Pulse contributors. MIT License.

#include "Pulse.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "PulseCollector.h"
#include "PulseCollectorRegistry.h"
#include "Styling/AppStyle.h"
#include "UI/SPulseHub.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "Pulse"

DEFINE_LOG_CATEGORY(LogPulse);

/** The one Pulse window. */
static const FName GPulseHubTabName(TEXT("Pulse"));

// The per-tool tab ids from before the tools shared a window. Still registered, hidden from menus, so
// an editor layout saved with one of them open restores as the Pulse window on that tool rather than
// silently dropping the tab.
static const FName GPulseLegacyAuditTabName(TEXT("PulseAudit"));
static const FName GPulseLegacyNiagaraAuditorTabName(TEXT("PulseNiagaraAuditor"));
static const FName GPulseLegacyMemoryAuditorTabName(TEXT("PulseMemoryAuditor"));

static TSharedRef<SDockTab> SpawnPulseHubTab(const FSpawnTabArgs& Args, TOptional<EPulseHubSection> InitialSection)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(LOCTEXT("HubTabLabel", "Pulse"))
		[
			SNew(SPulseHub)
			.InitialSection(InitialSection)
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

	// Tab registration comes after the collectors: the audit panel queries the registry as soon as a
	// user presses Run, and a tab that could spawn before its collectors exist would audit nothing.
	// Registered straight under Tools: every tool lives in this one window, so a "Pulse" submenu
	// holding a single "Pulse" entry would only add a click.
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(GPulseHubTabName,
		FOnSpawnTab::CreateStatic(&SpawnPulseHubTab, TOptional<EPulseHubSection>()))
		.SetDisplayName(LOCTEXT("HubTabTitle", "Pulse"))
		.SetTooltipText(LOCTEXT("HubTabTooltip", "Pulse tools: the asset audit, the Niagara cost auditor and the memory auditor, in one window."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "MaterialEditor.TogglePlatformStats.Tab"));

	const TPair<FName, EPulseHubSection> LegacyTabs[] = {
		{ GPulseLegacyAuditTabName, EPulseHubSection::AssetAudit },
		{ GPulseLegacyNiagaraAuditorTabName, EPulseHubSection::NiagaraAuditor },
		{ GPulseLegacyMemoryAuditorTabName, EPulseHubSection::MemoryAuditor },
	};
	for (const TPair<FName, EPulseHubSection>& LegacyTab : LegacyTabs)
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(LegacyTab.Key,
			FOnSpawnTab::CreateStatic(&SpawnPulseHubTab, TOptional<EPulseHubSection>(LegacyTab.Value)))
			.SetDisplayName(LOCTEXT("HubTabTitle", "Pulse"))
			.SetMenuType(ETabSpawnerMenuType::Hidden);
	}
}

void FPulseModule::ShutdownModule()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GPulseHubTabName);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GPulseLegacyAuditTabName);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GPulseLegacyNiagaraAuditorTabName);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GPulseLegacyMemoryAuditorTabName);
	}

	for (const TUniquePtr<IPulseCollector>& Collector : OwnedCollectors)
	{
		FPulseCollectorRegistry::Unregister(Collector.Get());
	}
	OwnedCollectors.Empty();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPulseModule, Pulse)
