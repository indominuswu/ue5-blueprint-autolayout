// Copyright Epic Games, Inc. All Rights Reserved.

// Ensure the header is included only once.
#pragma once

// Core UE types for editor utilities.
#include "CoreMinimal.h"

// Default values for layout settings.
#include "BlueprintAutoLayoutDefaults.h"

// Forward declarations for Blueprint editor types.
class UBlueprint;
class UEdGraph;
class UEdGraphNode;

// Blueprint graph auto-layout API.
namespace K2AutoLayout
{
struct BLUEPRINTAUTOLAYOUT_API FAutoLayoutSettings
{
    // Legacy horizontal spacing used when exec/data spacing are untouched.
    float NodeSpacingX = BlueprintAutoLayout::Defaults::DefaultNodeSpacingX;

    // Per-type horizontal spacing controls.
    float NodeSpacingXExec = BlueprintAutoLayout::Defaults::DefaultNodeSpacingXExec;
    float NodeSpacingXData = BlueprintAutoLayout::Defaults::DefaultNodeSpacingXData;

    // Per-type vertical spacing controls.
    float NodeSpacingYExec = BlueprintAutoLayout::Defaults::DefaultNodeSpacingYExec;
    float NodeSpacingYData = BlueprintAutoLayout::Defaults::DefaultNodeSpacingYData;

    // Placement tuning parameters.
    int32 VariableGetMinLength =
        BlueprintAutoLayout::Defaults::DefaultVariableGetMinLength;
    EBlueprintAutoLayoutRankAlignment RankAlignment =
        BlueprintAutoLayout::Defaults::DefaultRankAlignment;
};

// Result payload for auto layout execution.
struct BLUEPRINTAUTOLAYOUT_API FAutoLayoutResult
{
    bool bSuccess = false;
    FString Error;
    FString Guidance;
    int32 NodesLaidOut = 0;
    int32 ComponentsLaidOut = 0;
};

// Auto layout islands in the provided graph, seeded by the selected nodes.
BLUEPRINTAUTOLAYOUT_API bool AutoLayoutIslands(UBlueprint *Blueprint, UEdGraph *Graph,
                                               const TArray<UEdGraphNode *> &StartNodes,
                                               const FAutoLayoutSettings &Settings,
                                               FAutoLayoutResult &OutResult);
} // namespace K2AutoLayout
