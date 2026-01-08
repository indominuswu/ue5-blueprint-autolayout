// Copyright Epic Games, Inc. All Rights Reserved.

// Layout graph public interface.
#include "Graph/GraphLayout.h"

// Placement and Sugiyama layout passes.
#include "Graph/GraphLayoutPlacement.h"
#include "Graph/GraphLayoutSugiyama.h"

// Logging for layout diagnostics.
#include "BlueprintAutoLayoutLog.h"

// Utility helpers for deterministic ordering and hashing.
#include "Algo/Unique.h"
#include "Misc/Crc.h"

// Graph layout implementation.
namespace GraphLayout
{
namespace
{
// Tuning constants for the Sugiyama sweeps used during layout.
constexpr int32 kSugiyamaSweeps = 8;

// Create a stable GUID from a string seed so synthetic nodes are deterministic
// across runs and machines.
FGuid MakeDeterministicGuid(const FString &Seed)
{
    const uint32 A = FCrc::StrCrc32(*Seed);
    const uint32 B = FCrc::StrCrc32(*FString::Printf(TEXT("%s|A"), *Seed));
    const uint32 C = FCrc::StrCrc32(*FString::Printf(TEXT("%s|B"), *Seed));
    const uint32 D = FCrc::StrCrc32(*FString::Printf(TEXT("%s|C"), *Seed));
    return FGuid(A, B, C, D);
}

// Build a synthetic node key from a deterministic GUID seed.
FNodeKey MakeSyntheticNodeKey(const FString &Seed)
{
    FNodeKey Key;
    Key.Guid = MakeDeterministicGuid(Seed);
    return Key;
}

// Create a placeholder pin identity for dummy edge segments.
FPinKey MakeDummyPinKey(const FNodeKey &Owner, EPinDirection Direction)
{
    return MakePinKey(Owner, Direction, FName(TEXT("Dummy")), 0);
}

// Log a summary of the Sugiyama graph contents.
void LogSugiyamaSummary(const TCHAR *Label, const TCHAR *Stage,
                        const FSugiyamaGraph &Graph)
{
    const int32 DummyCount = CountDummyNodes(Graph);
    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("Sugiyama[%s] %s: nodes=%d edges=%d dummy=%d"), Label, Stage,
           Graph.Nodes.Num(), Graph.Edges.Num(), DummyCount);
}

// Log detailed node state for the Sugiyama graph.
void LogSugiyamaNodes(const TCHAR *Label, const TCHAR *Stage,
                      const FSugiyamaGraph &Graph)
{
    if (!ShouldDumpSugiyamaDetail(Graph)) {
        return;
    }
    for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index) {
        const FSugiyamaNode &Node = Graph.Nodes[Index];
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("Sugiyama[%s] %s node[%d]: key=%s rank=%d order=%d ")
                   TEXT("size=(%.1f,%.1f) execOut=%d dummy=%d srcIndex=%d"),
               Label, Stage, Index, *BuildNodeKeyString(Node.Key), Node.Rank,
               Node.Order, Node.Size.X, Node.Size.Y, Node.ExecOutputPinCount,
               Node.bIsDummy ? 1 : 0, Node.SourceIndex);
    }
}

// Log detailed edge state for the Sugiyama graph.
void LogSugiyamaEdges(const TCHAR *Label, const TCHAR *Stage,
                      const FSugiyamaGraph &Graph)
{
    if (!ShouldDumpSugiyamaDetail(Graph)) {
        return;
    }
    for (int32 EdgeIndex = 0; EdgeIndex < Graph.Edges.Num(); ++EdgeIndex) {
        const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
        const FString SrcKey = Graph.Nodes.IsValidIndex(Edge.Src)
                                   ? BuildNodeKeyString(Graph.Nodes[Edge.Src].Key)
                                   : FString(TEXT("invalid"));
        const FString DstKey = Graph.Nodes.IsValidIndex(Edge.Dst)
                                   ? BuildNodeKeyString(Graph.Nodes[Edge.Dst].Key)
                                   : FString(TEXT("invalid"));
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("Sugiyama[%s] %s edge[%d]: %s -> %s srcPin=%s dstPin=%s ")
                   TEXT("stable=%s"),
               Label, Stage, EdgeIndex, *SrcKey, *DstKey,
               *BuildPinKeyString(Edge.SrcPin), *BuildPinKeyString(Edge.DstPin),
               *Edge.StableKey);
    }
}

// Keep deterministic ordering when building queues or layers by node key.
void InsertSortedByNodeKey(const TArray<FSugiyamaNode> &Nodes, TArray<int32> &List,
                           int32 NodeIndex)
{
    int32 InsertIndex = 0;
    while (InsertIndex < List.Num() &&
           NodeKeyLess(Nodes[List[InsertIndex]].Key, Nodes[NodeIndex].Key)) {
        ++InsertIndex;
    }
    List.Insert(NodeIndex, InsertIndex);
}

// Build out-edge lists while respecting temporary reversals used for cycle
// breaking, so DFS sees a consistent effective direction.
void BuildEffectiveOutEdges(const FSugiyamaGraph &Graph,
                            TArray<TArray<int32>> &OutEdges)
{
    OutEdges.SetNum(Graph.Nodes.Num());
    for (int32 EdgeIndex = 0; EdgeIndex < Graph.Edges.Num(); ++EdgeIndex) {
        const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
        const int32 Src = Edge.bReversed ? Edge.Dst : Edge.Src;
        const int32 Dst = Edge.bReversed ? Edge.Src : Edge.Dst;
        if (Src == Dst) {
            continue;
        }
        OutEdges[Src].Add(EdgeIndex);
    }

    // Sort per-node edge lists for deterministic traversal.
    for (TArray<int32> &EdgeList : OutEdges) {
        EdgeList.Sort([&](int32 A, int32 B) {
            const FSugiyamaEdge &EdgeA = Graph.Edges[A];
            const FSugiyamaEdge &EdgeB = Graph.Edges[B];
            const FPinKey &PinA = EdgeA.bReversed ? EdgeA.DstPin : EdgeA.SrcPin;
            const FPinKey &PinB = EdgeB.bReversed ? EdgeB.DstPin : EdgeB.SrcPin;
            int32 Compare = ComparePinKey(PinA, PinB);
            if (Compare != 0) {
                return Compare < 0;
            }
            const int32 DstA = EdgeA.bReversed ? EdgeA.Src : EdgeA.Dst;
            const int32 DstB = EdgeB.bReversed ? EdgeB.Src : EdgeB.Dst;
            Compare = CompareNodeKey(Graph.Nodes[DstA].Key, Graph.Nodes[DstB].Key);
            if (Compare != 0) {
                return Compare < 0;
            }
            if (EdgeA.StableKey != EdgeB.StableKey) {
                return EdgeA.StableKey < EdgeB.StableKey;
            }
            return A < B;
        });
    }
}

// DFS visit state used to detect back edges.
enum class EVisitState : uint8
{
    Unvisited,
    Visiting,
    Done
};

