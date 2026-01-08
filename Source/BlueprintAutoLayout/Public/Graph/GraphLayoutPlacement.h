// Copyright Epic Games, Inc. All Rights Reserved.

// Ensure the header is included only once.
#pragma once

// Core UE types for placement math.
#include "CoreMinimal.h"

// Layout node and edge definitions for placement.
#include "Graph/GraphLayout.h"

// Placement utilities for layout passes.
namespace GraphLayout
{
// Output of a global placement pass.
struct FGlobalPlacement
{
    TMap<int32, FVector2f> Positions;
    int32 AnchorNodeIndex = INDEX_NONE;
};

// Place nodes by rank order using standard or compact strategies.
FGlobalPlacement PlaceGlobalRankOrder(const TArray<FLayoutNode> &Nodes,
                                      float NodeSpacingXExec, float NodeSpacingXData,
                                      float NodeSpacingYExec, float NodeSpacingYData,
                                      EBlueprintAutoLayoutRankAlignment RankAlignment);
FGlobalPlacement PlaceGlobalRankOrderCompact(
    const TArray<FLayoutNode> &Nodes, const TArray<FLayoutEdge> &Edges,
    float NodeSpacingXExec, float NodeSpacingXData, float NodeSpacingYExec,
    float NodeSpacingYData, bool bAlignExecChainsHorizontally,
    EBlueprintAutoLayoutRankAlignment RankAlignment);

// Compute the offset that aligns the chosen anchor node to its original position.
FVector2f ComputeGlobalAnchorOffset(const TArray<FLayoutNode> &Nodes,
                                    const FGlobalPlacement &Placement);
} // namespace GraphLayout
