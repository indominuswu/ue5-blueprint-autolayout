// Copyright Epic Games, Inc. All Rights Reserved.

#include "Graph/GraphLayoutSugiyama.h"

#include "BlueprintAutoLayoutLog.h"

namespace GraphLayout
{
namespace
{
void LogRankOrders(const TCHAR *Label, const TCHAR *Stage, const FSugiyamaGraph &Graph,
                   const TArray<TArray<int32>> &RankNodes)
{
    if (!ShouldDumpSugiyamaDetail(Graph)) {
        return;
    }
    for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
        const TArray<int32> &Layer = RankNodes[Rank];
        for (int32 OrderIndex = 0; OrderIndex < Layer.Num(); ++OrderIndex) {
            const int32 NodeIndex = Layer[OrderIndex];
            const FSugiyamaNode &Node = Graph.Nodes[NodeIndex];
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] %s rank=%d order=%d node=%s"), Label, Stage, Rank,
                   OrderIndex, *BuildNodeKeyString(Node.Key));
        }
    }
}

struct FOrderConstraint
{
    int32 Before = INDEX_NONE;
    int32 After = INDEX_NONE;
};

// Convert a pin index into a fractional offset for barycenter computation.
double GetPinOffset(const FSugiyamaNode &Node, int32 PinIndex, int32 PinCount)
{
    const int32 Denom = FMath::Max(1, PinCount);
    return static_cast<double>(PinIndex) / static_cast<double>(Denom);
}

struct FOrderItem
{
    int32 NodeIndex = INDEX_NONE;
    double Barycenter = 0.0;
    int32 NeighborCount = 0;
};

struct FForwardSweepPolicy
{
    const TArray<TArray<int32>> &EdgeList;

    const TCHAR *Direction() const
    {
        return TEXT("Fwd");
    }
    const TCHAR *EdgeLabel() const
    {
        return TEXT("in-edges");
    }
    int32 NeighborRankDelta() const
    {
        return -1;
    }
    const TArray<int32> &GetEdgesForNode(int32 NodeIndex) const
    {
        return EdgeList[NodeIndex];
    }
    int32 GetNeighborIndex(const FSugiyamaEdge &Edge) const
    {
        return Edge.Src;
    }
    const FPinKey &GetPinKey(const FSugiyamaEdge &Edge) const
    {
        return Edge.SrcPin;
    }
    int32 GetPinIndex(const FSugiyamaEdge &Edge) const
    {
        return Edge.SrcPinIndex;
    }
    int32 GetPinCount(const FSugiyamaNode &Node) const
    {
        return Node.OutputPinCount;
    }
    bool ShouldSkip(const FSugiyamaEdge &Edge, const FSugiyamaNode &Neighbor,
                    bool /*bSkipDataPins*/) const
    {
        return Neighbor.ExecOutputPinCount == 0 && Edge.Kind != EEdgeKind::Exec;
    }
};

struct FBackwardSweepPolicy
{
    const TArray<TArray<int32>> &EdgeList;

    const TCHAR *Direction() const
    {
        return TEXT("Bwd");
    }
    const TCHAR *EdgeLabel() const
    {
        return TEXT("out-edges");
    }
    int32 NeighborRankDelta() const
    {
        return 1;
    }
    const TArray<int32> &GetEdgesForNode(int32 NodeIndex) const
    {
        return EdgeList[NodeIndex];
    }
    int32 GetNeighborIndex(const FSugiyamaEdge &Edge) const
    {
        return Edge.Dst;
    }
    const FPinKey &GetPinKey(const FSugiyamaEdge &Edge) const
    {
        return Edge.DstPin;
    }
    int32 GetPinIndex(const FSugiyamaEdge &Edge) const
    {
        return Edge.DstPinIndex;
    }
    int32 GetPinCount(const FSugiyamaNode &Node) const
    {
        return Node.InputPinCount;
    }
    bool ShouldSkip(const FSugiyamaEdge &Edge, const FSugiyamaNode &Neighbor,
                    bool bSkipDataPins) const
    {
        return bSkipDataPins &&
               (Edge.Kind == EEdgeKind::Exec || Neighbor.ExecInputPinCount > 0);
    }
};

