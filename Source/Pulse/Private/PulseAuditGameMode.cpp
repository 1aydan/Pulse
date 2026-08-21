// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseAuditGameMode.h"

#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"

APulseAuditGameMode::APulseAuditGameMode()
{
	// A spectator pawn is a bare free-flying camera: no mesh, no physics body, no animation. That is
	// the whole point — anything else would tick alongside the content being measured.
	DefaultPawnClass = ASpectatorPawn::StaticClass();
	PlayerControllerClass = APlayerController::StaticClass();
	HUDClass = nullptr;

	// Nothing here needs to tick.
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// No start-of-match delay: measurement begins as soon as the world is up, and a warmup state
	// would leave the first subject measured under different conditions than the rest.
	bStartPlayersAsSpectators = true;
}
