// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Graph/GraphLayoutWorkTypes.h"

namespace GraphLayout
{
struct FGlobalPlacement
{
    TMap<int32, FVector2f> Positions;
    int32 AnchorNodeIndex = INDEX_NONE;
};

FGlobalPlacement PlaceGlobalRankOrder(const TArray<FWorkNode> &Nodes, float NodeSpacingX, float NodeSpacingY);
FVector2f ComputeGlobalAnchorOffset(const TArray<FWorkNode> &Nodes, const FGlobalPlacement &Placement);
} // namespace GraphLayout
