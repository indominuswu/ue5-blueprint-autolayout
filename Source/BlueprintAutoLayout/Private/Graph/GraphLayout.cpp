// Copyright Epic Games, Inc. All Rights Reserved.

#include "Graph/GraphLayout.h"

#include "Graph/GraphLayoutKeyUtils.h"

#include "BlueprintAutoLayoutLog.h"

#include "Algo/Unique.h"
#include "Misc/Crc.h"

namespace GraphLayout
{
namespace
{
// Tuning constants for the Sugiyama sweeps used during layout.
constexpr int32 kSugiyamaSweeps = 8;
constexpr int32 kVerboseDumpNodeLimit = 120;
constexpr int32 kVerboseDumpEdgeLimit = 240;
constexpr int32 kVerboseCrossingDetailLimit = 64;

int32 CompareNodeKey(const FNodeKey &A, const FNodeKey &B)
{
    return KeyUtils::CompareNodeKey(A, B);
}

bool NodeKeyLess(const FNodeKey &A, const FNodeKey &B)
{
    return KeyUtils::NodeKeyLess(A, B);
}

FString BuildNodeKeyString(const FNodeKey &Key)
{
    return KeyUtils::BuildNodeKeyString(Key);
}

enum class EPinDirection : uint8
{
    Input = 0,
    Output = 1
};

struct FPinKey
{
    FNodeKey NodeKey;
    EPinDirection Direction = EPinDirection::Input;
    FName PinName;
    int32 PinIndex = 0;
};

int32 ComparePinKey(const FPinKey &A, const FPinKey &B)
{
    return KeyUtils::ComparePinKey(A.NodeKey, static_cast<int32>(A.Direction), A.PinName, A.PinIndex, B.NodeKey,
                                   static_cast<int32>(B.Direction), B.PinName, B.PinIndex);
}

bool PinKeyLess(const FPinKey &A, const FPinKey &B)
{
    return ComparePinKey(A, B) < 0;
}

FPinKey MakePinKey(const FNodeKey &Owner, EPinDirection Direction, const FName &PinName, int32 PinIndex)
{
    FPinKey Key;
    Key.NodeKey = Owner;
    Key.Direction = Direction;
    Key.PinName = PinName;
    Key.PinIndex = PinIndex;
    return Key;
}

FString BuildPinKeyString(const FPinKey &Key)
{
    const TCHAR *Dir = Key.Direction == EPinDirection::Input ? TEXT("I") : TEXT("O");
    return KeyUtils::BuildPinKeyString(Key.NodeKey, Dir, Key.PinName, Key.PinIndex);
}

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

FNodeKey MakeSyntheticNodeKey(const FString &Seed)
{
    FNodeKey Key;
    Key.Guid = MakeDeterministicGuid(Seed);
    return Key;
}

FPinKey MakeDummyPinKey(const FNodeKey &Owner, EPinDirection Direction)
{
    return MakePinKey(Owner, Direction, FName(TEXT("Dummy")), 0);
}

// Internal node representation used during layout to avoid mutating inputs.
struct FWorkNode
{
    int32 LocalIndex = INDEX_NONE;
    int32 GraphId = INDEX_NONE;
    FNodeKey Key;
    FString Name;
    FVector2f Size = FVector2f::ZeroVector;
    FVector2f OriginalPosition = FVector2f::ZeroVector;
    bool bHasExecPins = false;
    int32 InputPinCount = 0;
    int32 ExecInputPinCount = 0;
    int32 OutputPinCount = 0;
    int32 ExecOutputPinCount = 0;
    int32 GlobalRank = 0;
    int32 GlobalOrder = 0;
};

// Internal edge representation with resolved node indices and pin keys.
struct FWorkEdge
{
    int32 Src = INDEX_NONE;
    int32 Dst = INDEX_NONE;
    EEdgeKind Kind = EEdgeKind::Data;
    int32 SrcPinIndex = 0;
    int32 DstPinIndex = 0;
    FName SrcPinName;
    FName DstPinName;
    FPinKey SrcPinKey;
    FPinKey DstPinKey;
    FString StableKey;
};

// Data used by the Sugiyama-style layered layout.
struct FSugiyamaNode
{
    int32 Id = INDEX_NONE;
    FNodeKey Key;
    FString Name;
    int32 OutputPinCount = 0;
    int32 InputPinCount = 0;
    int32 ExecOutputPinCount = 0;
    int32 ExecInputPinCount = 0;
    bool bHasExecPins = false;
    FVector2f Size = FVector2f::ZeroVector;
    int32 Rank = 0;
    int32 Order = 0;
    bool bIsDummy = false;
    int32 SourceIndex = INDEX_NONE;
};

struct FSugiyamaEdge
{
    int32 Src = INDEX_NONE;
    int32 Dst = INDEX_NONE;
    FPinKey SrcPin;
    FPinKey DstPin;
    int32 SrcPinIndex = 0;
    int32 DstPinIndex = 0;
    EEdgeKind Kind = EEdgeKind::Data;
    FString StableKey;
    bool bReversed = false;
};

struct FSugiyamaGraph
{
    TArray<FSugiyamaNode> Nodes;
    TArray<FSugiyamaEdge> Edges;
};

int32 CountDummyNodes(const FSugiyamaGraph &Graph)
{
    int32 Count = 0;
    for (const FSugiyamaNode &Node : Graph.Nodes) {
        if (Node.bIsDummy) {
            ++Count;
        }
    }
    return Count;
}

bool ShouldDumpDetail(int32 NodeCount, int32 EdgeCount)
{
    return NodeCount <= kVerboseDumpNodeLimit && EdgeCount <= kVerboseDumpEdgeLimit;
}

bool ShouldDumpSugiyamaDetail(const FSugiyamaGraph &Graph)
{
    return ShouldDumpDetail(Graph.Nodes.Num(), Graph.Edges.Num());
}

void LogSugiyamaSummary(const TCHAR *Label, const TCHAR *Stage, const FSugiyamaGraph &Graph)
{
    const int32 DummyCount = CountDummyNodes(Graph);
    UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] %s: nodes=%d edges=%d dummy=%d"), Label, Stage,
           Graph.Nodes.Num(), Graph.Edges.Num(), DummyCount);
}

void LogSugiyamaNodes(const TCHAR *Label, const TCHAR *Stage, const FSugiyamaGraph &Graph)
{
    if (!ShouldDumpSugiyamaDetail(Graph)) {
        return;
    }
    for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index) {
        const FSugiyamaNode &Node = Graph.Nodes[Index];
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("Sugiyama[%s] %s node[%d]: key=%s rank=%d order=%d ")
                   TEXT("size=(%.1f,%.1f) execOut=%d dummy=%d srcIndex=%d"),
               Label, Stage, Index, *BuildNodeKeyString(Node.Key), Node.Rank, Node.Order, Node.Size.X, Node.Size.Y,
               Node.ExecOutputPinCount, Node.bIsDummy ? 1 : 0, Node.SourceIndex);
    }
}