// Find back edges via DFS and flip the best candidate until the graph is a DAG.
void RemoveCycles(FSugiyamaGraph &Graph, const TCHAR *Label)
{
    if (Graph.Nodes.Num() < 2 || Graph.Edges.IsEmpty()) {
        return;
    }

    // Log the start of cycle removal for diagnostics.
    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("Sugiyama[%s] RemoveCycles: start nodes=%d edges=%d"), Label,
           Graph.Nodes.Num(), Graph.Edges.Num());

    // Use a stable node order so cycle breaking stays deterministic.
    TArray<int32> NodeOrder;
    NodeOrder.Reserve(Graph.Nodes.Num());
    for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index) {
        NodeOrder.Add(Index);
    }
    NodeOrder.Sort([&](int32 A, int32 B) {
        return NodeKeyLess(Graph.Nodes[A].Key, Graph.Nodes[B].Key);
    });

    // Repeat until no back edges remain.
    for (;;) {
        // Build effective adjacency with current reversals and find back edges.
        TArray<TArray<int32>> OutEdges;
        BuildEffectiveOutEdges(Graph, OutEdges);

        // Initialize visit state for DFS traversal.
        TArray<EVisitState> VisitState;
        VisitState.Init(EVisitState::Unvisited, Graph.Nodes.Num());

        // Iterative DFS stack avoids recursion and tracks next edge index.
        struct FStackEntry
        {
            int32 NodeIndex = INDEX_NONE;
            int32 NextEdge = 0;
        };

        // Collect edges that point back into the current DFS stack.
        TArray<int32> BackEdges;

        // Walk each unvisited node to discover back edges.
        for (int32 StartNode : NodeOrder) {
            if (VisitState[StartNode] != EVisitState::Unvisited) {
                continue;
            }

            // Seed the DFS stack for this component.
            TArray<FStackEntry> Stack;
            Stack.Add({StartNode, 0});
            VisitState[StartNode] = EVisitState::Visiting;

            // Traverse the graph iteratively to avoid recursion.
            while (!Stack.IsEmpty()) {
                FStackEntry &Entry = Stack.Last();
                if (Entry.NextEdge >= OutEdges[Entry.NodeIndex].Num()) {
                    VisitState[Entry.NodeIndex] = EVisitState::Done;
                    Stack.Pop();
                    continue;
                }

                // Advance to the next outgoing edge.
                const int32 EdgeIndex = OutEdges[Entry.NodeIndex][Entry.NextEdge++];
                const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                const int32 NextNode = Edge.bReversed ? Edge.Src : Edge.Dst;

                // Traverse to unvisited nodes or record back edges.
                if (VisitState[NextNode] == EVisitState::Unvisited) {
                    VisitState[NextNode] = EVisitState::Visiting;
                    Stack.Add({NextNode, 0});
                } else if (VisitState[NextNode] == EVisitState::Visiting) {
                    BackEdges.Add(EdgeIndex);
                }
            }
        }

        // Stop once the graph is acyclic.
        if (BackEdges.IsEmpty()) {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] RemoveCycles: done"), Label);
            break;
        }

        // Log the number of back edges before selecting one to reverse.
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("Sugiyama[%s] RemoveCycles: backEdges=%d"), Label, BackEdges.Num());

        // Choose a deterministic back edge to reverse for cycle breaking.
        auto IsEdgeLess = [&](int32 A, int32 B) {
            const FSugiyamaEdge &EdgeA = Graph.Edges[A];
            const FSugiyamaEdge &EdgeB = Graph.Edges[B];

            // Compare effective endpoints and pin keys for determinism.
            const int32 SrcA = EdgeA.bReversed ? EdgeA.Dst : EdgeA.Src;
            const int32 SrcB = EdgeB.bReversed ? EdgeB.Dst : EdgeB.Src;
            const int32 DstA = EdgeA.bReversed ? EdgeA.Src : EdgeA.Dst;
            const int32 DstB = EdgeB.bReversed ? EdgeB.Src : EdgeB.Dst;
            const FPinKey &SrcPinA = EdgeA.bReversed ? EdgeA.DstPin : EdgeA.SrcPin;
            const FPinKey &SrcPinB = EdgeB.bReversed ? EdgeB.DstPin : EdgeB.SrcPin;
            const FPinKey &DstPinA = EdgeA.bReversed ? EdgeA.SrcPin : EdgeA.DstPin;
            const FPinKey &DstPinB = EdgeB.bReversed ? EdgeB.SrcPin : EdgeB.DstPin;

            // Compare source/destination keys and pins in order.
            int32 Compare =
                CompareNodeKey(Graph.Nodes[SrcA].Key, Graph.Nodes[SrcB].Key);
            if (Compare != 0) {
                return Compare < 0;
            }
            Compare = ComparePinKey(SrcPinA, SrcPinB);
            if (Compare != 0) {
                return Compare < 0;
            }
            Compare = CompareNodeKey(Graph.Nodes[DstA].Key, Graph.Nodes[DstB].Key);
            if (Compare != 0) {
                return Compare < 0;
            }
            Compare = ComparePinKey(DstPinA, DstPinB);
            if (Compare != 0) {
                return Compare < 0;
            }
            return A < B;
        };

        // Choose the best back edge to reverse for cycle breaking.
        int32 BestEdge = BackEdges[0];
        for (int32 EdgeIndex : BackEdges) {
            if (IsEdgeLess(EdgeIndex, BestEdge)) {
                BestEdge = EdgeIndex;
            }
        }
        const FSugiyamaEdge &ChosenEdge = Graph.Edges[BestEdge];
        const int32 EffectiveSrc =
            ChosenEdge.bReversed ? ChosenEdge.Dst : ChosenEdge.Src;
        const int32 EffectiveDst =
            ChosenEdge.bReversed ? ChosenEdge.Src : ChosenEdge.Dst;
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("Sugiyama[%s] RemoveCycles: reverse edge %s -> %s stable=%s"),
               Label, *BuildNodeKeyString(Graph.Nodes[EffectiveSrc].Key),
               *BuildNodeKeyString(Graph.Nodes[EffectiveDst].Key),
               *ChosenEdge.StableKey);
        // Flip the selected edge and repeat until all cycles are removed.
        Graph.Edges[BestEdge].bReversed = !Graph.Edges[BestEdge].bReversed;
    }
}

// Commit reversal flags by swapping endpoints and pin metadata.
void ApplyEdgeDirections(FSugiyamaGraph &Graph)
{
    for (FSugiyamaEdge &Edge : Graph.Edges) {
        if (!Edge.bReversed) {
            continue;
        }
        Swap(Edge.Src, Edge.Dst);
        Swap(Edge.SrcPin, Edge.DstPin);
        Swap(Edge.SrcPinIndex, Edge.DstPinIndex);
        Edge.bReversed = false;
    }
}

// Build out-edge lists for the finalized DAG, sorted for determinism.
void BuildOutEdges(const FSugiyamaGraph &Graph, TArray<TArray<int32>> &OutEdges)
{
    OutEdges.SetNum(Graph.Nodes.Num());
    for (int32 EdgeIndex = 0; EdgeIndex < Graph.Edges.Num(); ++EdgeIndex) {
        const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
        if (Edge.Src == Edge.Dst) {
            continue;
        }
        OutEdges[Edge.Src].Add(EdgeIndex);
    }

    // Sort per-node edge lists for deterministic traversal.
    for (TArray<int32> &EdgeList : OutEdges) {
        EdgeList.Sort([&](int32 A, int32 B) {
            const FSugiyamaEdge &EdgeA = Graph.Edges[A];
            const FSugiyamaEdge &EdgeB = Graph.Edges[B];
            int32 Compare = ComparePinKey(EdgeA.SrcPin, EdgeB.SrcPin);
            if (Compare != 0) {
                return Compare < 0;
            }
            Compare =
                CompareNodeKey(Graph.Nodes[EdgeA.Dst].Key, Graph.Nodes[EdgeB.Dst].Key);
            if (Compare != 0) {
                return Compare < 0;
            }
            if (EdgeA.StableKey != EdgeB.StableKey) {
                return EdgeA.StableKey < EdgeB.StableKey;
            }
            return A < B;
        });
    }
}

// Determine whether an edge has a finite max length constraint.
bool EdgeHasFiniteMaxLen(const FSugiyamaGraph &Graph, const FSugiyamaEdge &Edge)
{
    if (Edge.Src == Edge.Dst) {
        return false;
    }
    const FSugiyamaNode &SrcNode = Graph.Nodes[Edge.Src];
    const FSugiyamaNode &DstNode = Graph.Nodes[Edge.Dst];
    return !SrcNode.bHasExecPins || !DstNode.bHasExecPins;
}