template <typename PolicyType>
void RunSweep(FSugiyamaGraph &Graph, TArray<TArray<int32>> &RankNodes,
              bool bCrossDetail, const TCHAR *Label, int32 Sweep, int32 StartRank,
              int32 EndRank, int32 Step, const PolicyType &Policy, bool bSkipDataPins)
{
    for (int32 Rank = StartRank; Rank != EndRank; Rank += Step) {
        TArray<int32> &Layer = RankNodes[Rank];
        if (Layer.IsEmpty()) {
            continue;
        }

        TArray<FOrderItem> Items;
        Items.Reserve(Layer.Num());

        for (int32 NodeIndex : Layer) {
            const FSugiyamaNode &Node = Graph.Nodes[NodeIndex];
            const TArray<int32> &NodeEdges = Policy.GetEdgesForNode(NodeIndex);
            if (bCrossDetail) {
                UE_LOG(LogBlueprintAutoLayout, Verbose,
                       TEXT("Sugiyama[%s] Sweep%d %s rank=%d node=%s calculating "
                            "barycenter from %d %s"),
                       Label, Sweep, Policy.Direction(), Rank,
                       *BuildNodeKeyString(Node.Key), NodeEdges.Num(),
                       Policy.EdgeLabel());
            }
            double Sum = 0.0;
            int32 Count = 0;
            TArray<int32> NeighborEdges;
            for (int32 EdgeIndex : NodeEdges) {
                const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                const int32 NeighborIndex = Policy.GetNeighborIndex(Edge);
                if (Graph.Nodes[NeighborIndex].Rank !=
                    Rank + Policy.NeighborRankDelta()) {
                    continue;
                }
                NeighborEdges.Add(EdgeIndex);
            }

            NeighborEdges.Sort([&](int32 A, int32 B) {
                return PinKeyLess(Policy.GetPinKey(Graph.Edges[A]),
                                  Policy.GetPinKey(Graph.Edges[B]));
            });

            for (int32 EdgeIndex : NeighborEdges) {
                const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                const int32 NeighborIndex = Policy.GetNeighborIndex(Edge);
                const FSugiyamaNode &Neighbor = Graph.Nodes[NeighborIndex];
                const int32 PinIndex = Policy.GetPinIndex(Edge);
                if (Policy.ShouldSkip(Edge, Neighbor, bSkipDataPins)) {
                    // Skip data pins for barycenter calculation
                    if (bCrossDetail) {
                        UE_LOG(LogBlueprintAutoLayout, Verbose,
                               TEXT("Sugiyama[%s]   skip neighbor node=%s order=%d "
                                    "pinIndex=%d (data pin)"),
                               Label, *BuildNodeKeyString(Neighbor.Key), Neighbor.Order,
                               PinIndex);
                    }
                    continue;
                }

                double PinOffset =
                    GetPinOffset(Neighbor, PinIndex, Policy.GetPinCount(Neighbor));
                if (bCrossDetail) {
                    UE_LOG(LogBlueprintAutoLayout, Verbose,
                           TEXT("Sugiyama[%s]   consider neighbor node=%s order=%d "
                                "pinIndex=%d pinoffset=%.3f"),
                           Label, *BuildNodeKeyString(Neighbor.Key), Neighbor.Order,
                           PinIndex, PinOffset);
                }

                Sum += static_cast<double>(Neighbor.Order) + PinOffset;
                ++Count;
            }

            FOrderItem Item;
            Item.NodeIndex = NodeIndex;
            if (Count == 0) {
                Item.Barycenter = static_cast<double>(Node.Order);
            } else {
                Item.Barycenter = Sum / Count;
            }
            Item.NeighborCount = Count;
            Items.Add(Item);
        }

        if (bCrossDetail) {
            for (const FOrderItem &Item : Items) {
                UE_LOG(LogBlueprintAutoLayout, Verbose,
                       TEXT("Sugiyama[%s] Sweep%d %s rank=%d node=%s ")
                           TEXT("bary=%.3f neighbors=%d"),
                       Label, Sweep, Policy.Direction(), Rank,
                       *BuildNodeKeyString(Graph.Nodes[Item.NodeIndex].Key),
                       Item.Barycenter, Item.NeighborCount);
            }
        }

        Items.Sort([&](const FOrderItem &A, const FOrderItem &B) {
            if (A.Barycenter != B.Barycenter) {
                return A.Barycenter < B.Barycenter;
            }
            return NodeKeyLess(Graph.Nodes[A.NodeIndex].Key,
                               Graph.Nodes[B.NodeIndex].Key);
        });

        Layer.Reset(Items.Num());
        for (int32 Index = 0; Index < Items.Num(); ++Index) {
            const int32 NodeIndex = Items[Index].NodeIndex;
            Graph.Nodes[NodeIndex].Order = Index;
            Layer.Add(NodeIndex);
        }

        if (bCrossDetail) {
            for (int32 Index = 0; Index < Items.Num(); ++Index) {
                const int32 NodeIndex = Items[Index].NodeIndex;
                UE_LOG(LogBlueprintAutoLayout, Verbose,
                       TEXT("Sugiyama[%s] Sweep%d %s rank=%d order=%d node=%s"), Label,
                       Sweep, Policy.Direction(), Rank, Index,
                       *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key));
            }
        }
    }
}

