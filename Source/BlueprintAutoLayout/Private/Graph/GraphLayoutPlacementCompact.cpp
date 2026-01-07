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

// Constraint describing a minimum vertical separation between two nodes.
struct FConstraint
{
    int32 Target = INDEX_NONE;
    int32 Source = INDEX_NONE;
    float Delta = 0.0f;
};

// Approximate pin offset along the node height for constraint positioning.
float GetApproxPinOffset(const FLayoutNode &Node, int32 PinIndex, int32 PinCount)
{
    // Approximate pin location as a fraction of node height using pin index within the
    // direction.
    const int32 Denom = FMath::Max(1, PinCount);
    const float Fraction =
        (static_cast<float>(PinIndex) + 0.5f) / static_cast<float>(Denom);
    return Node.Size.Y * FMath::Clamp(Fraction, 0.0f, 1.0f);
}

// End of anonymous namespace helpers.
} // namespace

// Place nodes by rank order using compact constraint relaxation.
FGlobalPlacement PlaceGlobalRankOrderCompact(
    const TArray<FLayoutNode> &Nodes, const TArray<FLayoutEdge> &Edges,
    float NodeSpacingXExec, float NodeSpacingXData, float NodeSpacingYExec,
    float NodeSpacingYData, EBlueprintAutoLayoutRankAlignment RankAlignment)
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
    for (const FLayoutNode &Node : Nodes) {
        const int32 Rank = FMath::Max(0, Node.GlobalRank);
        MaxRank = FMath::Max(MaxRank, Rank);
    }

    // Compute per-rank widths and spacing based on node types.
    TArray<float> RankWidth;
    TArray<float> RankSpacingX;
    RankWidth.Init(0.0f, MaxRank + 1);
    RankSpacingX.Init(0.0f, MaxRank + 1);
    for (const FLayoutNode &Node : Nodes) {
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

    // Sort within each rank by explicit order, then by stable key.
    for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
        TArray<int32> &Layer = RankNodes[Rank];
        Layer.Sort([&](int32 A, int32 B) {
            const FLayoutNode &NodeA = Nodes[A];
            const FLayoutNode &NodeB = Nodes[B];
            if (NodeA.GlobalOrder != NodeB.GlobalOrder) {
                return NodeA.GlobalOrder < NodeB.GlobalOrder;
            }
            return NodeKeyLess(NodeA.Key, NodeB.Key);
        });
    }

    // Choose a deterministic incoming exec edge per destination for alignment.
    TArray<int32> ExecConstraintEdgeIndex;
    ExecConstraintEdgeIndex.Init(INDEX_NONE, Nodes.Num());

    // Prefer adjacent-rank sources with the smallest order before stable tie-breaks.
    auto IsPreferredExecEdge = [&](const FLayoutEdge &Candidate, int32 CandidateIndex,
                                   int32 CurrentIndex) {
        if (CurrentIndex == INDEX_NONE) {
            return true;
        }
        const FLayoutEdge &Current = Edges[CurrentIndex];
        const int32 DstRank = Nodes[Candidate.Dst].GlobalRank;
        const bool bCandidateAdjacent = Nodes[Candidate.Src].GlobalRank == DstRank - 1;
        const bool bCurrentAdjacent = Nodes[Current.Src].GlobalRank == DstRank - 1;
        if (bCandidateAdjacent != bCurrentAdjacent) {
            return bCandidateAdjacent;
        }
        if (bCandidateAdjacent) {
            const int32 CandidateOrder = Nodes[Candidate.Src].GlobalOrder;
            const int32 CurrentOrder = Nodes[Current.Src].GlobalOrder;
            if (CandidateOrder != CurrentOrder) {
                return CandidateOrder < CurrentOrder;
            }
        }
        if (Candidate.Src != Current.Src) {
            return NodeKeyLess(Nodes[Candidate.Src].Key, Nodes[Current.Src].Key);
        }
        if (Candidate.SrcPinName != Current.SrcPinName) {
            return Candidate.SrcPinName.LexicalLess(Current.SrcPinName);
        }
        if (Candidate.SrcPinIndex != Current.SrcPinIndex) {
            return Candidate.SrcPinIndex < Current.SrcPinIndex;
        }
        if (Candidate.DstPinName != Current.DstPinName) {
            return Candidate.DstPinName.LexicalLess(Current.DstPinName);
        }
        if (Candidate.DstPinIndex != Current.DstPinIndex) {
            return Candidate.DstPinIndex < Current.DstPinIndex;
        }
        if (Candidate.StableKey != Current.StableKey) {
            return Candidate.StableKey < Current.StableKey;
        }
        return CandidateIndex < CurrentIndex;
    };

    // Scan exec edges to select the alignment edge for each destination.
    for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex) {
        const FLayoutEdge &Edge = Edges[EdgeIndex];
        if (Edge.Kind != EEdgeKind::Exec) {
            continue;
        }
        if (!Nodes.IsValidIndex(Edge.Src) || !Nodes.IsValidIndex(Edge.Dst)) {
            continue;
        }
        if (Edge.Src == Edge.Dst) {
            continue;
        }
        const int32 CurrentIndex = ExecConstraintEdgeIndex[Edge.Dst];
        if (IsPreferredExecEdge(Edge, EdgeIndex, CurrentIndex)) {
            ExecConstraintEdgeIndex[Edge.Dst] = EdgeIndex;
        }
    }

    // Track representative destinations for variable-get nodes by rank.
    TArray<TArray<TPair<int32, int32>>> VariableGetDestinationsByRank;
    VariableGetDestinationsByRank.SetNum(Nodes.Num());

    // Compare destinations by order and key for deterministic selection.
    auto IsPreferredVariableGetDestination = [&](int32 Candidate, int32 Current) {
        if (Current == INDEX_NONE) {
            return true;
        }
        const int32 CandidateOrder = Nodes[Candidate].GlobalOrder;
        const int32 CurrentOrder = Nodes[Current].GlobalOrder;
        if (CandidateOrder != CurrentOrder) {
            return CandidateOrder < CurrentOrder;
        }
        if (NodeKeyLess(Nodes[Candidate].Key, Nodes[Current].Key)) {
            return true;
        }
        if (NodeKeyLess(Nodes[Current].Key, Nodes[Candidate].Key)) {
            return false;
        }
        return Candidate < Current;
    };

    // Scan data edges to select one destination per rank for each variable-get source.
    for (const FLayoutEdge &Edge : Edges) {
        if (Edge.Kind == EEdgeKind::Exec) {
            continue;
        }
        if (!Nodes.IsValidIndex(Edge.Src) || !Nodes.IsValidIndex(Edge.Dst)) {
            continue;
        }
        if (Edge.Src == Edge.Dst) {
            continue;
        }
        if (!Nodes[Edge.Src].bIsVariableGet) {
            continue;
        }
        if (Nodes[Edge.Dst].bIsVariableGet) {
            continue;
        }
        // Find the per-rank slot for the destination representative.
        const int32 DestRank = Nodes[Edge.Dst].GlobalRank;
        TArray<TPair<int32, int32>> &Destinations =
            VariableGetDestinationsByRank[Edge.Src];
        int32 RankIndex = INDEX_NONE;
        for (int32 PairIndex = 0; PairIndex < Destinations.Num(); ++PairIndex) {
            if (Destinations[PairIndex].Key == DestRank) {
                RankIndex = PairIndex;
                break;
            }
        }
        // Add or update the destination choice for the rank.
        if (RankIndex == INDEX_NONE) {
            Destinations.Add(TPair<int32, int32>(DestRank, Edge.Dst));
            continue;
        }
        const int32 CurrentDest = Destinations[RankIndex].Value;
        if (IsPreferredVariableGetDestination(Edge.Dst, CurrentDest)) {
            Destinations[RankIndex].Value = Edge.Dst;
        }
    }

    // Relax constraints to compute the minimum feasible Y for each node.
    TArray<float> YPositions;
    YPositions.Init(0.0f, Nodes.Num());
    const int32 MaxIterations = FMath::Max(3, Nodes.Num());
    bool bUpdated = true;
    for (int32 Iteration = 0; Iteration < 4 && bUpdated; ++Iteration) {
        // Build constraint list (Target >= Source + Delta).
        TArray<FConstraint> Constraints;
        Constraints.Reserve(Nodes.Num() + Edges.Num() * 2);

        // Order constraints within each rank prevent overlap.
        for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
            const TArray<int32> &Layer = RankNodes[Rank];
            for (int32 Index = 1; Index < Layer.Num(); ++Index) {
                const int32 Prev = Layer[Index - 1];
                const int32 Curr = Layer[Index];
                const float SpacingY =
                    Nodes[Curr].bHasExecPins ? NodeSpacingYExec : NodeSpacingYData;
                FConstraint Constraint;
                Constraint.Source = Prev;
                Constraint.Target = Curr;
                Constraint.Delta = Nodes[Prev].Size.Y + SpacingY;
                Constraints.Add(Constraint);
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                       TEXT("  CompactPlacement: Iteration %d order constraint node "
                            "guid=%s name=%s ")
                           TEXT(">= node guid=%s name=%s + (nodeHeight=%.1f + "
                                "spacingY=%.1f)"),
                       Iteration,
                       *Nodes[Curr].Key.Guid.ToString(EGuidFormats::DigitsWithHyphens),
                       *Nodes[Curr].Name,
                       *Nodes[Prev].Key.Guid.ToString(EGuidFormats::DigitsWithHyphens),
                       *Nodes[Prev].Name, Nodes[Prev].Size.Y, SpacingY);
            }
        }

        // Align variable-get node sources with representative destinations.
        if (Iteration < MaxIterations - 2) {
            for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
                const TArray<int32> &Layer = RankNodes[Rank];
                for (int32 Index = 1; Index < Layer.Num(); ++Index) {
                    const int32 SrcIndex = Layer[Index];
                    if (!Nodes[SrcIndex].bIsVariableGet) {
                        continue;
                    }
                    const TArray<TPair<int32, int32>> &Destinations =
                        VariableGetDestinationsByRank[SrcIndex];
                    if (Destinations.IsEmpty()) {
                        continue;
                    }
                    // Choose the representative destination with the smallest current
                    // Y.
                    int32 BestDest = INDEX_NONE;
                    float BestY = 0.0f;
                    for (const TPair<int32, int32> &Pair : Destinations) {
                        const int32 DestIndex = Pair.Value;
                        if (!Nodes.IsValidIndex(DestIndex)) {
                            continue;
                        }
                        const float DestY = YPositions[DestIndex];
                        if (BestDest == INDEX_NONE ||
                            DestY < BestY - KINDA_SMALL_NUMBER) {
                            BestDest = DestIndex;
                            BestY = DestY;
                            continue;
                        }
                        if (FMath::IsNearlyEqual(DestY, BestY)) {
                            const int32 CandidateOrder = Nodes[DestIndex].GlobalOrder;
                            const int32 CurrentOrder = Nodes[BestDest].GlobalOrder;
                            if (CandidateOrder != CurrentOrder) {
                                if (CandidateOrder < CurrentOrder) {
                                    BestDest = DestIndex;
                                    BestY = DestY;
                                }
                                continue;
                            }
                            if (NodeKeyLess(Nodes[DestIndex].Key,
                                            Nodes[BestDest].Key)) {
                                BestDest = DestIndex;
                                BestY = DestY;
                            }
                        }
                    }
                    // Add a zero-delta constraint to align the variable-get node.
                    if (BestDest == INDEX_NONE) {
                        continue;
                    }

                    // Check subsequent nodes in the same rank for any node that has
                    // exec pins. Avoid moving data nodes too aggressively; it can drag
                    // exec nodes and prevent convergence. Treat data nodes as
                    // best-effort with weaker constraints.
                    bool bHasExecAfter = false;
                    for (int32 NextLayerIndex = Index + 1; NextLayerIndex < Layer.Num();
                         ++NextLayerIndex) {
                        const int32 NextIndex = Layer[NextLayerIndex];
                        if (Nodes[NextIndex].bHasExecPins) {
                            bHasExecAfter = true;
                            break;
                        }
                    }
                    if (bHasExecAfter) {
                        continue;
                    }
                    FConstraint Constraint;
                    Constraint.Source = BestDest;
                    Constraint.Target = SrcIndex;
                    Constraint.Delta = 0.0f;
                    Constraints.Add(Constraint);
                }
            }
        }

        // Avoid non-convergent cases by restricting the final pass to intra-rank
        // constraints only.
        if (Iteration < MaxIterations - 2) {
            // Exec constraints keep destination node at or below the source node.
            for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex) {
                const FLayoutEdge &Edge = Edges[EdgeIndex];
                if (Edge.Kind != EEdgeKind::Exec) {
                    continue;
                }
                if (!Nodes.IsValidIndex(Edge.Src) || !Nodes.IsValidIndex(Edge.Dst)) {
                    continue;
                }
                if (Edge.Src == Edge.Dst) {
                    continue;
                }
                if (ExecConstraintEdgeIndex[Edge.Dst] != EdgeIndex) {
                    continue;
                }

                // Add a zero-delta constraint for the chosen exec alignment edge.
                FConstraint Constraint;
                Constraint.Source = Edge.Src;
                Constraint.Target = Edge.Dst;
                Constraint.Delta = 0.0;
                Constraints.Add(Constraint);
            }
        }
        bUpdated = false;
        for (const FConstraint &Constraint : Constraints) {
            if (!Nodes.IsValidIndex(Constraint.Source) ||
                !Nodes.IsValidIndex(Constraint.Target)) {
                continue;
            }
            const float Candidate = YPositions[Constraint.Source] + Constraint.Delta;
            if (Candidate > YPositions[Constraint.Target] + KINDA_SMALL_NUMBER) {
                const float OldY = YPositions[Constraint.Target];
                YPositions[Constraint.Target] = Candidate;
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                       TEXT("  CompactPlacement: Iteration %d updated node guid=%s "
                            "name=%s to Y=%.1f (old=%.1f "
                            "delta=%.1f from node guid=%s name=%s)"),
                       Iteration,
                       *Nodes[Constraint.Target].Key.Guid.ToString(
                           EGuidFormats::DigitsWithHyphens),
                       *Nodes[Constraint.Target].Name, YPositions[Constraint.Target],
                       OldY, Constraint.Delta,
                       *Nodes[Constraint.Source].Key.Guid.ToString(
                           EGuidFormats::DigitsWithHyphens),
                       *Nodes[Constraint.Source].Name);
                bUpdated = true;
            }
        }
    }

    // Warn when constraint relaxation fails to converge within iteration limits.
    if (bUpdated) {
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("CompactPlacement: constraint relaxation hit max iterations=%d"),
               MaxIterations);
    }

    // Emit final placements using the compacted Y positions.
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

    // Emit final node positions based on relaxed Y coordinates.
    for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
        const FLayoutNode &Node = Nodes[Index];
        const int32 Rank = FMath::Max(0, Node.GlobalRank);
        const float X =
            RankXLeft[Rank] + GetAlignedOffset(RankWidth[Rank], Node.Size.X);
        const float Y = YPositions[Index];
        const TCHAR *NodeName = Node.Name.IsEmpty() ? TEXT("<unnamed>") : *Node.Name;
        const FString GuidString =
            Node.Key.Guid.ToString(EGuidFormats::DigitsWithHyphens);
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("  Compact place node guid=%s name=%s rank=%d order=%d at (%.1f, "
                    "%.1f)"),
               *GuidString, NodeName, Node.GlobalRank, Node.GlobalOrder, X, Y);
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
        const FLayoutNode &Node = Nodes[Index];
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
