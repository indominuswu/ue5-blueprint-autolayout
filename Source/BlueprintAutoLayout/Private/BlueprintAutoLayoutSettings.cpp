// Copyright Epic Games, Inc. All Rights Reserved.

#include "BlueprintAutoLayoutSettings.h"

#include "K2/K2AutoLayout.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(BlueprintAutoLayoutSettings)

FName UBlueprintAutoLayoutSettings::GetCategoryName() const
{
    return TEXT("Plugins");
}

K2AutoLayout::FAutoLayoutSettings UBlueprintAutoLayoutSettings::ToAutoLayoutSettings() const
{
    K2AutoLayout::FAutoLayoutSettings Settings;
    Settings.NodeSpacingX = NodeSpacingX;
    Settings.NodeSpacingYExec = NodeSpacingYExec;
    Settings.NodeSpacingYData = NodeSpacingYData;
    return Settings;
}