// Count unique variable-get destinations per source node.
TArray<int32> BuildVariableGetDestCounts(const FSugiyamaGraph &Graph)
{
    TArray<int32> Counts;
    Counts.Init(0, Graph.Nodes.Num());

    // Gather destination pairs for variable-get sources.
    TArray<TPair<int32, int32>> DestPairs;
    DestPairs.Reserve(Graph.Edges.Num());

    // Collect edges that originate from variable-get nodes.
    for (const FSugiyamaEdge &Edge : Graph.Edges) {
        if (Edge.Src == Edge.Dst) {
            continue;
        }
        if (!Graph.Nodes.IsValidIndex(Edge.Src) ||
            !Graph.Nodes.IsValidIndex(Edge.Dst)) {
            continue;
        }
        if (!Graph.Nodes[Edge.Src].bIsVariableGet) {
            continue;
        }
        DestPairs.Emplace(Edge.Src, Edge.Dst);
    }

    // Sort pairs to allow unique destination counting.
    DestPairs.Sort([](const TPair<int32, int32> &A, const TPair<int32, int32> &B) {
        if (A.Key != B.Key) {
            return A.Key < B.Key;
        }
        return A.Value < B.Value;
    });

    // Count unique destinations per variable-get source.
    int32 PrevSrc = INDEX_NONE;
    int32 PrevDst = INDEX_NONE;
    for (const TPair<int32, int32> &Pair : DestPairs) {
        if (Pair.Key == PrevSrc && Pair.Value == PrevDst) {
            continue;
        }
        if (Counts.IsValidIndex(Pair.Key)) {
            ++Counts[Pair.Key];
        }
        PrevSrc = Pair.Key;
        PrevDst = Pair.Value;
    }

    // Return the destination counts.
    return Counts;
}

// Resolve min length for an edge given variable-get constraints.
int32 GetEdgeMinLength(const FSugiyamaGraph &Graph, const FSugiyamaEdge &Edge,
                       int32 VariableGetMinLength,
                       const TArray<int32> &VariableGetDestCounts)
{
    if (!Graph.Nodes.IsValidIndex(Edge.Src) || !Graph.Nodes.IsValidIndex(Edge.Dst)) {
        return 1;
    }
    const FSugiyamaNode &SrcNode = Graph.Nodes[Edge.Src];
    const FSugiyamaNode &DstNode = Graph.Nodes[Edge.Dst];
    if (SrcNode.bHasExecPins) {
        return 1;
    }
    if (!SrcNode.bIsVariableGet) {
        return 1;
    }
    if (VariableGetDestCounts.IsValidIndex(Edge.Src) &&
        VariableGetDestCounts[Edge.Src] > 1) {
        return 1;
    }
    return FMath::Max(0, VariableGetMinLength);
}

// Update min length values on edges based on variable-get rules.
void UpdateEdgeMinLengths(FSugiyamaGraph &Graph, int32 VariableGetMinLength)
{
    // Build per-variable-get destination counts to resolve min length rules.
    const TArray<int32> VariableGetDestCounts = BuildVariableGetDestCounts(Graph);

    // Cache the resolved min length on each edge for later passes.
    for (FSugiyamaEdge &Edge : Graph.Edges) {
        Edge.MinLen =
            GetEdgeMinLength(Graph, Edge, VariableGetMinLength, VariableGetDestCounts);
    }
}

// Check if any edge uses finite max length constraints.
bool GraphUsesFiniteMaxLen(const FSugiyamaGraph &Graph)
{
    for (const FSugiyamaEdge &Edge : Graph.Edges) {
        if (EdgeHasFiniteMaxLen(Graph, Edge)) {
            return true;
        }
    }
    return false;
}

