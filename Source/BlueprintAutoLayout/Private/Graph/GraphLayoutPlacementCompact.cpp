// Copyright Epic Games, Inc. All Rights Reserved.

#include "Graph/GraphLayoutPlacement.h"

#include "BlueprintAutoLayoutLog.h"
#include "Graph/GraphLayoutKeyUtils.h"

namespace GraphLayout
{
namespace
{
// Provide a stable key ordering for node tie-breaks.
bool NodeKeyLess(const FNodeKey &A, const FNodeKey &B)
{
    return KeyUtils::NodeKeyLess(A, B);
}

struct FConstraint
{
    int32 Target = INDEX_NONE;
    int32 Source = INDEX_NONE;
    float Delta = 0.0f;
};

float GetApproxPinOffset(const FWorkNode &Node, int32 PinIndex, int32 PinCount)
{
    // Approximate pin location as a fraction of node height using pin index within the direction.
    const int32 Denom = FMath::Max(1, PinCount);
    const float Fraction = (static_cast<float>(PinIndex) + 0.5f) / static_cast<float>(Denom);
    return Node.Size.Y * FMath::Clamp(Fraction, 0.0f, 1.0f);
}
} // namespace

FGlobalPlacement PlaceGlobalRankOrderCompact(const TArray<FWorkNode> &Nodes, const TArray<FWorkEdge> &Edges,
                                             float NodeSpacingX, float NodeSpacingYExec, float NodeSpacingYData)
{
    // Initialize the result and early-out when there is nothing to place.
    FGlobalPlacement Result;
    if (Nodes.IsEmpty()) {
        return Result;
    }

    NodeSpacingX = FMath::Max(0.0f, NodeSpacingX);
    NodeSpacingYExec = FMath::Max(0.0f, NodeSpacingYExec);
    NodeSpacingYData = FMath::Max(0.0f, NodeSpacingYData);

    // Scan nodes to find the maximum rank used for layout sizing.
    int32 MaxRank = 0;
    for (const FWorkNode &Node : Nodes) {
        const int32 Rank = FMath::Max(0, Node.GlobalRank);
        MaxRank = FMath::Max(MaxRank, Rank);
    }

    // Compute the widest node per rank to establish per-column width.
    TArray<float> RankWidth;
    RankWidth.Init(0.0f, MaxRank + 1);
    for (const FWorkNode &Node : Nodes) {
        const int32 Rank = FMath::Max(0, Node.GlobalRank);
        RankWidth[Rank] = FMath::Max(RankWidth[Rank], Node.Size.X);
    }

    // Convert per-rank widths into left-edge offsets with spacing applied.
    TArray<float> RankXLeft;
    RankXLeft.Init(0.0f, MaxRank + 1);
    float XOffset = 0.0f;
    for (int32 Rank = 0; Rank < RankXLeft.Num(); ++Rank) {
        RankXLeft[Rank] = XOffset;
        XOffset += RankWidth[Rank] + NodeSpacingX;
    }

    // Group node indices by their rank for per-layer ordering.
    TArray<TArray<int32>> RankNodes;
    RankNodes.SetNum(MaxRank + 1);
    for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
        const int32 Rank = FMath::Max(0, Nodes[Index].GlobalRank);
        RankNodes[Rank].Add(Index);
    }