void LogSugiyamaEdges(const TCHAR *Label, const TCHAR *Stage, const FSugiyamaGraph &Graph)
{
    if (!ShouldDumpSugiyamaDetail(Graph)) {
        return;
    }
    for (int32 EdgeIndex = 0; EdgeIndex < Graph.Edges.Num(); ++EdgeIndex) {
        const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
        const FString SrcKey = Graph.Nodes.IsValidIndex(Edge.Src) ? BuildNodeKeyString(Graph.Nodes[Edge.Src].Key)
                                                                  : FString(TEXT("invalid"));
        const FString DstKey = Graph.Nodes.IsValidIndex(Edge.Dst) ? BuildNodeKeyString(Graph.Nodes[Edge.Dst].Key)
                                                                  : FString(TEXT("invalid"));
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("Sugiyama[%s] %s edge[%d]: %s -> %s srcPin=%s dstPin=%s ") TEXT("stable=%s"), Label, Stage,
               EdgeIndex, *SrcKey, *DstKey, *BuildPinKeyString(Edge.SrcPin), *BuildPinKeyString(Edge.DstPin),
               *Edge.StableKey);
    }
}

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
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] %s rank=%d order=%d node=%s"), Label, Stage,
                   Rank, OrderIndex, *BuildNodeKeyString(Node.Key));
        }
    }
}

// Keep deterministic ordering when building queues or layers by node key.
void InsertSortedByNodeKey(const TArray<FSugiyamaNode> &Nodes, TArray<int32> &List, int32 NodeIndex)
{
    int32 InsertIndex = 0;
    while (InsertIndex < List.Num() && NodeKeyLess(Nodes[List[InsertIndex]].Key, Nodes[NodeIndex].Key)) {
        ++InsertIndex;
    }
    List.Insert(NodeIndex, InsertIndex);
}

