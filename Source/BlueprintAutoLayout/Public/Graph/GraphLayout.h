// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "BlueprintAutoLayoutDefaults.h"

namespace GraphLayout
{
enum class EEdgeKind : uint8
{
    Exec,
    Data
};

struct BLUEPRINTAUTOLAYOUT_API FNodeKey
{
    FGuid Guid;
};

struct BLUEPRINTAUTOLAYOUT_API FLayoutNode
{
    int32 Id = 0;
    FNodeKey Key;
    FString Name;
    FVector2f Size = FVector2f::ZeroVector;
    bool bHasExecPins = false;
    int32 ExecInputPinCount = 0;
    int32 ExecOutputPinCount = 0;
    int32 InputPinCount = 0;
    int32 OutputPinCount = 0;
    FVector2f Position = FVector2f::ZeroVector; // Input: original top-left for anchoring.
};

struct BLUEPRINTAUTOLAYOUT_API FLayoutEdge
{
    int32 Src = INDEX_NONE;
    int32 Dst = INDEX_NONE;
    int32 SrcPinIndex = INDEX_NONE;
    int32 DstPinIndex = INDEX_NONE;
    FName SrcPinName;
    FName DstPinName;
    EEdgeKind Kind = EEdgeKind::Data;
    FString StableKey;
};

struct BLUEPRINTAUTOLAYOUT_API FLayoutGraph
{
    TArray<FLayoutNode> Nodes;
    TArray<FLayoutEdge> Edges;
};

struct BLUEPRINTAUTOLAYOUT_API FLayoutSettings
{
    float NodeSpacingX = BlueprintAutoLayout::Defaults::DefaultNodeSpacingX;
    float NodeSpacingY = BlueprintAutoLayout::Defaults::DefaultNodeSpacingY;
};

struct BLUEPRINTAUTOLAYOUT_API FLayoutComponentResult
{
    TMap<int32, FVector2f> NodePositions;
    FBox2f Bounds = FBox2f(EForceInit::ForceInit);
};

BLUEPRINTAUTOLAYOUT_API bool LayoutComponent(const FLayoutGraph &Graph, const TArray<int32> &ComponentNodeIds,
                                             const FLayoutSettings &Settings, FLayoutComponentResult &OutResult,
                                             FString *OutError = nullptr);
} // namespace GraphLayout