// Assign a rank to each node using a topological pass.
int32 AssignLayers(FSugiyamaGraph &Graph, const TCHAR *Label,
                   int32 VariableGetMinLength)
{
    const int32 NodeCount = Graph.Nodes.Num();
    if (NodeCount == 0) {
        // Nothing to rank if the graph has no nodes.
        return 0;
    }

    // Log the initial layer assignment context.
    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("Sugiyama[%s] AssignLayers: nodes=%d edges=%d"), Label, NodeCount,
           Graph.Edges.Num());
    const bool bDumpDetail = ShouldDumpSugiyamaDetail(Graph);
    const bool bUseMaxLenConstraints = GraphUsesFiniteMaxLen(Graph);
    if (bUseMaxLenConstraints) {
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("Sugiyama[%s] AssignLayers: maxLen constraints enabled (data nodes "
                    "maxLen=1, variableGetMinLen=%d)"),
               Label, VariableGetMinLength);
    }

    // RankBase is the minimum layer each node can occupy based on constraints.
    TArray<int32> RankBase;
    RankBase.Init(0, NodeCount);

    // InDegree counts incoming edges for topological processing.
    TArray<int32> InDegree;
    InDegree.Init(0, NodeCount);

    // Count incoming edges for each node.
    for (const FSugiyamaEdge &Edge : Graph.Edges) {
        if (Edge.Src == Edge.Dst) {
            continue;
        }
        InDegree[Edge.Dst] += 1;
    }

    // OutEdges provides adjacency by source node for fast traversal.
    TArray<TArray<int32>> OutEdges;
    BuildOutEdges(Graph, OutEdges);

    // Seed the queue with source nodes, ordered by node key for determinism.
    TArray<int32> Queue;
    for (int32 Index = 0; Index < NodeCount; ++Index) {
        if (InDegree[Index] == 0) {
            InsertSortedByNodeKey(Graph.Nodes, Queue, Index);
        }
    }

    // TopoOrder records a deterministic topological ordering for later passes.
    TArray<int32> TopoOrder;
    TopoOrder.Reserve(NodeCount);
    // Track which nodes were included to detect cycles.
    TArray<bool> InTopo;
    InTopo.Init(false, NodeCount);

    // Kahn's algorithm: build topo order.
    while (!Queue.IsEmpty()) {
        const int32 NodeIndex = Queue[0];
        Queue.RemoveAt(0);
        TopoOrder.Add(NodeIndex);
        InTopo[NodeIndex] = true;

        // Relax outgoing edges and update in-degree counts.
        for (int32 EdgeIndex : OutEdges[NodeIndex]) {
            FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
            const int32 Dst = Edge.Dst;
            InDegree[Dst] -= 1;
            if (InDegree[Dst] == 0) {
                InsertSortedByNodeKey(Graph.Nodes, Queue, Dst);
            }
        }
    }

    // Handle cycles or disconnected nodes by appending remaining nodes.
    if (TopoOrder.Num() < NodeCount) {
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("Sugiyama[%s] TopoOrder: cycles/disconnected nodes topo=%d/%d"),
               Label, TopoOrder.Num(), NodeCount);
        // Cycles or self-contained components: append remaining nodes in key order.
        TArray<int32> Remaining;
        Remaining.Reserve(NodeCount - TopoOrder.Num());
        for (int32 Index = 0; Index < NodeCount; ++Index) {
            if (!InTopo[Index]) {
                Remaining.Add(Index);
            }
        }
        if (bDumpDetail) {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] TopoOrder: remaining nodes=%d"), Label,
                   Remaining.Num());
            for (int32 Index = 0; Index < Remaining.Num(); ++Index) {
                const int32 NodeIndex = Remaining[Index];
                UE_LOG(LogBlueprintAutoLayout, Verbose,
                       TEXT("Sugiyama[%s] TopoOrder remaining[%d]: node=%s"), Label,
                       Index, *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key));
            }
        } else {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] TopoOrder: remaining nodes list suppressed"),
                   Label);
        }
        Remaining.Sort([&](int32 A, int32 B) {
            return NodeKeyLess(Graph.Nodes[A].Key, Graph.Nodes[B].Key);
        });
        if (bDumpDetail) {
            for (int32 Index = 0; Index < Remaining.Num(); ++Index) {
                const int32 NodeIndex = Remaining[Index];
                UE_LOG(LogBlueprintAutoLayout, Verbose,
                       TEXT("Sugiyama[%s] TopoOrder remainingSorted[%d]: node=%s"),
                       Label, Index, *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key));
            }
        }
        TopoOrder.Append(Remaining);
        if (bDumpDetail) {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] TopoOrder: appended remaining total=%d"), Label,
                   TopoOrder.Num());
        }
    }

    // Log the computed topological order at very verbose levels.
    UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
           TEXT("Sugiyama[%s] TopoOrder: begin total=%d"), Label, TopoOrder.Num());
    for (int32 OrderIndex = 0; OrderIndex < TopoOrder.Num(); ++OrderIndex) {
        const int32 NodeIndex = TopoOrder[OrderIndex];
        const FSugiyamaNode &Node = Graph.Nodes[NodeIndex];
        const TCHAR *Name = Node.Name.IsEmpty() ? TEXT("<unnamed>") : *Node.Name;
        UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
               TEXT("Sugiyama[%s] TopoOrder[%d]: node=%s name=%s"), Label, OrderIndex,
               *BuildNodeKeyString(Node.Key), Name);
    }

    // Apply either maxLen constraints or a simple longest-path ranking.
    if (bUseMaxLenConstraints) {
        struct FConstraintEdge
        {
            int32 Src = INDEX_NONE;
            int32 Dst = INDEX_NONE;
            int32 Weight = 0;
            bool EdgeHasFiniteMaxLen = false;
        };

        // Build forward constraint edges in topo order.
        TArray<FConstraintEdge> ForwardConstraints;
        ForwardConstraints.Reserve(Graph.Edges.Num());
        for (int32 NodeIndex : TopoOrder) {
            // Collect constraint edges in topo order for a forward pass.
            for (int32 EdgeIndex : OutEdges[NodeIndex]) {
                const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                if (Edge.Src == Edge.Dst) {
                    continue;
                }
                FConstraintEdge Constraint;
                Constraint.Src = Edge.Src;
                Constraint.Dst = Edge.Dst;
                Constraint.Weight = Edge.MinLen;
                Constraint.EdgeHasFiniteMaxLen = EdgeHasFiniteMaxLen(Graph, Edge);
                ForwardConstraints.Add(Constraint);
            }
        }

        // Forward pass: propagate minimum ranks based on edge weights.
        // for (const FConstraintEdge &Constraint : ForwardConstraints) {
        //     const int32 OldRank = RankBase[Constraint.Dst];
        //     const int32 Candidate = RankBase[Constraint.Src] + Constraint.Weight;
        //     if (Candidate > OldRank) {
        //         RankBase[Constraint.Dst] = Candidate;
        //         const FSugiyamaNode &SrcNode = Graph.Nodes[Constraint.Src];
        //         const FSugiyamaNode &DstNode = Graph.Nodes[Constraint.Dst];
        //         const FString SrcName =
        //             SrcNode.Name.IsEmpty() ? TEXT("<unnamed>") : SrcNode.Name;
        //         const FString DstName =
        //             DstNode.Name.IsEmpty() ? TEXT("<unnamed>") : DstNode.Name;
        //         UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
        //                TEXT("Sugiyama[%s] AssignLayers dst=%s name=%s rank %d->%d "
        //                     "src=%s name=%s w=%d"),
        //                Label, *BuildNodeKeyString(DstNode.Key), *DstName, OldRank,
        //                RankBase[Constraint.Dst], *BuildNodeKeyString(SrcNode.Key),
        //                *SrcName, Constraint.Weight);
        //     }
        // }

        // Propagate rank bases using the forward constraints.
        for (int32 NodeIndex : TopoOrder) {
            for (int32 EdgeIndex : OutEdges[NodeIndex]) {
                const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                const int32 Dst = Edge.Dst;
                // Enforce "child is at least one rank below parent".
                const int32 EdgeWeight = Edge.MinLen;
                RankBase[Dst] =
                    FMath::Max(RankBase[Dst], RankBase[NodeIndex] + EdgeWeight);
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                       TEXT("Sugiyama[%s] AssignLayers forward pass: src=%s dst=%s "
                            "rankBase=%d weight=%d"),
                       Label, *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key),
                       *BuildNodeKeyString(Graph.Nodes[Dst].Key), RankBase[Dst],
                       EdgeWeight);
            }
        }

        // Backward pass to tighten ranks based on maxLen constraints.
        TMap<int32, TArray<int32>> SrcToDsts;
        TMap<int32, FConstraintEdge> SrcToConstraint;
        for (const FConstraintEdge &Constraint : ForwardConstraints) {
            if (Constraint.EdgeHasFiniteMaxLen == false) {
                continue;
            }
            TArray<int32> &DstList = SrcToDsts.FindOrAdd(Constraint.Src);
            DstList.Add(Constraint.Dst);
            SrcToConstraint.Add(Constraint.Src, Constraint);
        }
        // Repeat a few sweeps to propagate tightened ranks.
        // 何回かやる
        for (int32 Sweep = 0; Sweep < 10; ++Sweep) {
            if (bDumpDetail) {
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                       TEXT("Sugiyama[%s] AssignLayers: backward pass sweep %d"), Label,
                       Sweep);
            }
            bool bUpdated = false;
            for (int32 OrderIndex = TopoOrder.Num() - 1; OrderIndex >= 0;
                 --OrderIndex) {
                const int32 NodeIndex = TopoOrder[OrderIndex];
                if (!SrcToDsts.Contains(NodeIndex)) {
                    continue;
                }
                FConstraintEdge &Constraint = SrcToConstraint.FindChecked(NodeIndex);
                TArray<int32> &DstList = SrcToDsts.FindChecked(NodeIndex);
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                       TEXT("Sugiyama[%s]    src=%s dsts=%d w=%d"), Label,
                       *BuildNodeKeyString(Graph.Nodes[Constraint.Src].Key),
                       DstList.Num(), Constraint.Weight);
                int32 MinRank = MAX_int32;
                for (int32 Dst : DstList) {
                    MinRank = FMath::Min(MinRank, RankBase[Dst]);
                    if (bDumpDetail) {
                        UE_LOG(
                            LogBlueprintAutoLayout, VeryVerbose,
                            TEXT("Sugiyama[%s]      src=%s consider dst=%s rank=%d"),
                            Label, *BuildNodeKeyString(Graph.Nodes[Constraint.Src].Key),
                            *BuildNodeKeyString(Graph.Nodes[Dst].Key), RankBase[Dst]);
                    }
                }
                MinRank =
                    FMath::Max(MinRank - Constraint.Weight, RankBase[Constraint.Src]);
                if (MinRank == RankBase[Constraint.Src]) {
                    continue;
                }
                RankBase[Constraint.Src] = MinRank;
                bUpdated = true;
                if (bDumpDetail) {
                    UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                           TEXT("Sugiyama[%s]      src=%s rank->%d"), Label,
                           *BuildNodeKeyString(Graph.Nodes[Constraint.Src].Key),
                           RankBase[Constraint.Src]);
                }
            }
            if (bDumpDetail) {
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                       TEXT("Sugiyama[%s] AssignLayers: backward pass sweep %d "
                            "complete bUpdated=%d"),
                       Label, Sweep, bUpdated ? 1 : 0);
            }
        }
    } else {
        for (int32 NodeIndex : TopoOrder) {
            for (int32 EdgeIndex : OutEdges[NodeIndex]) {
                const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                const int32 Dst = Edge.Dst;
                // Enforce "child is at least one rank below parent".
                const int32 EdgeWeight = Edge.MinLen;
                RankBase[Dst] =
                    FMath::Max(RankBase[Dst], RankBase[NodeIndex] + EdgeWeight);
            }
        }
    }

    // Optionally log rank bases for debugging.
    if (bDumpDetail) {
        // Helpful trace of the topo order and RankBase assignments.
        for (int32 OrderIndex = 0; OrderIndex < TopoOrder.Num(); ++OrderIndex) {
            const int32 NodeIndex = TopoOrder[OrderIndex];
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] TopoOrder[%d]: node=%s rankBase=%d"), Label,
                   OrderIndex, *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key),
                   RankBase[NodeIndex]);
        }
    }

    // Apply final ranks and compute the maximum rank.
    int32 MaxRank = 0;
    for (int32 Index = 0; Index < NodeCount; ++Index) {
        Graph.Nodes[Index].Rank = RankBase[Index];
        MaxRank = FMath::Max(MaxRank, RankBase[Index]);
        if (bDumpDetail) {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] Rank: node=%s rank=%d"), Label,
                   *BuildNodeKeyString(Graph.Nodes[Index].Key), RankBase[Index]);
        }
    }

    // Return the maximum rank for downstream sizing.
    return MaxRank;
}