void ApplyMinLenZeroOrdering(FSugiyamaGraph &Graph, TArray<TArray<int32>> &RankNodes)
{
    // Gather min-len-zero edges that keep source and destination on the same rank.
    TMap<int32, TArray<int32>> ZeroLenByDst;
    ZeroLenByDst.Reserve(Graph.Nodes.Num());
    TSet<int32> ZeroLenSources;
    ZeroLenSources.Reserve(Graph.Nodes.Num());

    // Filter edges down to min-len-zero links between real nodes on the same layer.
    for (int32 EdgeIndex = 0; EdgeIndex < Graph.Edges.Num(); ++EdgeIndex) {
        const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
        if (Edge.Src == Edge.Dst) {
            continue;
        }
        if (Edge.MinLen != 0) {
            continue;
        }
        if (!Graph.Nodes.IsValidIndex(Edge.Src) ||
            !Graph.Nodes.IsValidIndex(Edge.Dst)) {
            continue;
        }
        const FSugiyamaNode &SrcNode = Graph.Nodes[Edge.Src];
        const FSugiyamaNode &DstNode = Graph.Nodes[Edge.Dst];
        if (SrcNode.bIsDummy || DstNode.bIsDummy) {
            continue;
        }
        if (SrcNode.Rank != DstNode.Rank) {
            continue;
        }
        ZeroLenByDst.FindOrAdd(Edge.Dst).Add(EdgeIndex);
        ZeroLenSources.Add(Edge.Src);
    }

    // Sort each destination's sources by the destination pin index.
    for (TPair<int32, TArray<int32>> &Pair : ZeroLenByDst) {
        Pair.Value.Sort([&](int32 A, int32 B) {
            const FSugiyamaEdge &EdgeA = Graph.Edges[A];
            const FSugiyamaEdge &EdgeB = Graph.Edges[B];
            if (EdgeA.DstPinIndex != EdgeB.DstPinIndex) {
                return EdgeA.DstPinIndex < EdgeB.DstPinIndex;
            }
            return NodeKeyLess(Graph.Nodes[EdgeA.Src].Key, Graph.Nodes[EdgeB.Src].Key);
        });
    }

    // Rebuild each layer so min-len-zero sources follow their destination.
    for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
        TArray<int32> &Layer = RankNodes[Rank];
        if (Layer.IsEmpty()) {
            continue;
        }

        // Track which nodes have already been placed into the new order.
        TSet<int32> Added;
        Added.Reserve(Layer.Num());
        TArray<int32> NewLayer;
        NewLayer.Reserve(Layer.Num());

        // Append a node and its chained min-len-zero sources in pin order.
        auto AppendNodeAndSources = [&](int32 StartNode) {
            TArray<int32> Stack;
            Stack.Add(StartNode);
            while (!Stack.IsEmpty()) {
                const int32 NodeIndex = Stack.Pop(EAllowShrinking::No);
                if (Added.Contains(NodeIndex)) {
                    continue;
                }
                Added.Add(NodeIndex);
                NewLayer.Add(NodeIndex);

                // Queue min-len-zero sources for this destination in pin order.
                const TArray<int32> *EdgeList = ZeroLenByDst.Find(NodeIndex);
                if (!EdgeList) {
                    continue;
                }
                for (int32 EdgeListIndex = EdgeList->Num() - 1; EdgeListIndex >= 0;
                     --EdgeListIndex) {
                    const int32 EdgeIndex = (*EdgeList)[EdgeListIndex];
                    Stack.Add(Graph.Edges[EdgeIndex].Src);
                }
            }
        };

        // Walk the original order and defer min-len-zero sources to their destination.
        for (int32 NodeIndex : Layer) {
            if (Added.Contains(NodeIndex)) {
                continue;
            }
            if (ZeroLenSources.Contains(NodeIndex)) {
                continue;
            }
            AppendNodeAndSources(NodeIndex);
        }

        // Append any remaining nodes that could not be placed via destinations.
        for (int32 NodeIndex : Layer) {
            if (Added.Contains(NodeIndex)) {
                continue;
            }
            AppendNodeAndSources(NodeIndex);
        }

        // Persist the new per-rank order onto nodes and the layer list.
        for (int32 Index = 0; Index < NewLayer.Num(); ++Index) {
            const int32 NodeIndex = NewLayer[Index];
            Graph.Nodes[NodeIndex].Order = Index;
        }
        Layer = MoveTemp(NewLayer);
    }
}
} // namespace

