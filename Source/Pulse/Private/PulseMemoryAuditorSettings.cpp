// Copyright (c) 2026 Pulse contributors. MIT License.

#include "PulseMemoryAuditorSettings.h"

UPulseMemoryAuditorSettings::UPulseMemoryAuditorSettings()
{
	FDirectoryPath GameRoot;
	GameRoot.Path = TEXT("/Game");
	Scan.ScanPaths.Add(GameRoot);

	Scan.ExcludePathPatterns.Add(TEXT("/Developers/"));
	Scan.ExcludePathPatterns.Add(TEXT("/Collections/"));
}