// Append exec-tail dummy nodes so terminal exec nodes reach the maximum rank.
void AddTerminalExecTailNodes(FSugiyamaGraph &Graph, int32 MaxRank, const TCHAR *Label)
{
    // Skip padding when the graph is empty or already flat.
    if (MaxRank <= 0 || Graph.Nodes.IsEmpty()) {
        return;
    }

    // Count outgoing exec edges per node from the original edge list.
    const int32 OriginalEdgeCount = Graph.Edges.Num();
    TArray<int32> OutExecCounts;
    OutExecCounts.Init(0, Graph.Nodes.Num());
    for (int32 EdgeIndex = 0; EdgeIndex < OriginalEdgeCount; ++EdgeIndex) {
        const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
        if (Edge.Kind != EEdgeKind::Exec) {
            continue;
        }
        if (Edge.Src == Edge.Dst) {
            continue;
        }
        if (!OutExecCounts.IsValidIndex(Edge.Src)) {
            continue;
        }
        ++OutExecCounts[Edge.Src];
    }

    // Add a synthetic exec tail per terminal exec node below the max rank.
    const int32 OriginalNodeCount = Graph.Nodes.Num();
    int32 TailAdded = 0;
    for (int32 NodeIndex = 0; NodeIndex < OriginalNodeCount; ++NodeIndex) {
        const FSugiyamaNode &Node = Graph.Nodes[NodeIndex];
        if (Node.bIsDummy || !Node.bHasExecPins) {
            continue;
        }
        if (OutExecCounts[NodeIndex] > 0) {
            continue;
        }
        if (Node.Rank >= MaxRank) {
            continue;
        }
        const FNodeKey NodeKey = Node.Key;
        const FString NodeKeyString = BuildNodeKeyString(NodeKey);
        const FString TailSeed = FString::Printf(TEXT("ExecTail|%s"), *NodeKeyString);
        FSugiyamaNode Tail;
        Tail.Id = Graph.Nodes.Num();
        Tail.Key = MakeSyntheticNodeKey(TailSeed);
        Tail.Name = TEXT("Dummy");
        Tail.InputPinCount = 1;
        Tail.OutputPinCount = 0;
        Tail.ExecInputPinCount = 1;
        Tail.ExecOutputPinCount = 0;
        Tail.bHasExecPins = true;
        Tail.Size = FVector2f::ZeroVector;
        Tail.Rank = MaxRank;
        Tail.Order = 0;
        Tail.bIsDummy = true;
        Graph.Nodes.Add(Tail);
        FSugiyamaEdge TailEdge;
        TailEdge.Src = NodeIndex;
        TailEdge.Dst = Tail.Id;
        TailEdge.SrcPin = MakeDummyPinKey(NodeKey, EPinDirection::Output);
        TailEdge.DstPin = MakeDummyPinKey(Tail.Key, EPinDirection::Input);
        TailEdge.SrcPinIndex = 0;
        TailEdge.DstPinIndex = 0;
        TailEdge.Kind = EEdgeKind::Exec;
        TailEdge.MinLen = 1;
        TailEdge.StableKey = TailSeed;
        Graph.Edges.Add(TailEdge);
        ++TailAdded;
    }

    // Log how many tail nodes were created for exec sinks.
    if (TailAdded > 0) {
        UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] ExecTail: added=%d"),
               Label, TailAdded);
    }
}

// Insert dummy nodes so every edge spans a single rank.
void SplitLongEdges(FSugiyamaGraph &Graph, const TCHAR *Label)
{
    const int32 OriginalNodeCount = Graph.Nodes.Num();
    const int32 OriginalEdgeCount = Graph.Edges.Num();
    int32 DummyAdded = 0;
    int32 SplitEdgeCount = 0;
    const bool bDumpDetail = ShouldDumpDetail(OriginalNodeCount, OriginalEdgeCount);

    // Accumulate edges with inserted dummy segments.
    TArray<FSugiyamaEdge> NewEdges;
    NewEdges.Reserve(Graph.Edges.Num());

    // Walk edges and split those that span multiple ranks.
    for (const FSugiyamaEdge &Edge : Graph.Edges) {
        const int32 SrcRank = Graph.Nodes[Edge.Src].Rank;
        const int32 DstRank = Graph.Nodes[Edge.Dst].Rank;
        const int32 RankDiff = DstRank - SrcRank;

        // Keep edges that already span a single layer.
        if (RankDiff <= 1) {
            NewEdges.Add(Edge);
            continue;
        }

        // Track split edges and dummy node count.
        ++SplitEdgeCount;
        DummyAdded += RankDiff - 1;
        if (bDumpDetail) {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] SplitLongEdges: edge %s -> %s rankDiff=%d"),
                   Label, *BuildNodeKeyString(Graph.Nodes[Edge.Src].Key),
                   *BuildNodeKeyString(Graph.Nodes[Edge.Dst].Key), RankDiff);
        }

        // Capture edge kind and start the dummy chain from the source node.
        const bool bExecEdge = Edge.Kind == EEdgeKind::Exec;
        int32 Prev = Edge.Src;
        // Insert a chain of dummy nodes so each segment spans one rank.
        for (int32 Step = 1; Step < RankDiff; ++Step) {
            FSugiyamaNode Dummy;
            Dummy.Id = Graph.Nodes.Num();
            Dummy.Key = MakeSyntheticNodeKey(
                FString::Printf(TEXT("Dummy|%s|%d"), *Edge.StableKey, Step));
            Dummy.Name = TEXT("Dummy");
            Dummy.InputPinCount = 1;
            Dummy.OutputPinCount = 1;
            Dummy.ExecInputPinCount = bExecEdge ? 1 : 0;
            Dummy.ExecOutputPinCount = bExecEdge ? 1 : 0;
            Dummy.bHasExecPins = bExecEdge;
            Dummy.Size = FVector2f::ZeroVector;
            Dummy.Rank = SrcRank + Step;
            Dummy.Order = 0;
            Dummy.bIsDummy = true;
            Graph.Nodes.Add(Dummy);

            // Emit the edge segment connecting the previous node to this dummy.
            FSugiyamaEdge Segment;
            Segment.Src = Prev;
            Segment.Dst = Dummy.Id;
            if (Prev == Edge.Src) {
                Segment.SrcPin = Edge.SrcPin;
                Segment.SrcPinIndex = Edge.SrcPinIndex;
            } else {
                Segment.SrcPin =
                    MakeDummyPinKey(Graph.Nodes[Prev].Key, EPinDirection::Output);
                Segment.SrcPinIndex = 0;
            }
            Segment.DstPin = MakeDummyPinKey(Dummy.Key, EPinDirection::Input);
            Segment.DstPinIndex = 0;
            Segment.Kind = Edge.Kind;
            Segment.MinLen = Edge.MinLen;
            Segment.StableKey =
                FString::Printf(TEXT("%s|seg%d"), *Edge.StableKey, Step);
            NewEdges.Add(Segment);
            Prev = Dummy.Id;
        }

        // Close the chain by linking the final dummy (or src) to the original
        // destination.
        FSugiyamaEdge FinalEdge;
        FinalEdge.Src = Prev;
        FinalEdge.Dst = Edge.Dst;
        FinalEdge.SrcPin =
            MakeDummyPinKey(Graph.Nodes[Prev].Key, EPinDirection::Output);
        FinalEdge.SrcPinIndex = 0;
        FinalEdge.DstPin = Edge.DstPin;
        FinalEdge.DstPinIndex = Edge.DstPinIndex;
        FinalEdge.Kind = Edge.Kind;
        FinalEdge.MinLen = Edge.MinLen;
        FinalEdge.StableKey =
            FString::Printf(TEXT("%s|seg%d"), *Edge.StableKey, RankDiff);
        NewEdges.Add(FinalEdge);
    }

    // Replace the graph edges with the split edge list.
    Graph.Edges = MoveTemp(NewEdges);

    // Log split edge summary and any dummy nodes created.
    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("Sugiyama[%s] SplitLongEdges: nodes=%d (dummyAdded=%d) ")
               TEXT("edges=%d (splitEdges=%d)"),
           Label, Graph.Nodes.Num(), DummyAdded, Graph.Edges.Num(), SplitEdgeCount);
    if (bDumpDetail && DummyAdded > 0) {
        for (int32 Index = OriginalNodeCount; Index < Graph.Nodes.Num(); ++Index) {
            const FSugiyamaNode &Node = Graph.Nodes[Index];
            if (!Node.bIsDummy) {
                continue;
            }
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] DummyNode[%d]: key=%s rank=%d"), Label, Index,
                   *BuildNodeKeyString(Node.Key), Node.Rank);
        }
    }
}

