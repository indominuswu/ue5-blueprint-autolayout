// Copyright Epic Games, Inc. All Rights Reserved.

// Placement interface definitions.
#include "Graph/GraphLayoutPlacement.h"

// Logging and key utilities for deterministic placement.
#include "BlueprintAutoLayoutLog.h"
#include "Graph/GraphLayoutKeyUtils.h"

// Graph layout placement implementation.
namespace GraphLayout
{
namespace
{
// Provide a stable key ordering for node tie-breaks.
bool NodeKeyLess(const FNodeKey &A, const FNodeKey &B)
{
    return KeyUtils::NodeKeyLess(A, B);
}
} // namespace

// Place nodes by rank order using basic stacking and alignment.
FGlobalPlacement PlaceGlobalRankOrder(const TArray<FWorkNode> &Nodes,
                                      float NodeSpacingXExec, float NodeSpacingXData,
                                      float NodeSpacingYExec, float NodeSpacingYData,
                                      EBlueprintAutoLayoutRankAlignment RankAlignment)
{
    // Initialize the result and early-out when there is nothing to place.
    FGlobalPlacement Result;
    if (Nodes.IsEmpty()) {
        return Result;
    }

    // Clamp spacing inputs to non-negative values.
    NodeSpacingXExec = FMath::Max(0.0f, NodeSpacingXExec);
    NodeSpacingXData = FMath::Max(0.0f, NodeSpacingXData);
    NodeSpacingYExec = FMath::Max(0.0f, NodeSpacingYExec);
    NodeSpacingYData = FMath::Max(0.0f, NodeSpacingYData);

    // Scan nodes to find the maximum rank used for layout sizing.
    int32 MaxRank = 0;
    for (const FWorkNode &Node : Nodes) {
        const int32 Rank = FMath::Max(0, Node.GlobalRank);
        MaxRank = FMath::Max(MaxRank, Rank);
    }

    // Compute per-rank widths and spacing based on node types.
    TArray<float> RankWidth;
    TArray<float> RankSpacingX;
    RankWidth.Init(0.0f, MaxRank + 1);
    RankSpacingX.Init(0.0f, MaxRank + 1);
    for (const FWorkNode &Node : Nodes) {
        const int32 Rank = FMath::Max(0, Node.GlobalRank);
        RankWidth[Rank] = FMath::Max(RankWidth[Rank], Node.Size.X);
        const float SpacingX = Node.bHasExecPins ? NodeSpacingXExec : NodeSpacingXData;
        RankSpacingX[Rank] = FMath::Max(RankSpacingX[Rank], SpacingX);
    }

    // Fill empty ranks with a default spacing to keep columns separated.
    const float DefaultSpacingX = FMath::Max(NodeSpacingXExec, NodeSpacingXData);
    for (int32 Rank = 0; Rank < RankSpacingX.Num(); ++Rank) {
        if (RankSpacingX[Rank] <= KINDA_SMALL_NUMBER) {
            RankSpacingX[Rank] = DefaultSpacingX;
        }
    }

    // Convert per-rank widths into left-edge offsets with spacing applied.
    TArray<float> RankXLeft;
    RankXLeft.Init(0.0f, MaxRank + 1);
    float XOffset = 0.0f;
    for (int32 Rank = 0; Rank < RankXLeft.Num(); ++Rank) {
        RankXLeft[Rank] = XOffset;
        XOffset += RankWidth[Rank] + RankSpacingX[Rank];
    }

    // Group node indices by their rank for per-layer ordering.
    TArray<TArray<int32>> RankNodes;
    RankNodes.SetNum(MaxRank + 1);
    for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
        const int32 Rank = FMath::Max(0, Nodes[Index].GlobalRank);
        RankNodes[Rank].Add(Index);
    }

    // Lay out each rank using per-node vertical spacing.
    for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
        // Sort within a rank by explicit order, then by stable key.
        TArray<int32> &Layer = RankNodes[Rank];
        Layer.Sort([&](int32 A, int32 B) {
            const FWorkNode &NodeA = Nodes[A];
            const FWorkNode &NodeB = Nodes[B];
            if (NodeA.GlobalOrder != NodeB.GlobalOrder) {
                return NodeA.GlobalOrder < NodeB.GlobalOrder;
            }
            return NodeKeyLess(NodeA.Key, NodeB.Key);
        });

