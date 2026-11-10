using UnrealBuildTool;

public class Huebox : ModuleRules
{
	public Huebox(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"PhysicsCore",
			"DeveloperSettings"
		});

		// HUD only: GWhiteTexture for the additive light diagram (canvas triangles with a blend mode).
		PrivateDependencyModuleNames.Add("RenderCore");
		// SViewport: the player controller hands the game viewport keyboard focus.
		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		// Shadow puppets: flat leather pieces built at runtime from generated geometry (Rig/HueShadowPuppet).
		PrivateDependencyModuleNames.Add("ProceduralMeshComponent");

		// Sub-folders are include roots, e.g. #include "Core/HueTypes.h".
		PublicIncludePaths.Add(ModuleDirectory);
	}
}