// Full Sugiyama pipeline: break cycles, layer, split long edges, and order.
int32 RunSugiyama(FSugiyamaGraph &Graph, int32 NumSweeps, const TCHAR *Label,
                  int32 VariableGetMinLength)
{
    // Emit the initial graph state for debugging.
    LogSugiyamaSummary(Label, TEXT("start"), Graph);
    LogSugiyamaNodes(Label, TEXT("start"), Graph);
    LogSugiyamaEdges(Label, TEXT("start"), Graph);

    // Break cycles, normalize edge directions, and cache min lengths for layering.
    RemoveCycles(Graph, Label);
    ApplyEdgeDirections(Graph);
    UpdateEdgeMinLengths(Graph, VariableGetMinLength);
    LogSugiyamaEdges(Label, TEXT("afterCycle"), Graph);
    int32 MaxRank = AssignLayers(Graph, Label, VariableGetMinLength);
    // Add exec tail nodes so terminal exec nodes align to the max rank.
    AddTerminalExecTailNodes(Graph, MaxRank, Label);
    // Insert dummy nodes so all edges span single ranks.
    SplitLongEdges(Graph, Label);

    // Update MaxRank from any newly inserted dummy nodes.
    for (const FSugiyamaNode &Node : Graph.Nodes) {
        MaxRank = FMath::Max(MaxRank, Node.Rank);
    }

    // Initialize and refine rank orders to reduce crossings.
    TArray<TArray<int32>> RankNodes;
    AssignInitialOrder(Graph, MaxRank, RankNodes, Label);
    RunCrossingReduction(Graph, MaxRank, NumSweeps, RankNodes, Label);
    // Emit final graph state for debugging.
    LogSugiyamaSummary(Label, TEXT("final"), Graph);
    LogSugiyamaNodes(Label, TEXT("final"), Graph);
    LogSugiyamaEdges(Label, TEXT("final"), Graph);
    return MaxRank;
}

// Build working nodes for a layout component and map ids to indices.
bool BuildWorkNodes(const FLayoutGraph &Graph, const TArray<int32> &ComponentNodeIds,
                    TArray<FLayoutNode> &OutNodes,
                    TMap<int32, int32> &OutLocalIdToIndex, FString *OutError)
{
    OutNodes.Reset();
    OutLocalIdToIndex.Reset();

    // Build a lookup from graph node id to node index.
    TMap<int32, int32> GraphIdToIndex;
    GraphIdToIndex.Reserve(Graph.Nodes.Num());
    for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index) {
        GraphIdToIndex.Add(Graph.Nodes[Index].Id, Index);
    }

    // Sort and unique the component node ids for deterministic output order.
    TArray<int32> SortedIds = ComponentNodeIds;
    SortedIds.Sort();
    SortedIds.SetNum(Algo::Unique(SortedIds));

    // Reserve output storage based on the unique node ids.
    OutNodes.Reserve(SortedIds.Num());
    OutLocalIdToIndex.Reserve(SortedIds.Num());

    // Copy graph nodes into a compact working array for layout.
    for (int32 NodeId : SortedIds) {
        const int32 GraphIndex = GraphIdToIndex.FindRef(NodeId);
        if (GraphIndex == INDEX_NONE) {
            if (OutError) {
                *OutError = FString::Printf(
                    TEXT("Missing node id in layout graph: %d."), NodeId);
            }
            return false;
        }

        // Copy input fields into a working node and reset layout outputs.
        const FLayoutNode &GraphNode = Graph.Nodes[GraphIndex];
        const int32 LocalIndex = OutNodes.Num();
        FLayoutNode Node;
        Node.Id = GraphNode.Id;
        Node.Key = GraphNode.Key;
        Node.Name = GraphNode.Name;
        Node.Size = FVector2f(FMath::Max(0.0f, GraphNode.Size.X),
                              FMath::Max(0.0f, GraphNode.Size.Y));
        Node.Position = GraphNode.Position;
        Node.bHasExecPins = GraphNode.bHasExecPins;
        Node.bIsVariableGet = GraphNode.bIsVariableGet;
        Node.ExecInputPinCount = GraphNode.ExecInputPinCount;
        Node.ExecOutputPinCount = GraphNode.ExecOutputPinCount;
        Node.InputPinCount = GraphNode.InputPinCount;
        Node.OutputPinCount = GraphNode.OutputPinCount;
        Node.GlobalRank = 0;
        Node.GlobalOrder = 0;
        OutLocalIdToIndex.Add(Node.Id, LocalIndex);
        OutNodes.Add(Node);
    }

    // Optionally log the working nodes for verbose diagnostics.
    if (OutNodes.Num() <= kVerboseDumpNodeLimit) {
        for (const FLayoutNode &Node : OutNodes) {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("LayoutComponent: node graphId=%d key=%s size=(%.1f,%.1f) ")
                       TEXT("pos=(%.1f,%.1f) execPins=%d execIn=%d execOut=%d "
                            "inputPins=%d outputPins=%d"),
                   Node.Id, *BuildNodeKeyString(Node.Key), Node.Size.X, Node.Size.Y,
                   Node.Position.X, Node.Position.Y, Node.bHasExecPins ? 1 : 0,
                   Node.ExecInputPinCount, Node.ExecOutputPinCount, Node.InputPinCount,
                   Node.OutputPinCount);
        }
    }

    // Signal success with populated outputs.
    return true;
}

// Handle the single-node component fast path.
bool TryHandleSingleNode(const TArray<FLayoutNode> &Nodes,
                         FLayoutComponentResult &OutResult)
{
    // Fast path for a component that contains exactly one node.
    if (Nodes.Num() != 1) {
        return false;
    }

    // Populate results directly from the lone node.
    const FLayoutNode &Solo = Nodes[0];
    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("LayoutComponent: single node fast path graphId=%d"), Solo.Id);
    OutResult.NodePositions.Add(Solo.Id, Solo.Position);
    const FVector2f Min = Solo.Position;
    const FVector2f Max = Solo.Position + Solo.Size;
    OutResult.Bounds += FBox2f(Min, Max);
    return true;
}