// Build out-edge lists while respecting temporary reversals used for cycle
// breaking, so DFS sees a consistent effective direction.
void BuildEffectiveOutEdges(const FSugiyamaGraph &Graph, TArray<TArray<int32>> &OutEdges)
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

    UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] RemoveCycles: start nodes=%d edges=%d"), Label,
           Graph.Nodes.Num(), Graph.Edges.Num());

    TArray<int32> NodeOrder;
    NodeOrder.Reserve(Graph.Nodes.Num());
    for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index) {
        NodeOrder.Add(Index);
    }
    NodeOrder.Sort([&](int32 A, int32 B) { return NodeKeyLess(Graph.Nodes[A].Key, Graph.Nodes[B].Key); });

    for (;;) {
        TArray<TArray<int32>> OutEdges;
        BuildEffectiveOutEdges(Graph, OutEdges);

        TArray<EVisitState> VisitState;
        VisitState.Init(EVisitState::Unvisited, Graph.Nodes.Num());

        struct FStackEntry
        {
            int32 NodeIndex = INDEX_NONE;
            int32 NextEdge = 0;
        };

        TArray<int32> BackEdges;

        for (int32 StartNode : NodeOrder) {
            if (VisitState[StartNode] != EVisitState::Unvisited) {
                continue;
            }

            TArray<FStackEntry> Stack;
            Stack.Add({StartNode, 0});
            VisitState[StartNode] = EVisitState::Visiting;

            while (!Stack.IsEmpty()) {
                FStackEntry &Entry = Stack.Last();
                if (Entry.NextEdge >= OutEdges[Entry.NodeIndex].Num()) {
                    VisitState[Entry.NodeIndex] = EVisitState::Done;
                    Stack.Pop();
                    continue;
                }

                const int32 EdgeIndex = OutEdges[Entry.NodeIndex][Entry.NextEdge++];
                const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                const int32 NextNode = Edge.bReversed ? Edge.Src : Edge.Dst;

                if (VisitState[NextNode] == EVisitState::Unvisited) {
                    VisitState[NextNode] = EVisitState::Visiting;
                    Stack.Add({NextNode, 0});
                } else if (VisitState[NextNode] == EVisitState::Visiting) {
                    BackEdges.Add(EdgeIndex);
                }
            }
        }

        if (BackEdges.IsEmpty()) {
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] RemoveCycles: done"), Label);
            break;
        }

        UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] RemoveCycles: backEdges=%d"), Label,
               BackEdges.Num());

        auto IsEdgeLess = [&](int32 A, int32 B) {
            const FSugiyamaEdge &EdgeA = Graph.Edges[A];
            const FSugiyamaEdge &EdgeB = Graph.Edges[B];

            const int32 SrcA = EdgeA.bReversed ? EdgeA.Dst : EdgeA.Src;
            const int32 SrcB = EdgeB.bReversed ? EdgeB.Dst : EdgeB.Src;
            const int32 DstA = EdgeA.bReversed ? EdgeA.Src : EdgeA.Dst;
            const int32 DstB = EdgeB.bReversed ? EdgeB.Src : EdgeB.Dst;
            const FPinKey &SrcPinA = EdgeA.bReversed ? EdgeA.DstPin : EdgeA.SrcPin;
            const FPinKey &SrcPinB = EdgeB.bReversed ? EdgeB.DstPin : EdgeB.SrcPin;
            const FPinKey &DstPinA = EdgeA.bReversed ? EdgeA.SrcPin : EdgeA.DstPin;
            const FPinKey &DstPinB = EdgeB.bReversed ? EdgeB.SrcPin : EdgeB.DstPin;

            int32 Compare = CompareNodeKey(Graph.Nodes[SrcA].Key, Graph.Nodes[SrcB].Key);
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

        int32 BestEdge = BackEdges[0];
        for (int32 EdgeIndex : BackEdges) {
            if (IsEdgeLess(EdgeIndex, BestEdge)) {
                BestEdge = EdgeIndex;
            }
        }
        const FSugiyamaEdge &ChosenEdge = Graph.Edges[BestEdge];
        const int32 EffectiveSrc = ChosenEdge.bReversed ? ChosenEdge.Dst : ChosenEdge.Src;
        const int32 EffectiveDst = ChosenEdge.bReversed ? ChosenEdge.Src : ChosenEdge.Dst;
        UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] RemoveCycles: reverse edge %s -> %s stable=%s"),
               Label, *BuildNodeKeyString(Graph.Nodes[EffectiveSrc].Key),
               *BuildNodeKeyString(Graph.Nodes[EffectiveDst].Key), *ChosenEdge.StableKey);
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

    for (TArray<int32> &EdgeList : OutEdges) {
        EdgeList.Sort([&](int32 A, int32 B) {
            const FSugiyamaEdge &EdgeA = Graph.Edges[A];
            const FSugiyamaEdge &EdgeB = Graph.Edges[B];
            int32 Compare = ComparePinKey(EdgeA.SrcPin, EdgeB.SrcPin);
            if (Compare != 0) {
                return Compare < 0;
            }
            Compare = CompareNodeKey(Graph.Nodes[EdgeA.Dst].Key, Graph.Nodes[EdgeB.Dst].Key);
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

bool EdgeHasFiniteMaxLen(const FSugiyamaGraph &Graph, const FSugiyamaEdge &Edge)
{
    if (Edge.Src == Edge.Dst) {
        return false;
    }
    const FSugiyamaNode &SrcNode = Graph.Nodes[Edge.Src];
    const FSugiyamaNode &DstNode = Graph.Nodes[Edge.Dst];
    return !SrcNode.bHasExecPins || !DstNode.bHasExecPins;
}

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
int32 AssignLayers(FSugiyamaGraph &Graph, const TCHAR *Label)
{
    const int32 NodeCount = Graph.Nodes.Num();
    if (NodeCount == 0) {
        // Nothing to rank if the graph has no nodes.
        return 0;
    }

    UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] AssignLayers: nodes=%d edges=%d"), Label, NodeCount,
           Graph.Edges.Num());
    const bool bDumpDetail = ShouldDumpSugiyamaDetail(Graph);
    const bool bUseMaxLenConstraints = GraphUsesFiniteMaxLen(Graph);
    if (bUseMaxLenConstraints) {
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("Sugiyama[%s] AssignLayers: maxLen constraints enabled (data nodes maxLen=1)"), Label);
    }

    // RankBase is the minimum layer each node can occupy based on constraints.
    TArray<int32> RankBase;
    RankBase.Init(0, NodeCount);

    // InDegree counts incoming edges for topological processing.
    TArray<int32> InDegree;
    InDegree.Init(0, NodeCount);

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

        for (int32 EdgeIndex : OutEdges[NodeIndex]) {
            FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
            const int32 Dst = Edge.Dst;
            InDegree[Dst] -= 1;
            if (InDegree[Dst] == 0) {
                InsertSortedByNodeKey(Graph.Nodes, Queue, Dst);
            }
        }
    }

    if (TopoOrder.Num() < NodeCount) {
        UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] TopoOrder: cycles/disconnected nodes topo=%d/%d"),
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
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] TopoOrder: remaining nodes=%d"), Label,
                   Remaining.Num());
            for (int32 Index = 0; Index < Remaining.Num(); ++Index) {
                const int32 NodeIndex = Remaining[Index];
                UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] TopoOrder remaining[%d]: node=%s"), Label,
                       Index, *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key));
            }
        } else {
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] TopoOrder: remaining nodes list suppressed"),
                   Label);
        }
        Remaining.Sort([&](int32 A, int32 B) { return NodeKeyLess(Graph.Nodes[A].Key, Graph.Nodes[B].Key); });
        if (bDumpDetail) {
            for (int32 Index = 0; Index < Remaining.Num(); ++Index) {
                const int32 NodeIndex = Remaining[Index];
                UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] TopoOrder remainingSorted[%d]: node=%s"),
                       Label, Index, *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key));
            }
        }
        TopoOrder.Append(Remaining);
        if (bDumpDetail) {
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] TopoOrder: appended remaining total=%d"), Label,
                   TopoOrder.Num());
        }
    }

    UE_LOG(LogBlueprintAutoLayout, VeryVerbose, TEXT("Sugiyama[%s] TopoOrder: begin total=%d"), Label, TopoOrder.Num());
    for (int32 OrderIndex = 0; OrderIndex < TopoOrder.Num(); ++OrderIndex) {
        const int32 NodeIndex = TopoOrder[OrderIndex];
        const FSugiyamaNode &Node = Graph.Nodes[NodeIndex];
        const TCHAR *Name = Node.Name.IsEmpty() ? TEXT("<unnamed>") : *Node.Name;
        UE_LOG(LogBlueprintAutoLayout, VeryVerbose, TEXT("Sugiyama[%s] TopoOrder[%d]: node=%s name=%s"), Label,
               OrderIndex, *BuildNodeKeyString(Node.Key), Name);
    }

    if (bUseMaxLenConstraints) {
        struct FConstraintEdge
        {
            int32 Src = INDEX_NONE;
            int32 Dst = INDEX_NONE;
            int32 Weight = 0;
            bool EdgeHasFiniteMaxLen = false;
        };

        TArray<FConstraintEdge> ForwardConstraints;
        ForwardConstraints.Reserve(Graph.Edges.Num());
        for (int32 NodeIndex : TopoOrder) {
            for (int32 EdgeIndex : OutEdges[NodeIndex]) {
                const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                if (Edge.Src == Edge.Dst) {
                    continue;
                }
                FConstraintEdge Constraint;
                Constraint.Src = Edge.Src;
                Constraint.Dst = Edge.Dst;
                Constraint.Weight = 1;
                Constraint.EdgeHasFiniteMaxLen = EdgeHasFiniteMaxLen(Graph, Edge);
                ForwardConstraints.Add(Constraint);
            }
        }

        for (const FConstraintEdge &Constraint : ForwardConstraints) {
            const int32 OldRank = RankBase[Constraint.Dst];
            const int32 Candidate = RankBase[Constraint.Src] + Constraint.Weight;
            if (Candidate > OldRank) {
                RankBase[Constraint.Dst] = Candidate;
                const FSugiyamaNode &SrcNode = Graph.Nodes[Constraint.Src];
                const FSugiyamaNode &DstNode = Graph.Nodes[Constraint.Dst];
                const FString SrcName = SrcNode.Name.IsEmpty() ? TEXT("<unnamed>") : SrcNode.Name;
                const FString DstName = DstNode.Name.IsEmpty() ? TEXT("<unnamed>") : DstNode.Name;
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                       TEXT("Sugiyama[%s] AssignLayers dst=%s name=%s rank %d->%d src=%s name=%s w=%d"), Label,
                       *BuildNodeKeyString(DstNode.Key), *DstName, OldRank, RankBase[Constraint.Dst],
                       *BuildNodeKeyString(SrcNode.Key), *SrcName, Constraint.Weight);
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
        // 何回かやる
        for (int32 Sweep = 0; Sweep < 10; ++Sweep) {
            if (bDumpDetail) {
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose, TEXT("Sugiyama[%s] AssignLayers: backward pass sweep %d"),
                       Label, Sweep);
            }
            bool bUpdated = false;
            for (int32 OrderIndex = TopoOrder.Num() - 1; OrderIndex >= 0; --OrderIndex) {
                const int32 NodeIndex = TopoOrder[OrderIndex];
                if (!SrcToDsts.Contains(NodeIndex)) {
                    continue;
                }
                FConstraintEdge &Constraint = SrcToConstraint.FindChecked(NodeIndex);
                TArray<int32> &DstList = SrcToDsts.FindChecked(NodeIndex);
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose, TEXT("Sugiyama[%s]    src=%s dsts=%d w=%d"), Label,
                       *BuildNodeKeyString(Graph.Nodes[Constraint.Src].Key), DstList.Num(), Constraint.Weight);
                int32 MinRank = MAX_int32;
                for (int32 Dst : DstList) {
                    MinRank = FMath::Min(MinRank, RankBase[Dst]);
                    if (bDumpDetail) {
                        UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                               TEXT("Sugiyama[%s]      src=%s consider dst=%s rank=%d"), Label,
                               *BuildNodeKeyString(Graph.Nodes[Constraint.Src].Key),
                               *BuildNodeKeyString(Graph.Nodes[Dst].Key), RankBase[Dst]);
                    }
                }
                MinRank = FMath::Max(MinRank - Constraint.Weight, RankBase[Constraint.Src]);
                if (MinRank == RankBase[Constraint.Src]) {
                    continue;
                }
                RankBase[Constraint.Src] = MinRank;
                bUpdated = true;
                if (bDumpDetail) {
                    UE_LOG(LogBlueprintAutoLayout, VeryVerbose, TEXT("Sugiyama[%s]      src=%s rank->%d"), Label,
                           *BuildNodeKeyString(Graph.Nodes[Constraint.Src].Key), RankBase[Constraint.Src]);
                }
            }
            if (bDumpDetail) {
                UE_LOG(LogBlueprintAutoLayout, VeryVerbose,
                       TEXT("Sugiyama[%s] AssignLayers: backward pass sweep %d complete bUpdated=%d"), Label, Sweep,
                       bUpdated ? 1 : 0);
            }
        }
    } else {
        for (int32 NodeIndex : TopoOrder) {
            for (int32 EdgeIndex : OutEdges[NodeIndex]) {
                const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                const int32 Dst = Edge.Dst;
                // Enforce "child is at least one rank below parent".
                RankBase[Dst] = FMath::Max(RankBase[Dst], RankBase[NodeIndex] + 1);
            }
        }
    }

    if (bDumpDetail) {
        // Helpful trace of the topo order and RankBase assignments.
        for (int32 OrderIndex = 0; OrderIndex < TopoOrder.Num(); ++OrderIndex) {
            const int32 NodeIndex = TopoOrder[OrderIndex];
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] TopoOrder[%d]: node=%s rankBase=%d"), Label,
                   OrderIndex, *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key), RankBase[NodeIndex]);
        }
    }

    int32 MaxRank = 0;
    for (int32 Index = 0; Index < NodeCount; ++Index) {
        Graph.Nodes[Index].Rank = RankBase[Index];
        MaxRank = FMath::Max(MaxRank, RankBase[Index]);
        if (bDumpDetail) {
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] Rank: node=%s rank=%d"), Label,
                   *BuildNodeKeyString(Graph.Nodes[Index].Key), RankBase[Index]);
        }
    }

    return MaxRank;
}

