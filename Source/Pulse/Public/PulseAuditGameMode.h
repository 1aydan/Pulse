// Copyright (c) 2026 Pulse contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "PulseAuditGameMode.generated.h"

/**
 * Minimal game mode for any Pulse measurement map: spectator pawn, no HUD, no gameplay.
 *
 * Deliberately not tied to one auditor. Anything Pulse measures by spawning content into a play
 * session wants the same empty stage, so this is shared infrastructure rather than a piece of the
 * Niagara tool — the same job APSOForgeGameMode does for PSO capture.
 *
 * It matters because a project's default game mode is not free. It spawns a character with physics
 * and animation ticking, usually a HUD, often gameplay subsystems — all of it competing for the
 * game thread a measurement is trying to attribute. Without this the numbers carry whatever your
 * game does on an empty level.
 *
 * Assign it as the GameMode Override in the measurement map's World Settings. It cannot be forced
 * programmatically: FRequestPlaySessionParams has no game-mode field, and writing DefaultGameMode
 * onto the editor world before starting PIE would dirty the user's map.
 *
 * Not required. The Niagara auditor forces the player controller into spectator state at PIE start
 * regardless, so a map without this still measures — it just also runs whatever else your game mode
 * starts.
 *
 * EDITOR ONLY. Pulse is an Editor-type module, so this class does not exist in a cooked build. A map
 * referencing it is a development map and must not be shipped: cooking one would leave the World
 * Settings reference unresolvable. That is the right constraint anyway — a measurement stage has no
 * business in a package.
 */
UCLASS()
class PULSE_API APulseAuditGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	APulseAuditGameMode();
};