// Resolve per-type horizontal spacing, honoring the legacy single-value setting.
void ResolveNodeSpacingX(const FLayoutSettings &Settings, float &OutSpacingExec,
                         float &OutSpacingData)
{
    // Start with the per-type spacing values.
    OutSpacingExec = Settings.NodeSpacingXExec;
    OutSpacingData = Settings.NodeSpacingXData;

    // Detect when legacy spacing should override default exec/data values.
    const bool bExecDefault = FMath::IsNearlyEqual(
        OutSpacingExec, BlueprintAutoLayout::Defaults::DefaultNodeSpacingXExec);
    const bool bDataDefault = FMath::IsNearlyEqual(
        OutSpacingData, BlueprintAutoLayout::Defaults::DefaultNodeSpacingXData);
    const bool bLegacyNonDefault = !FMath::IsNearlyEqual(
        Settings.NodeSpacingX, BlueprintAutoLayout::Defaults::DefaultNodeSpacingX);

    // Apply legacy spacing when only the combined value is customized.
    if (bExecDefault && bDataDefault && bLegacyNonDefault) {
        OutSpacingExec = Settings.NodeSpacingX;
        OutSpacingData = Settings.NodeSpacingX;
    }

    // Clamp spacing to non-negative values.
    OutSpacingExec = FMath::Max(0.0f, OutSpacingExec);
    OutSpacingData = FMath::Max(0.0f, OutSpacingData);
}

// Build working edge list with stable pin keys for a component.
void BuildWorkEdges(const FLayoutGraph &Graph, const TArray<FLayoutNode> &Nodes,
                    const TMap<int32, int32> &LocalIdToIndex,
                    TArray<FLayoutEdge> &OutEdges)
{
    OutEdges.Reset();
    OutEdges.Reserve(Graph.Edges.Num());

    // Copy edges that connect nodes within the component and build stable pin keys.
    for (const FLayoutEdge &Edge : Graph.Edges) {
        const int32 SrcIndex = LocalIdToIndex.FindRef(Edge.Src);
        const int32 DstIndex = LocalIdToIndex.FindRef(Edge.Dst);
        if (SrcIndex == INDEX_NONE || DstIndex == INDEX_NONE) {
            continue;
        }
        if (SrcIndex == DstIndex) {
            continue;
        }

        // Populate a localized edge record for the layout graph.
        FLayoutEdge LocalEdge;
        LocalEdge.Src = SrcIndex;
        LocalEdge.Dst = DstIndex;
        LocalEdge.Kind = Edge.Kind;
        LocalEdge.SrcPinIndex = FMath::Max(0, Edge.SrcPinIndex);
        LocalEdge.DstPinIndex = FMath::Max(0, Edge.DstPinIndex);
        LocalEdge.SrcPinName = Edge.SrcPinName;
        LocalEdge.DstPinName = Edge.DstPinName;
        const FPinKey SrcPinKey =
            MakePinKey(Nodes[SrcIndex].Key, EPinDirection::Output, LocalEdge.SrcPinName,
                       LocalEdge.SrcPinIndex);
        const FPinKey DstPinKey =
            MakePinKey(Nodes[DstIndex].Key, EPinDirection::Input, LocalEdge.DstPinName,
                       LocalEdge.DstPinIndex);
        LocalEdge.StableKey =
            BuildPinKeyString(SrcPinKey) + TEXT("->") + BuildPinKeyString(DstPinKey);
        OutEdges.Add(MoveTemp(LocalEdge));
    }

    // Sort edges to keep downstream passes deterministic.
    OutEdges.Sort([](const FLayoutEdge &A, const FLayoutEdge &B) {
        if (A.StableKey != B.StableKey) {
            return A.StableKey < B.StableKey;
        }
        if (A.Src != B.Src) {
            return A.Src < B.Src;
        }
        if (A.Dst != B.Dst) {
            return A.Dst < B.Dst;
        }
        return A.SrcPinIndex < B.SrcPinIndex;
    });

    // Optionally log edge details for troubleshooting.
    if (ShouldDumpDetail(Nodes.Num(), OutEdges.Num())) {
        for (int32 EdgeIndex = 0; EdgeIndex < OutEdges.Num(); ++EdgeIndex) {
            const FLayoutEdge &Edge = OutEdges[EdgeIndex];
            const TCHAR *Kind =
                Edge.Kind == EEdgeKind::Exec ? TEXT("exec") : TEXT("data");
            const FPinKey SrcPinKey =
                MakePinKey(Nodes[Edge.Src].Key, EPinDirection::Output, Edge.SrcPinName,
                           Edge.SrcPinIndex);
            const FPinKey DstPinKey =
                MakePinKey(Nodes[Edge.Dst].Key, EPinDirection::Input, Edge.DstPinName,
                           Edge.DstPinIndex);
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("LayoutComponent: edge[%d] %s srcId=%d dstId=%d ")
                       TEXT("srcPin=%s dstPin=%s stable=%s"),
                   EdgeIndex, Kind, Nodes[Edge.Src].Id, Nodes[Edge.Dst].Id,
                   *BuildPinKeyString(SrcPinKey), *BuildPinKeyString(DstPinKey),
                   *Edge.StableKey);
        }
    }

    // Summarize the edge mix for verbose diagnostics.
    int32 ExecEdgeCount = 0;
    int32 DataEdgeCount = 0;
    for (const FLayoutEdge &Edge : OutEdges) {
        if (Edge.Kind == EEdgeKind::Exec) {
            ++ExecEdgeCount;
        } else if (Edge.Kind == EEdgeKind::Data) {
            ++DataEdgeCount;
        }
    }
    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("LayoutComponent: working nodes=%d edges=%d (exec=%d data=%d)"),
           Nodes.Num(), OutEdges.Num(), ExecEdgeCount, DataEdgeCount);
}

// Build a Sugiyama graph from working nodes and edges.
void BuildSugiyamaGraph(const TArray<FLayoutNode> &Nodes,
                        const TArray<FLayoutEdge> &Edges, FSugiyamaGraph &OutGraph)
{
    OutGraph.Nodes.Reset();
    OutGraph.Edges.Reset();
    OutGraph.Nodes.Reserve(Nodes.Num());
    OutGraph.Edges.Reserve(Edges.Num());

    // Copy node attributes into the Sugiyama working graph.
    for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
        const FLayoutNode &WorkNode = Nodes[Index];
        FSugiyamaNode Node;
        Node.Id = Index;
        Node.Key = WorkNode.Key;
        Node.Name = WorkNode.Name;
        Node.ExecInputPinCount = FMath::Max(0, WorkNode.ExecInputPinCount);
        Node.ExecOutputPinCount = FMath::Max(0, WorkNode.ExecOutputPinCount);
        Node.InputPinCount = FMath::Max(0, WorkNode.InputPinCount);
        Node.OutputPinCount = FMath::Max(0, WorkNode.OutputPinCount);
        Node.bHasExecPins = WorkNode.bHasExecPins;
        Node.bIsVariableGet = WorkNode.bIsVariableGet;
        Node.Size = WorkNode.Size;
        Node.SourceIndex = Index;
        OutGraph.Nodes.Add(Node);
    }

    // Copy edge metadata into the Sugiyama working graph.
    for (const FLayoutEdge &Edge : Edges) {
        FSugiyamaEdge GraphEdge;
        GraphEdge.Src = Edge.Src;
        GraphEdge.Dst = Edge.Dst;
        GraphEdge.SrcPin = MakePinKey(Nodes[Edge.Src].Key, EPinDirection::Output,
                                      Edge.SrcPinName, Edge.SrcPinIndex);
        GraphEdge.DstPin = MakePinKey(Nodes[Edge.Dst].Key, EPinDirection::Input,
                                      Edge.DstPinName, Edge.DstPinIndex);
        GraphEdge.SrcPinIndex = Edge.SrcPinIndex;
        GraphEdge.DstPinIndex = Edge.DstPinIndex;
        GraphEdge.Kind = Edge.Kind;
        GraphEdge.StableKey = Edge.StableKey;
        OutGraph.Edges.Add(MoveTemp(GraphEdge));
    }
}