// Initialize per-rank ordering deterministically before crossing reduction.
void AssignInitialOrder(FSugiyamaGraph &Graph, int32 MaxRank,
                        TArray<TArray<int32>> &RankNodes, const TCHAR *Label)
{
    // Group nodes by rank before applying per-layer ordering.
    RankNodes.SetNum(MaxRank + 1);
    for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index) {
        const int32 Rank = Graph.Nodes[Index].Rank;
        if (Rank >= 0 && Rank < RankNodes.Num()) {
            RankNodes[Rank].Add(Index);
        }
    }

    // Prefer exec-bearing nodes and larger exec fan-out to stabilize lane ordering.
    auto IsExecSortNode = [&](const FSugiyamaNode &Node) { return Node.bHasExecPins; };

    auto ExecLayerLess = [&](int32 A, int32 B) {
        const FSugiyamaNode &NodeA = Graph.Nodes[A];
        const FSugiyamaNode &NodeB = Graph.Nodes[B];
        const bool bExecA = IsExecSortNode(NodeA);
        const bool bExecB = IsExecSortNode(NodeB);
        if (bExecA != bExecB) {
            return bExecA;
        }
        if (bExecA && NodeA.ExecOutputPinCount != NodeB.ExecOutputPinCount) {
            return NodeA.ExecOutputPinCount > NodeB.ExecOutputPinCount;
        }
        return NodeKeyLess(NodeA.Key, NodeB.Key);
    };

    for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
        TArray<int32> &Layer = RankNodes[Rank];
        Layer.Sort(ExecLayerLess);
        // Persist the sorted order onto nodes for later sweeps.
        for (int32 Order = 0; Order < Layer.Num(); ++Order) {
            Graph.Nodes[Layer[Order]].Order = Order;
        }
    }

    LogRankOrders(Label, TEXT("InitialOrder"), Graph, RankNodes);
}

