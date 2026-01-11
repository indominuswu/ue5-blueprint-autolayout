// Copyright Epic Games, Inc. All Rights Reserved.

// Settings definition.
#include "BlueprintAutoLayoutSettings.h"

// Runtime layout settings translation.
#include "K2/K2AutoLayout.h"

// Generated body include.
#include UE_INLINE_GENERATED_CPP_BY_NAME(BlueprintAutoLayoutSettings)

// Report the editor category used to display the settings.
FName UBlueprintAutoLayoutSettings::GetCategoryName() const
{
    // Place settings under the Plugins category.
    return TEXT("Plugins");
}

// Translate editor-configured settings into runtime layout settings.
K2AutoLayout::FAutoLayoutSettings UBlueprintAutoLayoutSettings::ToAutoLayoutSettings()
    const
{
    // Populate runtime settings from editor-configured values.
    K2AutoLayout::FAutoLayoutSettings Settings;
    Settings.NodeSpacingX = NodeSpacingX;
    Settings.NodeSpacingXExec = NodeSpacingXExec;
    Settings.NodeSpacingXData = NodeSpacingXData;
    Settings.NodeSpacingYExec = NodeSpacingYExec;
    Settings.NodeSpacingYData = NodeSpacingYData;
    Settings.VariableGetMinLength = bPlaceVariableGetUnderDestination ? 0 : 1;
    Settings.RankAlignment = RankAlignment;
    Settings.bAlignExecChainsHorizontally = bAlignExecChainsHorizontally;

    // Apply legacy NodeSpacingX when exec/data spacing are still default.
    const bool bExecDefault =
        FMath::IsNearlyEqual(Settings.NodeSpacingXExec,
                             BlueprintAutoLayout::Defaults::DefaultNodeSpacingXExec);
    const bool bDataDefault =
        FMath::IsNearlyEqual(Settings.NodeSpacingXData,
                             BlueprintAutoLayout::Defaults::DefaultNodeSpacingXData);
    const bool bLegacyNonDefault = !FMath::IsNearlyEqual(
        Settings.NodeSpacingX, BlueprintAutoLayout::Defaults::DefaultNodeSpacingX);
    if (bExecDefault && bDataDefault && bLegacyNonDefault) {
        Settings.NodeSpacingXExec = Settings.NodeSpacingX;
        Settings.NodeSpacingXData = Settings.NodeSpacingX;
    }
    return Settings;
}