// Insert dummy nodes so every edge spans a single rank.
void SplitLongEdges(FSugiyamaGraph &Graph, const TCHAR *Label)
{
    const int32 OriginalNodeCount = Graph.Nodes.Num();
    const int32 OriginalEdgeCount = Graph.Edges.Num();
    int32 DummyAdded = 0;
    int32 SplitEdgeCount = 0;
    const bool bDumpDetail = ShouldDumpDetail(OriginalNodeCount, OriginalEdgeCount);

    TArray<FSugiyamaEdge> NewEdges;
    NewEdges.Reserve(Graph.Edges.Num());

    for (const FSugiyamaEdge &Edge : Graph.Edges) {
        const int32 SrcRank = Graph.Nodes[Edge.Src].Rank;
        const int32 DstRank = Graph.Nodes[Edge.Dst].Rank;
        const int32 RankDiff = DstRank - SrcRank;

        if (RankDiff <= 1) {
            NewEdges.Add(Edge);
            continue;
        }

        ++SplitEdgeCount;
        DummyAdded += RankDiff - 1;
        if (bDumpDetail) {
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] SplitLongEdges: edge %s -> %s rankDiff=%d"),
                   Label, *BuildNodeKeyString(Graph.Nodes[Edge.Src].Key),
                   *BuildNodeKeyString(Graph.Nodes[Edge.Dst].Key), RankDiff);
        }

        const bool bExecEdge = Edge.Kind == EEdgeKind::Exec;
        int32 Prev = Edge.Src;
        for (int32 Step = 1; Step < RankDiff; ++Step) {
            FSugiyamaNode Dummy;
            Dummy.Id = Graph.Nodes.Num();
            Dummy.Key = MakeSyntheticNodeKey(FString::Printf(TEXT("Dummy|%s|%d"), *Edge.StableKey, Step));
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

            FSugiyamaEdge Segment;
            Segment.Src = Prev;
            Segment.Dst = Dummy.Id;
            if (Prev == Edge.Src) {
                Segment.SrcPin = Edge.SrcPin;
                Segment.SrcPinIndex = Edge.SrcPinIndex;
            } else {
                Segment.SrcPin = MakeDummyPinKey(Graph.Nodes[Prev].Key, EPinDirection::Output);
                Segment.SrcPinIndex = 0;
            }
            Segment.DstPin = MakeDummyPinKey(Dummy.Key, EPinDirection::Input);
            Segment.DstPinIndex = 0;
            Segment.Kind = Edge.Kind;
            Segment.StableKey = FString::Printf(TEXT("%s|seg%d"), *Edge.StableKey, Step);
            NewEdges.Add(Segment);
            Prev = Dummy.Id;
        }

        FSugiyamaEdge FinalEdge;
        FinalEdge.Src = Prev;
        FinalEdge.Dst = Edge.Dst;
        FinalEdge.SrcPin = MakeDummyPinKey(Graph.Nodes[Prev].Key, EPinDirection::Output);
        FinalEdge.SrcPinIndex = 0;
        FinalEdge.DstPin = Edge.DstPin;
        FinalEdge.DstPinIndex = Edge.DstPinIndex;
        FinalEdge.Kind = Edge.Kind;
        FinalEdge.StableKey = FString::Printf(TEXT("%s|seg%d"), *Edge.StableKey, RankDiff);
        NewEdges.Add(FinalEdge);
    }

    Graph.Edges = MoveTemp(NewEdges);

    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("Sugiyama[%s] SplitLongEdges: nodes=%d (dummyAdded=%d) ") TEXT("edges=%d (splitEdges=%d)"), Label,
           Graph.Nodes.Num(), DummyAdded, Graph.Edges.Num(), SplitEdgeCount);
    if (bDumpDetail && DummyAdded > 0) {
        for (int32 Index = OriginalNodeCount; Index < Graph.Nodes.Num(); ++Index) {
            const FSugiyamaNode &Node = Graph.Nodes[Index];
            if (!Node.bIsDummy) {
                continue;
            }
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] DummyNode[%d]: key=%s rank=%d"), Label, Index,
                   *BuildNodeKeyString(Node.Key), Node.Rank);
        }
    }
}

