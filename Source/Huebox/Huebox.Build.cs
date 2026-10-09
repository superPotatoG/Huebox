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
			"PhysicsCore"
		});

		// Sub-folders are include roots, e.g. #include "Core/HueTypes.h".
		PublicIncludePaths.Add(ModuleDirectory);
	}
}