        // Position nodes within their rank column and stacked by order.
        auto GetSpacingY = [&](const FWorkNode &Node) {
            return Node.bHasExecPins ? NodeSpacingYExec : NodeSpacingYData;
        };
        auto GetAlignedOffset = [&](float ColumnWidth, float NodeWidth) {
            const float Extra = FMath::Max(0.0f, ColumnWidth - NodeWidth);
            switch (RankAlignment) {
            case EBlueprintAutoLayoutRankAlignment::Left:
                return 0.0f;
            case EBlueprintAutoLayoutRankAlignment::Right:
                return Extra;
            case EBlueprintAutoLayoutRankAlignment::Center:
            default:
                return Extra * 0.5f;
            }
        };

        float YOffset = 0.0f;
        for (int32 LayerOrder = 0; LayerOrder < Layer.Num(); ++LayerOrder) {
            const int32 Index = Layer[LayerOrder];
            const FWorkNode &Node = Nodes[Index];
            const int32 Order = LayerOrder;
            const float X =
                RankXLeft[Rank] + GetAlignedOffset(RankWidth[Rank], Node.Size.X);
            const float Y = YOffset;
            const TCHAR *NodeName =
                Node.Name.IsEmpty() ? TEXT("<unnamed>") : *Node.Name;
            const FString GuidString =
                Node.Key.Guid.ToString(EGuidFormats::DigitsWithHyphens);
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("  Placing node guid=%s name=%s rank=%d order=%d "
                        "original_order=%d at (%.1f, %.1f)"),
                   *GuidString, NodeName, Node.GlobalRank, Order, Node.GlobalOrder, X,
                   Y);
            Result.Positions.Add(Index, FVector2f(X, Y));
            YOffset += Node.Size.Y + GetSpacingY(Node);
        }
    }

    // Prefer an anchor with exec pins, then stable key, then index order.
    auto IsBetterAnchor = [&](int32 Candidate, int32 Current) {
        if (Candidate == INDEX_NONE) {
            return false;
        }
        if (Current == INDEX_NONE) {
            return true;
        }
        const bool bCandidateExec = Nodes[Candidate].bHasExecPins;
        const bool bCurrentExec = Nodes[Current].bHasExecPins;
        if (bCandidateExec != bCurrentExec) {
            return bCandidateExec;
        }
        if (NodeKeyLess(Nodes[Candidate].Key, Nodes[Current].Key)) {
            return true;
        }
        if (NodeKeyLess(Nodes[Current].Key, Nodes[Candidate].Key)) {
            return false;
        }
        return Candidate < Current;
    };

    // First pass: anchor at rank 0, order 0 when possible.
    int32 AnchorIndex = INDEX_NONE;
    for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
        const FWorkNode &Node = Nodes[Index];
        if (Node.GlobalRank == 0 && Node.GlobalOrder == 0) {
            if (IsBetterAnchor(Index, AnchorIndex)) {
                AnchorIndex = Index;
            }
        }
    }

    // Fallback: choose the best available node if no ideal anchor exists.
    if (AnchorIndex == INDEX_NONE) {
        for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
            if (IsBetterAnchor(Index, AnchorIndex)) {
                AnchorIndex = Index;
            }
        }
    }

    // Record the anchor selection for consumers that need a reference origin.
    Result.AnchorNodeIndex = AnchorIndex;
    return Result;
}

// Compute the offset that keeps the selected anchor aligned to its original position.
FVector2f ComputeGlobalAnchorOffset(const TArray<FWorkNode> &Nodes,
                                    const FGlobalPlacement &Placement)
{
    // When no anchor was chosen, keep the layout origin unchanged.
    if (Placement.AnchorNodeIndex == INDEX_NONE) {
        return FVector2f::ZeroVector;
    }
    // Require a stored position for the anchor in the placement map.
    const FVector2f *AnchorPos = Placement.Positions.Find(Placement.AnchorNodeIndex);
    if (!AnchorPos) {
        return FVector2f::ZeroVector;
    }
    // Ensure the anchor index is valid for the provided node array.
    if (!Nodes.IsValidIndex(Placement.AnchorNodeIndex)) {
        return FVector2f::ZeroVector;
    }
    // Offset so the anchor aligns with its original position.
    return Nodes[Placement.AnchorNodeIndex].OriginalPosition - *AnchorPos;
}
} // namespace GraphLayout