// Initialize per-rank ordering deterministically before crossing reduction.
void AssignInitialOrder(FSugiyamaGraph &Graph, int32 MaxRank, TArray<TArray<int32>> &RankNodes, const TCHAR *Label)
{
    RankNodes.SetNum(MaxRank + 1);
    for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index) {
        const int32 Rank = Graph.Nodes[Index].Rank;
        if (Rank >= 0 && Rank < RankNodes.Num()) {
            RankNodes[Rank].Add(Index);
        }
    }

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
        for (int32 Order = 0; Order < Layer.Num(); ++Order) {
            Graph.Nodes[Layer[Order]].Order = Order;
        }
    }

    LogRankOrders(Label, TEXT("InitialOrder"), Graph, RankNodes);
}

// Convert a pin index into a fractional offset for barycenter computation.
double GetPinOffset(const FSugiyamaNode &Node, int32 PinIndex, int32 PinCount)
{
    const int32 Denom = FMath::Max(1, PinCount);
    return static_cast<double>(PinIndex) / static_cast<double>(Denom);
}

// Sweep forward and backward to reduce edge crossings using barycenters.
void RunCrossingReduction(FSugiyamaGraph &Graph, int32 MaxRank, int32 NumSweeps, TArray<TArray<int32>> &RankNodes,
                          const TCHAR *Label)
{
    const bool bDumpDetail = ShouldDumpSugiyamaDetail(Graph);
    const bool bCrossDetail =
        Graph.Nodes.Num() <= kVerboseCrossingDetailLimit && Graph.Edges.Num() <= kVerboseDumpEdgeLimit;
    if (MaxRank <= 0 || NumSweeps <= 0) {
        if (bDumpDetail) {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("Sugiyama[%s] CrossingReduction: skipped maxRank=%d ") TEXT("sweeps=%d"), Label, MaxRank,
                   NumSweeps);
        }
        return;
    }

    if (bDumpDetail) {
        UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] CrossingReduction: sweeps=%d maxRank=%d"), Label,
               NumSweeps, MaxRank);
    }

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

    for (int32 Sweep = 0; Sweep < NumSweeps; ++Sweep) {
        for (int32 Rank = 1; Rank <= MaxRank; ++Rank) {
            TArray<int32> &Layer = RankNodes[Rank];
            if (Layer.IsEmpty()) {
                continue;
            }

            struct FOrderItem
            {
                int32 NodeIndex = INDEX_NONE;
                double Barycenter = 0.0;
                int32 NeighborCount = 0;
            };

            TArray<FOrderItem> Items;
            Items.Reserve(Layer.Num());

            for (int32 NodeIndex : Layer) {
                if (bCrossDetail) {
                    UE_LOG(LogBlueprintAutoLayout, Verbose,
                           TEXT("Sugiyama[%s] Sweep%d Fwd rank=%d node=%s calculating barycenter from %d in-edges"),
                           Label, Sweep, Rank, *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key),
                           InEdges[NodeIndex].Num());
                }
                double Sum = 0.0;
                int32 Count = 0;
                TArray<int32> NeighborEdges;
                for (int32 EdgeIndex : InEdges[NodeIndex]) {
                    const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                    if (Graph.Nodes[Edge.Src].Rank != Rank - 1) {
                        continue;
                    }
                    NeighborEdges.Add(EdgeIndex);
                }

                NeighborEdges.Sort(
                    [&](int32 A, int32 B) { return PinKeyLess(Graph.Edges[A].SrcPin, Graph.Edges[B].SrcPin); });

                for (int32 EdgeIndex : NeighborEdges) {
                    const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                    const FSugiyamaNode &Neighbor = Graph.Nodes[Edge.Src];
                    if (Neighbor.ExecOutputPinCount != 0 && Edge.Kind != EEdgeKind::Exec) {
                        // Skip data pins for barycenter calculation
                        if (bCrossDetail) {
                            UE_LOG(LogBlueprintAutoLayout, Verbose,
                                   TEXT("Sugiyama[%s]   skip neighbor node=%s order=%d pinIndex=%d (data pin)"), Label,
                                   *BuildNodeKeyString(Neighbor.Key), Neighbor.Order, Edge.SrcPinIndex);
                        }
                        continue;
                    }

                    double PinOffset = GetPinOffset(Neighbor, Edge.SrcPinIndex, Neighbor.OutputPinCount);
                    if (bCrossDetail) {
                        UE_LOG(LogBlueprintAutoLayout, Verbose,
                               TEXT("Sugiyama[%s]   consider neighbor node=%s order=%d pinIndex=%d pinoffset=%.3f"),
                               Label, *BuildNodeKeyString(Neighbor.Key), Neighbor.Order, Edge.SrcPinIndex, PinOffset);
                    }
                    Sum += static_cast<double>(Neighbor.Order) + PinOffset;
                    ++Count;
                }

                FOrderItem Item;
                Item.NodeIndex = NodeIndex;
                if (Count == 0) {
                    Item.Barycenter = static_cast<double>(Graph.Nodes[NodeIndex].Order);
                } else {
                    Item.Barycenter = Sum / Count;
                }
                Item.NeighborCount = Count;
                Items.Add(Item);
            }

            if (bCrossDetail) {
                for (const FOrderItem &Item : Items) {
                    UE_LOG(LogBlueprintAutoLayout, Verbose,
                           TEXT("Sugiyama[%s] Sweep%d Fwd rank=%d node=%s ") TEXT("bary=%.3f neighbors=%d"), Label,
                           Sweep, Rank, *BuildNodeKeyString(Graph.Nodes[Item.NodeIndex].Key), Item.Barycenter,
                           Item.NeighborCount);
                }
            }

            Items.Sort([&](const FOrderItem &A, const FOrderItem &B) {
                if (A.Barycenter != B.Barycenter) {
                    return A.Barycenter < B.Barycenter;
                }
                return NodeKeyLess(Graph.Nodes[A.NodeIndex].Key, Graph.Nodes[B.NodeIndex].Key);
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
                    UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("Sugiyama[%s] Sweep%d Fwd rank=%d order=%d node=%s"),
                           Label, Sweep, Rank, Index, *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key));
                }
            }
        }

        // 早期終了したexecレーンのorderは決定不能なので、最後に1回だけforwardを回す。
        if (Sweep < NumSweeps - 1) {
            for (int32 Rank = MaxRank - 1; Rank >= 0; --Rank) {
                TArray<int32> &Layer = RankNodes[Rank];
                if (Layer.IsEmpty()) {
                    continue;
                }

                struct FOrderItem
                {
                    int32 NodeIndex = INDEX_NONE;
                    double Barycenter = 0.0;
                    int32 NeighborCount = 0;
                };

                TArray<FOrderItem> Items;
                Items.Reserve(Layer.Num());

                for (int32 NodeIndex : Layer) {
                    if (bCrossDetail) {
                        UE_LOG(
                            LogBlueprintAutoLayout, Verbose,
                            TEXT("Sugiyama[%s] Sweep%d Bwd rank=%d node=%s calculating barycenter from %d out-edges"),
                            Label, Sweep, Rank, *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key),
                            OutEdges[NodeIndex].Num());
                    }
                    double Sum = 0.0;
                    int32 Count = 0;
                    TArray<int32> NeighborEdges;
                    for (int32 EdgeIndex : OutEdges[NodeIndex]) {
                        const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                        if (Graph.Nodes[Edge.Dst].Rank != Rank + 1) {
                            continue;
                        }
                        NeighborEdges.Add(EdgeIndex);
                    }

                    NeighborEdges.Sort(
                        [&](int32 A, int32 B) { return PinKeyLess(Graph.Edges[A].DstPin, Graph.Edges[B].DstPin); });

                    for (int32 EdgeIndex : NeighborEdges) {
                        const FSugiyamaEdge &Edge = Graph.Edges[EdgeIndex];
                        const FSugiyamaNode &Neighbor = Graph.Nodes[Edge.Dst];

                        double PinOffset = GetPinOffset(Neighbor, Edge.DstPinIndex, Neighbor.InputPinCount);
                        if (bCrossDetail) {
                            UE_LOG(LogBlueprintAutoLayout, Verbose,
                                   TEXT("Sugiyama[%s]   consider neighbor node=%s order=%d pinIndex=%d pinoffset=%.3f"),
                                   Label, *BuildNodeKeyString(Neighbor.Key), Neighbor.Order, Edge.DstPinIndex,
                                   PinOffset);
                        }

                        Sum += static_cast<double>(Neighbor.Order) + PinOffset;
                        ++Count;
                    }

                    FOrderItem Item;
                    Item.NodeIndex = NodeIndex;
                    if (Count == 0) {
                        Item.Barycenter = static_cast<double>(Graph.Nodes[NodeIndex].Order);
                    } else {
                        Item.Barycenter = Sum / Count;
                    }
                    Item.NeighborCount = Count;
                    Items.Add(Item);
                }

                if (bCrossDetail) {
                    for (const FOrderItem &Item : Items) {
                        UE_LOG(LogBlueprintAutoLayout, Verbose,
                               TEXT("Sugiyama[%s] Sweep%d Bwd rank=%d node=%s ") TEXT("bary=%.3f neighbors=%d"), Label,
                               Sweep, Rank, *BuildNodeKeyString(Graph.Nodes[Item.NodeIndex].Key), Item.Barycenter,
                               Item.NeighborCount);
                    }
                }

                Items.Sort([&](const FOrderItem &A, const FOrderItem &B) {
                    if (A.Barycenter != B.Barycenter) {
                        return A.Barycenter < B.Barycenter;
                    }
                    return NodeKeyLess(Graph.Nodes[A.NodeIndex].Key, Graph.Nodes[B.NodeIndex].Key);
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
                               TEXT("Sugiyama[%s] Sweep%d Bwd rank=%d order=%d node=%s"), Label, Sweep, Rank, Index,
                               *BuildNodeKeyString(Graph.Nodes[NodeIndex].Key));
                    }
                }
            }
        }

        for (int32 Rank = 0; Rank < RankNodes.Num(); ++Rank) {
            SortRankByOrder(Rank);
        }
    }

    LogRankOrders(Label, TEXT("CrossingFinalOrder"), Graph, RankNodes);
}

