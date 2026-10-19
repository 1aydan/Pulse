// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseContentCleanupSettings.h"

UPulseContentCleanupSettings::UPulseContentCleanupSettings()
{
	FDirectoryPath GameRoot;
	GameRoot.Path = TEXT("/Game");
	ScanPaths.Add(GameRoot);

	ExcludePathPatterns.Add(TEXT("/Developers/"));
	ExcludePathPatterns.Add(TEXT("/Collections/"));
	// Owned by their map; they are unused exactly when the map is, and the map is the row to act on.
	ExcludePathPatterns.Add(TEXT("/__ExternalActors__/"));
	ExcludePathPatterns.Add(TEXT("/__ExternalObjects__/"));

	IgnoredClassNames.Add(TEXT("EditorUtilityBlueprint"));
	IgnoredClassNames.Add(TEXT("EditorUtilityWidgetBlueprint"));
	IgnoredClassNames.Add(TEXT("EditorTutorial"));
	IgnoredClassNames.Add(TEXT("PrimaryAssetLabel"));
}