// Sweep forward and backward to reduce edge crossings using barycenters.
void RunCrossingReduction(FSugiyamaGraph &Graph, int32 MaxRank, int32 NumSweeps,
                          TArray<TArray<int32>> &RankNodes, const TCHAR *Label)
{
    const bool bDumpDetail = ShouldDumpSugiyamaDetail(Graph);
    const bool bCrossDetail = Graph.Nodes.Num() <= kVerboseCrossingDetailLimit &&
                              Graph.Edges.Num() <= kVerboseDumpEdgeLimit;
    if (MaxRank <= 0 || NumSweeps <= 0) {
        if (bDumpDetail) {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] CrossingReduction: skipped maxRank=%d ")
                       TEXT("sweeps=%d"),
                   Label, MaxRank, NumSweeps);
        }
        return;
    }

    if (bDumpDetail) {
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("Sugiyama[%s] CrossingReduction: sweeps=%d maxRank=%d"), Label,
               NumSweeps, MaxRank);
    }

    // Build adjacency lists for barycenter calculations.
    TArray<TArray<int32>> InEdges;
    TArray<TArray<int32>> OutEdges;
    InEdges.SetNum(Graph.Nodes.Num());
    OutEdges.SetNum(Graph.Nodes.Num());

    for (int32 EdgeIndex = 0; EdgeIndex < Graph.Edges.Num(); ++EdgeIndex) {
        const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
        if (Edge.Src == Edge.Dst) {
            continue;
        }
        OutEdges[Edge.Src].Add(EdgeIndex);
        InEdges[Edge.Dst].Add(EdgeIndex);
    }

    // Keep each rank list aligned to the node order field.
    auto SortRankByOrder = [&](int32 Rank) {
        TArray<int32> &Layer = RankNodes[Rank];
        Layer.Sort([&](int32 A, int32 B) {
            const FSugiyamaNode &NodeA = Graph.Nodes[A];
            const FSugiyamaNode &NodeB = Graph.Nodes[B];
            if (NodeA.Order != NodeB.Order) {
                return NodeA.Order < NodeB.Order;
            }
            return NodeKeyLess(NodeA.Key, NodeB.Key);
        });
    };

    const FForwardSweepPolicy ForwardPolicy{InEdges};
    const FBackwardSweepPolicy BackwardPolicy{OutEdges};

    for (int32 Sweep = 0; Sweep < NumSweeps; ++Sweep) {
        // Forward sweep: order each rank by barycenter of incoming neighbors.
        RunSweep(Graph, RankNodes, bCrossDetail, Label, Sweep, 1, MaxRank + 1, 1,
                 ForwardPolicy, false);

        // 早期終了したexecレーンのorderは決定不能なので、最後に1回だけforwardを回す。
        // 最後のbackwardはデータノードをexecノードと同じorderに揃えるために必要。
        if (Sweep < NumSweeps - 1) {
            RunSweep(Graph, RankNodes, bCrossDetail, Label, Sweep, MaxRank - 1, -1, -1,
                     BackwardPolicy, true);
        }

        if (Sweep < NumSweeps - 2) {
            RunSweep(Graph, RankNodes, bCrossDetail, Label, Sweep, MaxRank - 1, -1, -1,
                     BackwardPolicy, false);
        }

        for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
            SortRankByOrder(Rank);
        }
    }

    // Enforce min-len-zero ordering after crossing reduction sweeps.
    ApplyMinLenZeroOrdering(Graph, RankNodes);

    // Emit the final per-rank orders for debugging.
    LogRankOrders(Label, TEXT("CrossingFinalOrder"), Graph, RankNodes);
}
} // namespace GraphLayout
