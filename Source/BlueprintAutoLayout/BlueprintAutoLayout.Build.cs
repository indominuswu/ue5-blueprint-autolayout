// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class BlueprintAutoLayout : ModuleRules
{
  public BlueprintAutoLayout(ReadOnlyTargetRules Target) : base(Target)
  {
    PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

    PublicDependencyModuleNames.AddRange(
      new[]
      {
        "Core",
        "CoreUObject",
        "DeveloperSettings",
        "Engine"
      });

    PrivateDependencyModuleNames.AddRange(
      new[]
      {
        "ApplicationCore",
        "BlueprintGraph",
        "EditorSubsystem",
        "GraphEditor",
        "InputCore",
        "Kismet",
        "Slate",
        "SlateCore",
        "ToolMenus",
        "UnrealEd"
      });
  }
}
