// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "BlueprintAutoLayoutDefaults.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

namespace K2AutoLayout
{
struct BLUEPRINTAUTOLAYOUT_API FAutoLayoutSettings
{
    float NodeSpacingX = BlueprintAutoLayout::Defaults::DefaultNodeSpacingX;
    float NodeSpacingYExec = BlueprintAutoLayout::Defaults::DefaultNodeSpacingYExec;
    float NodeSpacingYData = BlueprintAutoLayout::Defaults::DefaultNodeSpacingYData;
    int32 VariableGetMinLength = BlueprintAutoLayout::Defaults::DefaultVariableGetMinLength;
    EBlueprintAutoLayoutRankAlignment RankAlignment = BlueprintAutoLayout::Defaults::DefaultRankAlignment;
};

struct BLUEPRINTAUTOLAYOUT_API FAutoLayoutResult
{
    bool bSuccess = false;
    FString Error;
    FString Guidance;
    int32 NodesLaidOut = 0;
    int32 ComponentsLaidOut = 0;
};

BLUEPRINTAUTOLAYOUT_API bool AutoLayoutIslands(UBlueprint *Blueprint, UEdGraph *Graph,
                                               const TArray<UEdGraphNode *> &StartNodes,
                                               const FAutoLayoutSettings &Settings, FAutoLayoutResult &OutResult);
} // namespace K2AutoLayout
