// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseSceneAuditorSettings.h"

UPulseSceneAuditorSettings::UPulseSceneAuditorSettings()
{
	FDirectoryPath GameRoot;
	GameRoot.Path = TEXT("/Game");
	ScanPaths.Add(GameRoot);

	ExcludePathPatterns.Add(TEXT("/Developers/"));
	ExcludePathPatterns.Add(TEXT("/Collections/"));
}
