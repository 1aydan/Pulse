// Copyright (c) 2026 Pulse contributors. MIT License.

using UnrealBuildTool;

public class Pulse : ModuleRules
{
	public Pulse(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"AssetRegistry",
			"ContentBrowser",
			"DeveloperSettings",
			"DeveloperToolSettings",
			"EngineSettings",
			"InputCore",
			"Json",
			"LevelEditor",
			"MaterialEditor",
			"Niagara",
			"PhysicsCore",
			"Projects",
			"RHI",
			"RenderCore",
			"Settings",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"WorkspaceMenuStructure"
		});
	}
}