    // Sort within each rank by explicit order, then by stable key.
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
    }

    // Count exec incoming edges to enforce the single-incoming constraint.
    TArray<int32> ExecIncoming;
    ExecIncoming.Init(0, Nodes.Num());
    for (const FWorkEdge &Edge : Edges) {
        if (Edge.Kind != EEdgeKind::Exec) {
            continue;
        }
        if (!Nodes.IsValidIndex(Edge.Dst) || Edge.Src == Edge.Dst) {
            continue;
        }
        ExecIncoming[Edge.Dst] += 1;
    }

    // Relax constraints to compute the minimum feasible Y for each node.
    TArray<float> YPositions;
    YPositions.Init(0.0f, Nodes.Num());
    const int32 MaxIterations = FMath::Max(1, Nodes.Num());
    bool bUpdated = true;
    for (int32 Iteration = 0; Iteration < MaxIterations && bUpdated; ++Iteration) {

        // Build constraint list (Target >= Source + Delta).
        TArray<FConstraint> Constraints;
        Constraints.Reserve(Nodes.Num() + Edges.Num());

        // Order constraints within each rank prevent overlap.
        for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
            const TArray<int32> &Layer = RankNodes[Rank];
            for (int32 Index = 1; Index < Layer.Num(); ++Index) {
                const int32 Prev = Layer[Index - 1];
                const int32 Curr = Layer[Index];
                const float SpacingY = Nodes[Curr].bHasExecPins ? NodeSpacingYExec : NodeSpacingYData;
                FConstraint Constraint;
                Constraint.Source = Prev;
                Constraint.Target = Curr;
                Constraint.Delta = Nodes[Prev].Size.Y + SpacingY;
                Constraints.Add(Constraint);
            }
        }

        // Exec constraints keep destination node at or below the source node.
        for (const FWorkEdge &Edge : Edges) {
            if (Edge.Kind != EEdgeKind::Exec) {
                continue;
            }
            if (!Nodes.IsValidIndex(Edge.Src) || !Nodes.IsValidIndex(Edge.Dst)) {
                continue;
            }
            if (Edge.Src == Edge.Dst) {
                continue;
            }
            if (ExecIncoming[Edge.Dst] != 1) {
                continue;
            }

            const FWorkNode &SrcNode = Nodes[Edge.Src];
            const FWorkNode &DstNode = Nodes[Edge.Dst];
            const float SrcOffset = GetApproxPinOffset(SrcNode, Edge.SrcPinIndex, SrcNode.OutputPinCount);
            const float DstOffset = GetApproxPinOffset(DstNode, Edge.DstPinIndex, DstNode.InputPinCount);

            FConstraint Constraint;
            Constraint.Source = Edge.Src;
            Constraint.Target = Edge.Dst;
            Constraint.Delta = 0.0;
            Constraints.Add(Constraint);
        }

        bUpdated = false;
        for (const FConstraint &Constraint : Constraints) {
            if (!Nodes.IsValidIndex(Constraint.Source) || !Nodes.IsValidIndex(Constraint.Target)) {
                continue;
            }
            const float Candidate = YPositions[Constraint.Source] + Constraint.Delta;
            if (Candidate > YPositions[Constraint.Target] + KINDA_SMALL_NUMBER) {
                const float OldY = YPositions[Constraint.Target];
                YPositions[Constraint.Target] = Candidate;
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                       TEXT("  CompactPlacement: Iteration %d updated node guid=%s name=%s to Y=%.1f (old=%.1f "
                            "delta=%.1f from node guid=%s name=%s)"),
                       Iteration, *Nodes[Constraint.Target].Key.Guid.ToString(EGuidFormats::DigitsWithHyphens),
                       *Nodes[Constraint.Target].Name, YPositions[Constraint.Target], OldY, Constraint.Delta,
                       *Nodes[Constraint.Source].Key.Guid.ToString(EGuidFormats::DigitsWithHyphens),
                       *Nodes[Constraint.Source].Name);
                bUpdated = true;
            }
        }
    }

    if (bUpdated) {
        UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("CompactPlacement: constraint relaxation hit max iterations=%d"),
               MaxIterations);
    }

    // Emit final placements using the compacted Y positions.
    for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
        const FWorkNode &Node = Nodes[Index];
        const int32 Rank = FMath::Max(0, Node.GlobalRank);
        const float X = RankXLeft[Rank] + (RankWidth[Rank] - Node.Size.X) * 0.5f;
        const float Y = YPositions[Index];
        const TCHAR *NodeName = Node.Name.IsEmpty() ? TEXT("<unnamed>") : *Node.Name;
        const FString GuidString = Node.Key.Guid.ToString(EGuidFormats::DigitsWithHyphens);
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("  Compact place node guid=%s name=%s rank=%d order=%d at (%.1f, %.1f)"), *GuidString, NodeName,
               Node.GlobalRank, Node.GlobalOrder, X, Y);
        Result.Positions.Add(Index, FVector2f(X, Y));
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
} // namespace GraphLayout