// Full Sugiyama pipeline: break cycles, layer, split long edges, and order.
int32 RunSugiyama(FSugiyamaGraph &Graph, int32 NumSweeps, const TCHAR *Label)
{
    LogSugiyamaSummary(Label, TEXT("start"), Graph);
    LogSugiyamaNodes(Label, TEXT("start"), Graph);
    LogSugiyamaEdges(Label, TEXT("start"), Graph);

    RemoveCycles(Graph, Label);
    ApplyEdgeDirections(Graph);
    LogSugiyamaEdges(Label, TEXT("afterCycle"), Graph);
    int32 MaxRank = AssignLayers(Graph, Label);
    SplitLongEdges(Graph, Label);

    for (const FSugiyamaNode &Node : Graph.Nodes) {
        MaxRank = FMath::Max(MaxRank, Node.Rank);
    }

    TArray<TArray<int32>> RankNodes;
    AssignInitialOrder(Graph, MaxRank, RankNodes, Label);
    RunCrossingReduction(Graph, MaxRank, NumSweeps, RankNodes, Label);
    LogSugiyamaSummary(Label, TEXT("final"), Graph);
    LogSugiyamaNodes(Label, TEXT("final"), Graph);
    LogSugiyamaEdges(Label, TEXT("final"), Graph);
    return MaxRank;
}

struct FGlobalPlacement
{
    TMap<int32, FVector2f> Positions;
    int32 AnchorNodeIndex = INDEX_NONE;
};

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

