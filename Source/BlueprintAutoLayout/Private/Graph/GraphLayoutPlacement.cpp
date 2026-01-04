// Copyright Epic Games, Inc. All Rights Reserved.

#include "Graph/GraphLayoutPlacement.h"

#include "Graph/GraphLayoutKeyUtils.h"

namespace GraphLayout
{
namespace
{
bool NodeKeyLess(const FNodeKey &A, const FNodeKey &B)
{
    return KeyUtils::NodeKeyLess(A, B);
}
} // namespace

FGlobalPlacement PlaceGlobalRankOrder(const TArray<FWorkNode> &Nodes, float NodeSpacingX, float NodeSpacingY)
{
    FGlobalPlacement Result;
    if (Nodes.IsEmpty()) {
        return Result;
    }

    int32 MaxRank = 0;
    float MaxHeight = 0.0f;
    for (const FWorkNode &Node : Nodes) {
        const int32 Rank = FMath::Max(0, Node.GlobalRank);
        MaxRank = FMath::Max(MaxRank, Rank);
        MaxHeight = FMath::Max(MaxHeight, Node.Size.Y);
    }

    TArray<float> RankWidth;
    RankWidth.Init(0.0f, MaxRank + 1);
    for (const FWorkNode &Node : Nodes) {
        const int32 Rank = FMath::Max(0, Node.GlobalRank);
        RankWidth[Rank] = FMath::Max(RankWidth[Rank], Node.Size.X);
    }

    TArray<float> RankXLeft;
    RankXLeft.Init(0.0f, MaxRank + 1);
    float XOffset = 0.0f;
    for (int32 Rank = 0; Rank < RankXLeft.Num(); ++Rank) {
        RankXLeft[Rank] = XOffset;
        XOffset += RankWidth[Rank] + NodeSpacingX;
    }

    TArray<TArray<int32>> RankNodes;
    RankNodes.SetNum(MaxRank + 1);
    for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
        const int32 Rank = FMath::Max(0, Nodes[Index].GlobalRank);
        RankNodes[Rank].Add(Index);
    }

    const float RowStride = MaxHeight + NodeSpacingY;
    for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
        TArray<int32> &Layer = RankNodes[Rank];
        Layer.Sort([&](int32 A, int32 B) {
            const FWorkNode &NodeA = Nodes[A];
            const FWorkNode &NodeB = Nodes[B];
            if (NodeA.GlobalOrder != NodeB.GlobalOrder) {
                return NodeA.GlobalOrder < NodeB.GlobalOrder;
            }
            return NodeKeyLess(NodeA.Key, NodeB.Key);
        });

        for (int32 Index : Layer) {
            const FWorkNode &Node = Nodes[Index];
            const int32 Order = FMath::Max(0, Node.GlobalOrder);
            const float X = RankXLeft[Rank] + (RankWidth[Rank] - Node.Size.X) * 0.5f;
            const float Y = RowStride > 0.0f ? Order * RowStride : 0.0f;
            Result.Positions.Add(Index, FVector2f(X, Y));
        }
    }

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

    int32 AnchorIndex = INDEX_NONE;
    for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
        const FWorkNode &Node = Nodes[Index];
        if (Node.GlobalRank == 0 && Node.GlobalOrder == 0) {
            if (IsBetterAnchor(Index, AnchorIndex)) {
                AnchorIndex = Index;
            }
        }
    }

    if (AnchorIndex == INDEX_NONE) {
        for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
            if (IsBetterAnchor(Index, AnchorIndex)) {
                AnchorIndex = Index;
            }
        }
    }

    Result.AnchorNodeIndex = AnchorIndex;
    return Result;
}

FVector2f ComputeGlobalAnchorOffset(const TArray<FWorkNode> &Nodes, const FGlobalPlacement &Placement)
{
    if (Placement.AnchorNodeIndex == INDEX_NONE) {
        return FVector2f::ZeroVector;
    }
    const FVector2f *AnchorPos = Placement.Positions.Find(Placement.AnchorNodeIndex);
    if (!AnchorPos) {
        return FVector2f::ZeroVector;
    }
    if (!Nodes.IsValidIndex(Placement.AnchorNodeIndex)) {
        return FVector2f::ZeroVector;
    }
    return Nodes[Placement.AnchorNodeIndex].OriginalPosition - *AnchorPos;
}
} // namespace GraphLayout
