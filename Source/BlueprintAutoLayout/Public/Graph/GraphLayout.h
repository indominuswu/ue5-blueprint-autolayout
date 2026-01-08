// Copyright Epic Games, Inc. All Rights Reserved.

// Ensure the header is included only once.
#pragma once

// Core UE types for layout data.
#include "CoreMinimal.h"

// Default spacing and alignment values.
#include "BlueprintAutoLayoutDefaults.h"

// Graph layout types and APIs.
namespace GraphLayout
{
// Edge categories used to differentiate exec vs data flow.
enum class EEdgeKind : uint8
{
    Exec,
    Data
};

// Stable key used to identify nodes across layout passes.
struct BLUEPRINTAUTOLAYOUT_API FNodeKey
{
    FGuid Guid;
};

// Node metadata needed for layout decisions.
struct BLUEPRINTAUTOLAYOUT_API FLayoutNode
{
    // Input fields describing the original graph node.
    int32 Id = 0;
    FNodeKey Key;
    FString Name;
    FVector2f Size = FVector2f::ZeroVector;
    bool bHasExecPins = false;
    bool bIsVariableGet = false;
    bool bIsReroute = false;
    int32 ExecInputPinCount = 0;
    int32 ExecOutputPinCount = 0;
    int32 InputPinCount = 0;
    int32 OutputPinCount = 0;
    FVector2f Position = FVector2f::ZeroVector; // Original top-left for anchoring.

    // Working layout outputs populated by the layout pipeline.
    int32 GlobalRank = 0;
    int32 GlobalOrder = 0;
};

// Edge metadata used to build the layout graph.
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

// Input graph container for layout.
struct BLUEPRINTAUTOLAYOUT_API FLayoutGraph
{
    TArray<FLayoutNode> Nodes;
    TArray<FLayoutEdge> Edges;
};

// Settings that control spacing and placement behavior.
struct BLUEPRINTAUTOLAYOUT_API FLayoutSettings
{
    // Legacy horizontal spacing used when exec/data values remain default.
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
    bool bAlignExecChainsHorizontally =
        BlueprintAutoLayout::Defaults::DefaultAlignExecChainsHorizontally;
};

// Result payload for a single connected component layout.
struct BLUEPRINTAUTOLAYOUT_API FLayoutComponentResult
{
    TMap<int32, FVector2f> NodePositions;
    FBox2f Bounds = FBox2f(EForceInit::ForceInit);
};

// Run layout for a connected component and emit node positions.
BLUEPRINTAUTOLAYOUT_API bool LayoutComponent(const FLayoutGraph &Graph,
                                             const TArray<int32> &ComponentNodeIds,
                                             const FLayoutSettings &Settings,
                                             FLayoutComponentResult &OutResult,
                                             FString *OutError = nullptr);
} // namespace GraphLayout