bool BuildWorkNodes(const FLayoutGraph &Graph, const TArray<int32> &ComponentNodeIds, TArray<FWorkNode> &OutNodes,
                    TMap<int32, int32> &OutLocalIdToIndex, FString *OutError)
{
    OutNodes.Reset();
    OutLocalIdToIndex.Reset();

    TMap<int32, int32> GraphIdToIndex;
    GraphIdToIndex.Reserve(Graph.Nodes.Num());
    for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index) {
        GraphIdToIndex.Add(Graph.Nodes[Index].Id, Index);
    }

    TArray<int32> SortedIds = ComponentNodeIds;
    SortedIds.Sort();
    SortedIds.SetNum(Algo::Unique(SortedIds));

    OutNodes.Reserve(SortedIds.Num());
    OutLocalIdToIndex.Reserve(SortedIds.Num());

    for (int32 NodeId : SortedIds) {
        const int32 GraphIndex = GraphIdToIndex.FindRef(NodeId);
        if (GraphIndex == INDEX_NONE) {
            if (OutError) {
                *OutError = FString::Printf(TEXT("Missing node id in layout graph: %d."), NodeId);
            }
            return false;
        }

        const FLayoutNode &GraphNode = Graph.Nodes[GraphIndex];
        FWorkNode Node;
        Node.LocalIndex = OutNodes.Num();
        Node.GraphId = GraphNode.Id;
        Node.Key = GraphNode.Key;
        Node.Name = GraphNode.Name;
        Node.Size = FVector2f(FMath::Max(0.0f, GraphNode.Size.X), FMath::Max(0.0f, GraphNode.Size.Y));
        Node.OriginalPosition = GraphNode.Position;
        Node.bHasExecPins = GraphNode.bHasExecPins;
        Node.ExecInputPinCount = GraphNode.ExecInputPinCount;
        Node.ExecOutputPinCount = GraphNode.ExecOutputPinCount;
        Node.InputPinCount = GraphNode.InputPinCount;
        Node.OutputPinCount = GraphNode.OutputPinCount;
        OutLocalIdToIndex.Add(Node.GraphId, Node.LocalIndex);
        OutNodes.Add(Node);
    }

    if (OutNodes.Num() <= kVerboseDumpNodeLimit) {
        for (const FWorkNode &Node : OutNodes) {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("LayoutComponent: node graphId=%d key=%s size=(%.1f,%.1f) ")
                       TEXT("pos=(%.1f,%.1f) execPins=%d execIn=%d execOut=%d inputPins=%d outputPins=%d"),
                   Node.GraphId, *BuildNodeKeyString(Node.Key), Node.Size.X, Node.Size.Y, Node.OriginalPosition.X,
                   Node.OriginalPosition.Y, Node.bHasExecPins ? 1 : 0, Node.ExecInputPinCount, Node.ExecOutputPinCount,
                   Node.InputPinCount, Node.OutputPinCount);
        }
    }

    return true;
}

bool TryHandleSingleNode(const TArray<FWorkNode> &Nodes, FLayoutComponentResult &OutResult)
{
    if (Nodes.Num() != 1) {
        return false;
    }

    const FWorkNode &Solo = Nodes[0];
    UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("LayoutComponent: single node fast path graphId=%d"), Solo.GraphId);
    OutResult.NodePositions.Add(Solo.GraphId, Solo.OriginalPosition);
    const FVector2f Min = Solo.OriginalPosition;
    const FVector2f Max = Solo.OriginalPosition + Solo.Size;
    OutResult.Bounds += FBox2f(Min, Max);
    return true;
}

void BuildWorkEdges(const FLayoutGraph &Graph, const TArray<FWorkNode> &Nodes, const TMap<int32, int32> &LocalIdToIndex,
                    TArray<FWorkEdge> &OutEdges)
{
    OutEdges.Reset();
    OutEdges.Reserve(Graph.Edges.Num());

    for (const FLayoutEdge &Edge : Graph.Edges) {
        const int32 SrcIndex = LocalIdToIndex.FindRef(Edge.Src);
        const int32 DstIndex = LocalIdToIndex.FindRef(Edge.Dst);
        if (SrcIndex == INDEX_NONE || DstIndex == INDEX_NONE) {
            continue;
        }
        if (SrcIndex == DstIndex) {
            continue;
        }

        FWorkEdge LocalEdge;
        LocalEdge.Src = SrcIndex;
        LocalEdge.Dst = DstIndex;
        LocalEdge.Kind = Edge.Kind;
        LocalEdge.SrcPinIndex = FMath::Max(0, Edge.SrcPinIndex);
        LocalEdge.DstPinIndex = FMath::Max(0, Edge.DstPinIndex);
        LocalEdge.SrcPinName = Edge.SrcPinName;
        LocalEdge.DstPinName = Edge.DstPinName;
        LocalEdge.SrcPinKey =
            MakePinKey(Nodes[SrcIndex].Key, EPinDirection::Output, LocalEdge.SrcPinName, LocalEdge.SrcPinIndex);
        LocalEdge.DstPinKey =
            MakePinKey(Nodes[DstIndex].Key, EPinDirection::Input, LocalEdge.DstPinName, LocalEdge.DstPinIndex);
        LocalEdge.StableKey =
            BuildPinKeyString(LocalEdge.SrcPinKey) + TEXT("->") + BuildPinKeyString(LocalEdge.DstPinKey);
        OutEdges.Add(MoveTemp(LocalEdge));
    }

    OutEdges.Sort([](const FWorkEdge &A, const FWorkEdge &B) {
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

    if (ShouldDumpDetail(Nodes.Num(), OutEdges.Num())) {
        for (int32 EdgeIndex = 0; EdgeIndex < OutEdges.Num(); ++EdgeIndex) {
            const FWorkEdge &Edge = OutEdges[EdgeIndex];
            const TCHAR *Kind = Edge.Kind == EEdgeKind::Exec ? TEXT("exec") : TEXT("data");
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("LayoutComponent: edge[%d] %s srcId=%d dstId=%d ") TEXT("srcPin=%s dstPin=%s stable=%s"),
                   EdgeIndex, Kind, Nodes[Edge.Src].GraphId, Nodes[Edge.Dst].GraphId,
                   *BuildPinKeyString(Edge.SrcPinKey), *BuildPinKeyString(Edge.DstPinKey), *Edge.StableKey);
        }
    }

    int32 ExecEdgeCount = 0;
    int32 DataEdgeCount = 0;
    for (const FWorkEdge &Edge : OutEdges) {
        if (Edge.Kind == EEdgeKind::Exec) {
            ++ExecEdgeCount;
        } else if (Edge.Kind == EEdgeKind::Data) {
            ++DataEdgeCount;
        }
    }
    UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("LayoutComponent: working nodes=%d edges=%d (exec=%d data=%d)"),
           Nodes.Num(), OutEdges.Num(), ExecEdgeCount, DataEdgeCount);
}