// Apply Sugiyama ranks and orders back to working nodes.
void ApplySugiyamaRanks(const FSugiyamaGraph &Graph, TArray<FLayoutNode> &Nodes)
{
    // Clear any previous ranks before applying Sugiyama results.
    for (FLayoutNode &Node : Nodes) {
        Node.GlobalRank = 0;
        Node.GlobalOrder = 0;
    }

    // Map non-dummy Sugiyama nodes back onto the original work nodes.
    for (const FSugiyamaNode &Node : Graph.Nodes) {
        if (Node.bIsDummy || Node.SourceIndex == INDEX_NONE) {
            continue;
        }
        if (!Nodes.IsValidIndex(Node.SourceIndex)) {
            continue;
        }
        Nodes[Node.SourceIndex].GlobalRank = FMath::Max(0, Node.Rank);
        Nodes[Node.SourceIndex].GlobalOrder = FMath::Max(0, Node.Order);
    }
}

// Log global rank/order values for the component nodes.
void LogGlobalRankOrders(const TArray<FLayoutNode> &Nodes)
{
    for (const FLayoutNode &Node : Nodes) {
        const TCHAR *Name = Node.Name.IsEmpty() ? TEXT("<unnamed>") : *Node.Name;
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("LayoutComponent: global node key=%s name=%s rank=%d order=%d"),
               *BuildNodeKeyString(Node.Key), Name, Node.GlobalRank, Node.GlobalOrder);
    }
}

// Apply computed positions and update bounds for the component result.
void ApplyFinalPositions(const TMap<int32, FVector2f> &PrimaryPositions,
                         const TMap<int32, FVector2f> &SecondaryPositions,
                         const FVector2f &AnchorOffset,
                         const TArray<FLayoutNode> &Nodes,
                         FLayoutComponentResult &OutResult)
{
    // Track nodes placed by the primary map to avoid double placement.
    TSet<int32> Positioned;
    // Apply primary positions first, updating bounds as we go.
    for (const TPair<int32, FVector2f> &Pair : PrimaryPositions) {
        const int32 NodeIndex = Pair.Key;
        if (!Nodes.IsValidIndex(NodeIndex)) {
            continue;
        }
        Positioned.Add(NodeIndex);
        const FVector2f Pos = Pair.Value + AnchorOffset;
        OutResult.NodePositions.Add(Nodes[NodeIndex].Id, Pos);
        const FVector2f Min = Pos;
        const FVector2f Max = Pos + Nodes[NodeIndex].Size;
        OutResult.Bounds += FBox2f(Min, Max);
    }

    // Apply any remaining positions from the secondary map.
    for (const TPair<int32, FVector2f> &Pair : SecondaryPositions) {
        const int32 NodeIndex = Pair.Key;
        if (!Nodes.IsValidIndex(NodeIndex) || Positioned.Contains(NodeIndex)) {
            continue;
        }
        const FVector2f Pos = Pair.Value + AnchorOffset;
        OutResult.NodePositions.Add(Nodes[NodeIndex].Id, Pos);
        const FVector2f Min = Pos;
        const FVector2f Max = Pos + Nodes[NodeIndex].Size;
        OutResult.Bounds += FBox2f(Min, Max);
    }

    // Optionally log the final node positions.
    if (Nodes.Num() <= kVerboseDumpNodeLimit) {
        for (const FLayoutNode &Node : Nodes) {
            const FVector2f *Pos = OutResult.NodePositions.Find(Node.Id);
            if (!Pos) {
                continue;
            }
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("LayoutComponent: final node graphId=%d key=%s ")
                       TEXT("pos=(%.1f,%.1f) size=(%.1f,%.1f)"),
                   Node.Id, *BuildNodeKeyString(Node.Key), Pos->X, Pos->Y, Node.Size.X,
                   Node.Size.Y);
        }
    }

    // Log the final bounds and node count for diagnostics.
    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("LayoutComponent: positioned=%d boundsMin=(%.1f,%.1f) ")
               TEXT("boundsMax=(%.1f,%.1f)"),
           OutResult.NodePositions.Num(), OutResult.Bounds.Min.X,
           OutResult.Bounds.Min.Y, OutResult.Bounds.Max.X, OutResult.Bounds.Max.Y);
}

// End of anonymous namespace helpers.
} // namespace
// Lay out a connected component using a single Sugiyama pass.
bool LayoutComponent(const FLayoutGraph &Graph, const TArray<int32> &ComponentNodeIds,
                     const FLayoutSettings &Settings, FLayoutComponentResult &OutResult,
                     FString *OutError)
{

    // Reset result and log the component context.
    OutResult = FLayoutComponentResult();
    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("LayoutComponent: componentNodes=%d graphNodes=%d graphEdges=%d"),
           ComponentNodeIds.Num(), Graph.Nodes.Num(), Graph.Edges.Num());

    // Validate input before building working structures.
    if (ComponentNodeIds.IsEmpty()) {
        if (OutError) {
            *OutError = TEXT("Layout component is empty.");
        }
        return false;
    }

    // Build working nodes and edge indices for the component.
    TArray<FLayoutNode> Nodes;
    TMap<int32, int32> LocalIdToIndex;
    if (!BuildWorkNodes(Graph, ComponentNodeIds, Nodes, LocalIdToIndex, OutError)) {
        return false;
    }

    // Fast path for single-node components.
    if (TryHandleSingleNode(Nodes, OutResult)) {
        return true;
    }

    // Build working edges and spacing parameters.
    TArray<FLayoutEdge> Edges;
    BuildWorkEdges(Graph, Nodes, LocalIdToIndex, Edges);

    // Resolve spacing inputs and clamp to non-negative values.
    float NodeSpacingXExec = 0.0f;
    float NodeSpacingXData = 0.0f;
    ResolveNodeSpacingX(Settings, NodeSpacingXExec, NodeSpacingXData);
    const float NodeSpacingYExec = FMath::Max(0.0f, Settings.NodeSpacingYExec);
    const float NodeSpacingYData = FMath::Max(0.0f, Settings.NodeSpacingYData);
    const int32 VariableGetMinLength = FMath::Max(0, Settings.VariableGetMinLength);

    // Run Sugiyama layout to assign global ranks and orders.
    FSugiyamaGraph SugiyamaGraph;
    BuildSugiyamaGraph(Nodes, Edges, SugiyamaGraph);
    RunSugiyama(SugiyamaGraph, kSugiyamaSweeps, TEXT("Component"),
                VariableGetMinLength);
    ApplySugiyamaRanks(SugiyamaGraph, Nodes);
    LogGlobalRankOrders(Nodes);

    // Convert ranks to actual positions and apply the anchor offset.
    const FGlobalPlacement GlobalPlacement = PlaceGlobalRankOrderCompact(
        Nodes, Edges, NodeSpacingXExec, NodeSpacingXData, NodeSpacingYExec,
        NodeSpacingYData, Settings.RankAlignment);
    const FVector2f AnchorOffset = ComputeGlobalAnchorOffset(Nodes, GlobalPlacement);
    TMap<int32, FVector2f> EmptyPositions;
    ApplyFinalPositions(EmptyPositions, GlobalPlacement.Positions, AnchorOffset, Nodes,
                        OutResult);
    return true;
}
} // namespace GraphLayout