void BuildSugiyamaGraph(const TArray<FWorkNode> &Nodes, const TArray<FWorkEdge> &Edges, FSugiyamaGraph &OutGraph)
{
    OutGraph.Nodes.Reset();
    OutGraph.Edges.Reset();
    OutGraph.Nodes.Reserve(Nodes.Num());
    OutGraph.Edges.Reserve(Edges.Num());

    for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
        const FWorkNode &WorkNode = Nodes[Index];
        FSugiyamaNode Node;
        Node.Id = Index;
        Node.Key = WorkNode.Key;
        Node.Name = WorkNode.Name;
        Node.ExecInputPinCount = FMath::Max(0, WorkNode.ExecInputPinCount);
        Node.ExecOutputPinCount = FMath::Max(0, WorkNode.ExecOutputPinCount);
        Node.InputPinCount = FMath::Max(0, WorkNode.InputPinCount);
        Node.OutputPinCount = FMath::Max(0, WorkNode.OutputPinCount);
        Node.bHasExecPins = WorkNode.bHasExecPins;
        Node.Size = WorkNode.Size;
        Node.SourceIndex = Index;
        OutGraph.Nodes.Add(Node);
    }

    for (const FWorkEdge &Edge : Edges) {
        FSugiyamaEdge GraphEdge;
        GraphEdge.Src = Edge.Src;
        GraphEdge.Dst = Edge.Dst;
        GraphEdge.SrcPin = Edge.SrcPinKey;
        GraphEdge.DstPin = Edge.DstPinKey;
        GraphEdge.SrcPinIndex = Edge.SrcPinIndex;
        GraphEdge.DstPinIndex = Edge.DstPinIndex;
        GraphEdge.Kind = Edge.Kind;
        GraphEdge.StableKey = Edge.StableKey;
        OutGraph.Edges.Add(MoveTemp(GraphEdge));
    }
}

void ApplySugiyamaRanks(const FSugiyamaGraph &Graph, TArray<FWorkNode> &Nodes)
{
    for (FWorkNode &Node : Nodes) {
        Node.GlobalRank = 0;
        Node.GlobalOrder = 0;
    }

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

void LogGlobalRankOrders(const TArray<FWorkNode> &Nodes)
{
    for (const FWorkNode &Node : Nodes) {
        const TCHAR *Name = Node.Name.IsEmpty() ? TEXT("<unnamed>") : *Node.Name;
        UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("LayoutComponent: global node key=%s name=%s rank=%d order=%d"),
               *BuildNodeKeyString(Node.Key), Name, Node.GlobalRank, Node.GlobalOrder);
    }
}

void ApplyFinalPositions(const TMap<int32, FVector2f> &PrimaryPositions,
                         const TMap<int32, FVector2f> &SecondaryPositions, const FVector2f &AnchorOffset,
                         const TArray<FWorkNode> &Nodes, FLayoutComponentResult &OutResult)
{
    TSet<int32> Positioned;
    for (const TPair<int32, FVector2f> &Pair : PrimaryPositions) {
        const int32 NodeIndex = Pair.Key;
        if (!Nodes.IsValidIndex(NodeIndex)) {
            continue;
        }
        Positioned.Add(NodeIndex);
        const FVector2f Pos = Pair.Value + AnchorOffset;
        OutResult.NodePositions.Add(Nodes[NodeIndex].GraphId, Pos);
        const FVector2f Min = Pos;
        const FVector2f Max = Pos + Nodes[NodeIndex].Size;
        OutResult.Bounds += FBox2f(Min, Max);
    }

    for (const TPair<int32, FVector2f> &Pair : SecondaryPositions) {
        const int32 NodeIndex = Pair.Key;
        if (!Nodes.IsValidIndex(NodeIndex) || Positioned.Contains(NodeIndex)) {
            continue;
        }
        const FVector2f Pos = Pair.Value + AnchorOffset;
        OutResult.NodePositions.Add(Nodes[NodeIndex].GraphId, Pos);
        const FVector2f Min = Pos;
        const FVector2f Max = Pos + Nodes[NodeIndex].Size;
        OutResult.Bounds += FBox2f(Min, Max);
    }

    if (Nodes.Num() <= kVerboseDumpNodeLimit) {
        for (const FWorkNode &Node : Nodes) {
            const FVector2f *Pos = OutResult.NodePositions.Find(Node.GraphId);
            if (!Pos) {
                continue;
            }
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("LayoutComponent: final node graphId=%d key=%s ") TEXT("pos=(%.1f,%.1f) size=(%.1f,%.1f)"),
                   Node.GraphId, *BuildNodeKeyString(Node.Key), Pos->X, Pos->Y, Node.Size.X, Node.Size.Y);
        }
    }

    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("LayoutComponent: positioned=%d boundsMin=(%.1f,%.1f) ") TEXT("boundsMax=(%.1f,%.1f)"),
           OutResult.NodePositions.Num(), OutResult.Bounds.Min.X, OutResult.Bounds.Min.Y, OutResult.Bounds.Max.X,
           OutResult.Bounds.Max.Y);
}

} // namespace
// Lay out a connected component using a single Sugiyama pass.
bool LayoutComponent(const FLayoutGraph &Graph, const TArray<int32> &ComponentNodeIds, const FLayoutSettings &Settings,
                     FLayoutComponentResult &OutResult, FString *OutError)
{

    OutResult = FLayoutComponentResult();
    UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("LayoutComponent: componentNodes=%d graphNodes=%d graphEdges=%d"),
           ComponentNodeIds.Num(), Graph.Nodes.Num(), Graph.Edges.Num());

    if (ComponentNodeIds.IsEmpty()) {
        if (OutError) {
            *OutError = TEXT("Layout component is empty.");
        }
        return false;
    }

    TArray<FWorkNode> Nodes;
    TMap<int32, int32> LocalIdToIndex;
    if (!BuildWorkNodes(Graph, ComponentNodeIds, Nodes, LocalIdToIndex, OutError)) {
        return false;
    }

    if (TryHandleSingleNode(Nodes, OutResult)) {
        return true;
    }

    TArray<FWorkEdge> Edges;
    BuildWorkEdges(Graph, Nodes, LocalIdToIndex, Edges);

    const float NodeSpacingX = FMath::Max(0.0f, Settings.NodeSpacingX);
    const float NodeSpacingY = FMath::Max(0.0f, Settings.NodeSpacingY);

    FSugiyamaGraph SugiyamaGraph;
    BuildSugiyamaGraph(Nodes, Edges, SugiyamaGraph);
    RunSugiyama(SugiyamaGraph, kSugiyamaSweeps, TEXT("Component"));
    ApplySugiyamaRanks(SugiyamaGraph, Nodes);
    LogGlobalRankOrders(Nodes);

    const FGlobalPlacement GlobalPlacement = PlaceGlobalRankOrder(Nodes, NodeSpacingX, NodeSpacingY);
    const FVector2f AnchorOffset = ComputeGlobalAnchorOffset(Nodes, GlobalPlacement);
    TMap<int32, FVector2f> EmptyPositions;
    ApplyFinalPositions(EmptyPositions, GlobalPlacement.Positions, AnchorOffset, Nodes, OutResult);
    return true;
}
} // namespace GraphLayout
