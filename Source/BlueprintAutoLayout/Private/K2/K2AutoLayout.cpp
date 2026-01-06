// Copyright Epic Games, Inc. All Rights Reserved.

#include "K2/K2AutoLayout.h"

#include "BlueprintAutoLayoutLog.h"
#include "BlueprintEditor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Graph/GraphLayout.h"
#include "Graph/GraphLayoutKeyUtils.h"
#include "GraphEditor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace K2AutoLayout
{
namespace
{
// Default sizes used when Slate geometry is not available.
constexpr float kDefaultNodeWidth = 300.0f;
constexpr float kDefaultNodeHeight = 100.0f;

// Deterministic ordering helpers so layout output is stable across runs.
bool NodeKeyLess(const GraphLayout::FNodeKey &A, const GraphLayout::FNodeKey &B)
{
    return GraphLayout::KeyUtils::NodeKeyLess(A, B);
}

// Key used to deterministically identify pins within a node.
struct FPinKey
{
    GraphLayout::FNodeKey NodeKey;
    EEdGraphPinDirection Direction = EGPD_Input;
    FName PinName;
    int32 PinIndex = 0;
};

bool PinKeyLess(const FPinKey &A, const FPinKey &B)
{
    return GraphLayout::KeyUtils::ComparePinKey(A.NodeKey, static_cast<int32>(A.Direction), A.PinName, A.PinIndex,
                                                B.NodeKey, static_cast<int32>(B.Direction), B.PinName, B.PinIndex) < 0;
}

// Human-readable and stable pin key string for edge identifiers.
FString BuildPinKeyString(const FPinKey &Key)
{
    const TCHAR *DirString = Key.Direction == EGPD_Input ? TEXT("I") : TEXT("O");
    return GraphLayout::KeyUtils::BuildPinKeyString(Key.NodeKey, DirString, Key.PinName, Key.PinIndex);
}

FBlueprintEditor *FindBlueprintEditor(UAssetEditorSubsystem *AssetEditorSubsystem, UBlueprint *Blueprint)
{
    // Find an existing Blueprint editor instance for the target asset.
    if (!AssetEditorSubsystem || !Blueprint) {
        return nullptr;
    }

    static const FName BlueprintEditorName(TEXT("BlueprintEditor"));
    static const FName LegacyBlueprintEditorName(TEXT("Kismet"));
    const TArray<IAssetEditorInstance *> Editors = AssetEditorSubsystem->FindEditorsForAsset(Blueprint);
    for (IAssetEditorInstance *EditorInstance : Editors) {
        if (!EditorInstance) {
            continue;
        }

        const FName EditorName = EditorInstance->GetEditorName();
        if (EditorName != BlueprintEditorName && EditorName != LegacyBlueprintEditorName) {
            continue;
        }

        return static_cast<FBlueprintEditor *>(EditorInstance);
    }

    return nullptr;
}

bool TryResolveGraphPanel(UBlueprint *Blueprint, UEdGraph *Graph, SGraphPanel *&OutPanel)
{
    // Force the graph to be open to capture geometry for widgets/pins.
    OutPanel = nullptr;

    if (!GEditor || !Blueprint || !Graph) {
        return false;
    }

    UAssetEditorSubsystem *AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSubsystem) {
        return false;
    }

    AssetEditorSubsystem->OpenEditorForAsset(Blueprint);

    FBlueprintEditor *BlueprintEditor = FindBlueprintEditor(AssetEditorSubsystem, Blueprint);
    if (!BlueprintEditor) {
        return false;
    }

    BlueprintEditor->OpenGraphAndBringToFront(Graph, true);

    const TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
    if (!GraphEditor.IsValid()) {
        return false;
    }

    SGraphPanel *Panel = GraphEditor->GetGraphPanel();
    if (!Panel) {
        return false;
    }

    OutPanel = Panel;
    return true;
}

} // namespace

bool AutoLayoutIslands(UBlueprint *Blueprint, UEdGraph *Graph, const TArray<UEdGraphNode *> &StartNodes,
                       const FAutoLayoutSettings &Settings, FAutoLayoutResult &OutResult)
{
    OutResult = FAutoLayoutResult();

    // Validate inputs up-front so we can return actionable feedback early.
    if (!Blueprint || !Graph) {
        OutResult.Error = TEXT("Missing Blueprint or graph.");
        OutResult.Guidance = TEXT("Provide a valid Blueprint and graph.");
        return false;
    }

    if (FBlueprintEditorUtils::IsGraphReadOnly(Graph)) {
        OutResult.Error = TEXT("Graph is read-only.");
        OutResult.Guidance = TEXT("Choose a writable graph and retry.");
        return false;
    }

    if (FBlueprintEditorUtils::IsGraphIntermediate(Graph)) {
        OutResult.Error = TEXT("Graph is intermediate.");
        OutResult.Guidance = TEXT("Choose a non-intermediate graph.");
        return false;
    }

    const UEdGraphSchema_K2 *Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
    if (!Schema) {
        OutResult.Error = TEXT("Graph does not use the K2 schema.");
        OutResult.Guidance = TEXT("Use a K2 Blueprint graph.");
        return false;
    }

    // Normalize and validate the selection; skip duplicates.
    TArray<UEdGraphNode *> FilteredStartNodes;
    FilteredStartNodes.Reserve(StartNodes.Num());
    for (UEdGraphNode *Node : StartNodes) {
        if (!Node) {
            continue;
        }
        if (Node->GetGraph() != Graph) {
            OutResult.Error = TEXT("Start nodes span multiple graphs.");
            OutResult.Guidance = TEXT("Provide nodes from a single graph.");
            return false;
        }
        FilteredStartNodes.AddUnique(Node);
    }

    if (FilteredStartNodes.IsEmpty()) {
        OutResult.Error = TEXT("No valid nodes selected for auto layout.");
        OutResult.Guidance = TEXT("Select nodes in the graph and retry.");
        return false;
    }

    // Gather all nodes in the graph to build stable layout data.
    TArray<UEdGraphNode *> AllNodes;
    AllNodes.Reserve(Graph->Nodes.Num());
    for (UEdGraphNode *Node : Graph->Nodes) {
        if (Node) {
            AllNodes.Add(Node);
        }
    }

    if (AllNodes.IsEmpty()) {
        OutResult.Error = TEXT("Graph has no nodes to layout.");
        OutResult.Guidance = TEXT("Add nodes to the graph and retry.");
        return false;
    }

    struct FNodeLayoutData
    {
        GraphLayout::FNodeKey Key;
        FVector2f Size = FVector2f::ZeroVector;
        bool bHasExecPins = false;
        int32 ExecInputPinCount = 0;
        int32 ExecOutputPinCount = 0;
        int32 InputPinCount = 0;
        int32 OutputPinCount = 0;
    };

    struct FPinLayoutData
    {
        FPinKey Key;
        bool bIsExec = false;
    };

    // If the graph panel is open, we can grab accurate geometry for pins/nodes.
    SGraphPanel *GraphPanel = nullptr;
    TryResolveGraphPanel(Blueprint, Graph, GraphPanel);

    TMap<UEdGraphNode *, TSharedPtr<SGraphNode>> NodeWidgets;
    if (GraphPanel) {
        // Only nodes with valid GUIDs can be resolved to widget instances.
        // This is best-effort; layout still works without live widgets.
        NodeWidgets.Reserve(AllNodes.Num());
        for (UEdGraphNode *Node : AllNodes) {
            if (!Node || !Node->NodeGuid.IsValid()) {
                continue;
            }
            const TSharedPtr<SGraphNode> NodeWidget = GraphPanel->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (NodeWidget.IsValid()) {
                NodeWidgets.Add(Node, NodeWidget);
            }
        }
    }

    TMap<UEdGraphNode *, FNodeLayoutData> NodeData;
    NodeData.Reserve(AllNodes.Num());
    TMap<UEdGraphPin *, FPinLayoutData> PinData;
    PinData.Reserve(AllNodes.Num() * 4);

    auto GatherPinsForDirection = [&PinData](UEdGraphNode *Node, EEdGraphPinDirection Direction, FNodeLayoutData &Data,
                                             int32 &PinCount, int32 &ExecPinCount, const TCHAR *Label) {
        int32 LocalPinIndex = 0;
        for (UEdGraphPin *Pin : Node->Pins) {
            if (!Pin || Pin->Direction != Direction) {
                continue;
            }

            FPinLayoutData PinInfo;
            PinInfo.Key.NodeKey = Data.Key;
            PinInfo.Key.Direction = Pin->Direction;
            PinInfo.Key.PinName = Pin->PinName;
            PinInfo.Key.PinIndex = LocalPinIndex;
            PinInfo.bIsExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
            PinData.Add(Pin, PinInfo);

            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("  %s Pin: %s PinIndex: %d"), Label, *Pin->PinName.ToString(),
                   LocalPinIndex);

            ++LocalPinIndex;
            ++PinCount;
            if (PinInfo.bIsExec) {
                ++ExecPinCount;
            }
        }
    };

    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("AutoLayoutIslands: Processing %d total nodes (selection=%d) in graph: %s"), AllNodes.Num(),
           FilteredStartNodes.Num(), *Graph->GetName());
    for (UEdGraphNode *Node : AllNodes) {
        if (!Node) {
            continue;
        }

        UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("  Processing node: %s"), *Node->GetName());

        // Build the node key and collect characteristics used by the layout.
        FNodeLayoutData Data;
        if (!Node->NodeGuid.IsValid()) {
            OutResult.Error = TEXT("NodeGuid is missing for a graph node.");
            OutResult.Guidance = TEXT("Regenerate node GUIDs and retry.");
            return false;
        }
        Data.Key.Guid = Node->NodeGuid;
        Data.ExecInputPinCount = 0;
        Data.ExecOutputPinCount = 0;

        const TSharedPtr<SGraphNode> NodeWidget = NodeWidgets.FindRef(Node);
        bool bHasGeometry = false;
        if (NodeWidget.IsValid()) {
            // Prefer geometry sizes from Slate when possible.
            const FGeometry &Geometry = NodeWidget->GetCachedGeometry();
            const FVector2f AbsoluteSize = Geometry.GetAbsoluteSize();
            const FVector2D DesiredSize = NodeWidget->GetDesiredSize();
            const bool bHasAbsoluteSize = AbsoluteSize.X > KINDA_SMALL_NUMBER && AbsoluteSize.Y > KINDA_SMALL_NUMBER;
            const bool bHasDesiredSize = DesiredSize.X > KINDA_SMALL_NUMBER && DesiredSize.Y > KINDA_SMALL_NUMBER;
            if (bHasAbsoluteSize || bHasDesiredSize) {
                const FVector2f DesiredSize2f(DesiredSize);
                const float SizeX =
                    FMath::Max(bHasAbsoluteSize ? AbsoluteSize.X : 0.0f, bHasDesiredSize ? DesiredSize2f.X : 0.0f);
                const float SizeY =
                    FMath::Max(bHasAbsoluteSize ? AbsoluteSize.Y : 0.0f, bHasDesiredSize ? DesiredSize2f.Y : 0.0f);
                if (SizeX > KINDA_SMALL_NUMBER && SizeY > KINDA_SMALL_NUMBER) {
                    Data.Size = FVector2f(SizeX, SizeY);
                    bHasGeometry = true;
                    UE_LOG(LogBlueprintAutoLayout, Verbose,
                           TEXT("  Captured max widget size: (%.1f, %.1f) abs=(%.1f, %.1f) desired=(%.1f, %.1f) "
                                "for node: %s"),
                           SizeX, SizeY, AbsoluteSize.X, AbsoluteSize.Y, DesiredSize.X, DesiredSize.Y,
                           *Node->GetName());
                }
            }
        } else {
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("  No widget found for node: %s; cannot capture geometry."),
                   *Node->GetName());
        }

        if (!bHasGeometry) {
            // Fallback to cached node dimensions or default settings.
            float Width = Node->GetWidth();
            float Height = Node->GetHeight();
            if (Width <= KINDA_SMALL_NUMBER) {
                Width = kDefaultNodeWidth;
            }
            if (Height <= KINDA_SMALL_NUMBER) {
                Height = kDefaultNodeHeight;
            }
            Data.Size = FVector2f(Width, Height);
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("  Using fallback size: (%.1f, %.1f) for node: %s"), Width,
                   Height, *Node->GetName());
        }

        // Capture pin metadata so edge ordering is deterministic.
        GatherPinsForDirection(Node, EGPD_Input, Data, Data.InputPinCount, Data.ExecInputPinCount, TEXT("Input"));
        GatherPinsForDirection(Node, EGPD_Output, Data, Data.OutputPinCount, Data.ExecOutputPinCount, TEXT("Output"));

        // Mark exec participation for layout heuristics downstream.
        Data.bHasExecPins = (Data.ExecInputPinCount + Data.ExecOutputPinCount) > 0;
        NodeData.Add(Node, Data);
    }

    GraphLayout::FLayoutGraph LayoutGraph;
    LayoutGraph.Nodes.Reserve(AllNodes.Num());

    TMap<UEdGraphNode *, int32> NodeToLayoutId;
    NodeToLayoutId.Reserve(AllNodes.Num());
    TArray<UEdGraphNode *> LayoutIdToNode;
    LayoutIdToNode.Reserve(AllNodes.Num());

    for (UEdGraphNode *Node : AllNodes) {
        if (!Node) {
            continue;
        }

        // Map editor nodes to layout nodes with stable identifiers and sizes.
        const FNodeLayoutData &Data = NodeData.FindChecked(Node);
        GraphLayout::FLayoutNode LayoutNode;
        LayoutNode.Id = LayoutGraph.Nodes.Num();
        LayoutNode.Key = Data.Key;
        FString NodeName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        if (NodeName.IsEmpty()) {
            NodeName = Node->GetName();
        }
        LayoutNode.Name = NodeName;
        LayoutNode.Size = Data.Size;
        LayoutNode.Position = FVector2f(Node->NodePosX, Node->NodePosY);
        LayoutNode.bHasExecPins = Data.bHasExecPins;
        LayoutNode.ExecInputPinCount = Data.ExecInputPinCount;
        LayoutNode.ExecOutputPinCount = Data.ExecOutputPinCount;
        LayoutNode.InputPinCount = Data.InputPinCount;
        LayoutNode.OutputPinCount = Data.OutputPinCount;
        LayoutGraph.Nodes.Add(LayoutNode);
        NodeToLayoutId.Add(Node, LayoutNode.Id);
        LayoutIdToNode.Add(Node);
    }

    if (LayoutGraph.Nodes.IsEmpty()) {
        OutResult.Error = TEXT("No movable nodes found for auto layout.");
        OutResult.Guidance = TEXT("Ensure the graph has layoutable nodes.");
        return false;
    }

    // Build graph edges based on pin links to drive layout connectivity.
    LayoutGraph.Edges.Reserve(AllNodes.Num() * 2);
    for (UEdGraphNode *Node : AllNodes) {
        if (!Node || !NodeToLayoutId.Contains(Node)) {
            continue;
        }

        const int32 SrcLayoutId = NodeToLayoutId.FindRef(Node);

        for (UEdGraphPin *Pin : Node->Pins) {
            if (!Pin || Pin->Direction != EGPD_Output) {
                continue;
            }

            // Only output pins can create edges; input pins are handled via links.
            const FPinLayoutData *SrcPinData = PinData.Find(Pin);
            if (!SrcPinData) {
                continue;
            }

            // Collect and sort linked input pins for deterministic edge ordering.
            TArray<UEdGraphPin *> LinkedPins;
            LinkedPins.Reserve(Pin->LinkedTo.Num());
            for (UEdGraphPin *Linked : Pin->LinkedTo) {
                if (!Linked || Linked->Direction != EGPD_Input) {
                    continue;
                }
                UEdGraphNode *TargetNode = Linked->GetOwningNode();
                if (!TargetNode || !NodeToLayoutId.Contains(TargetNode)) {
                    continue;
                }
                LinkedPins.Add(Linked);
            }

            LinkedPins.Sort([&PinData](const UEdGraphPin &A, const UEdGraphPin &B) {
                const FPinLayoutData *KeyA = PinData.Find(&A);
                const FPinLayoutData *KeyB = PinData.Find(&B);
                if (KeyA && KeyB) {
                    return PinKeyLess(KeyA->Key, KeyB->Key);
                }
                return &A < &B;
            });

            for (UEdGraphPin *Linked : LinkedPins) {
                UEdGraphNode *TargetNode = Linked->GetOwningNode();
                if (!TargetNode) {
                    continue;
                }

                const int32 DstLayoutId = NodeToLayoutId.FindRef(TargetNode);
                // Skip self edges; they do not contribute to layout adjacency.
                if (SrcLayoutId == DstLayoutId) {
                    continue;
                }

                const FPinLayoutData *DstPinData = PinData.Find(Linked);
                if (!DstPinData) {
                    continue;
                }

                // Build a layout edge with stable identifiers for reproducibility.
                GraphLayout::FLayoutEdge Edge;
                Edge.Src = SrcLayoutId;
                Edge.Dst = DstLayoutId;
                Edge.SrcPinName = SrcPinData->Key.PinName;
                Edge.DstPinName = DstPinData->Key.PinName;
                Edge.SrcPinIndex = SrcPinData->Key.PinIndex;
                Edge.DstPinIndex = DstPinData->Key.PinIndex;
                Edge.Kind = (SrcPinData->bIsExec && DstPinData->bIsExec) ? GraphLayout::EEdgeKind::Exec
                                                                         : GraphLayout::EEdgeKind::Data;
                Edge.StableKey = BuildPinKeyString(SrcPinData->Key) + TEXT("->") + BuildPinKeyString(DstPinData->Key);
                LayoutGraph.Edges.Add(MoveTemp(Edge));
            }
        }
    }

    // Start deterministic component ordering by stable node keys.
    TArray<int32> LayoutNodeOrder;
    LayoutNodeOrder.Reserve(LayoutGraph.Nodes.Num());
    for (int32 Index = 0; Index < LayoutGraph.Nodes.Num(); ++Index) {
        LayoutNodeOrder.Add(Index);
    }
    LayoutNodeOrder.Sort(
        [&LayoutGraph](int32 A, int32 B) { return NodeKeyLess(LayoutGraph.Nodes[A].Key, LayoutGraph.Nodes[B].Key); });

    // Build an undirected adjacency graph for component discovery.
    TArray<TSet<int32>> Adjacency;
    Adjacency.SetNum(LayoutGraph.Nodes.Num());
    for (const GraphLayout::FLayoutEdge &Edge : LayoutGraph.Edges) {
        if (Edge.Src == Edge.Dst) {
            continue;
        }
        Adjacency[Edge.Src].Add(Edge.Dst);
        Adjacency[Edge.Dst].Add(Edge.Src);
    }

    // Discover connected components in the layout graph.
    TSet<int32> VisitedLayoutNodes;
    TArray<TArray<int32>> Components;
    for (int32 NodeIndex : LayoutNodeOrder) {
        if (VisitedLayoutNodes.Contains(NodeIndex)) {
            continue;
        }

        TArray<int32> Stack;
        Stack.Add(NodeIndex);
        VisitedLayoutNodes.Add(NodeIndex);

        TArray<int32> Component;
        while (!Stack.IsEmpty()) {
            const int32 Current = Stack.Pop();
            Component.Add(Current);
            for (int32 Neighbor : Adjacency[Current]) {
                if (VisitedLayoutNodes.Contains(Neighbor)) {
                    continue;
                }
                VisitedLayoutNodes.Add(Neighbor);
                Stack.Add(Neighbor);
            }
        }

        Component.Sort([&LayoutGraph](int32 A, int32 B) {
            return NodeKeyLess(LayoutGraph.Nodes[A].Key, LayoutGraph.Nodes[B].Key);
        });
        Components.Add(MoveTemp(Component));
    }

    // Reduce layout scope to components touched by the user's selection.
    TSet<int32> SelectedLayoutNodes;
    for (UEdGraphNode *Node : FilteredStartNodes) {
        const int32 *LayoutId = NodeToLayoutId.Find(Node);
        if (LayoutId) {
            SelectedLayoutNodes.Add(*LayoutId);
        }
    }

    if (SelectedLayoutNodes.IsEmpty()) {
        OutResult.Error = TEXT("No selected nodes are eligible for layout.");
        OutResult.Guidance = TEXT("Select nodes in the graph and retry.");
        return false;
    }

    // Filter only components that include selected nodes to avoid moving
    // unrelated islands in the graph.
    TArray<TArray<int32>> SelectedComponents;
    for (const TArray<int32> &Component : Components) {
        bool bSelected = false;
        for (int32 NodeIndex : Component) {
            if (SelectedLayoutNodes.Contains(NodeIndex)) {
                bSelected = true;
                break;
            }
        }
        if (bSelected) {
            SelectedComponents.Add(Component);
        }
    }

    if (SelectedComponents.IsEmpty()) {
        OutResult.Error = TEXT("No connected components found for the selected nodes.");
        OutResult.Guidance = TEXT("Select nodes connected by pins and retry.");
        return false;
    }

    // Map UI settings into the layout engine configuration.
    GraphLayout::FLayoutSettings LayoutSettings;
    LayoutSettings.NodeSpacingX = Settings.NodeSpacingX;
    LayoutSettings.NodeSpacingYExec = Settings.NodeSpacingYExec;
    LayoutSettings.NodeSpacingYData = Settings.NodeSpacingYData;
    LayoutSettings.RankAlignment = Settings.RankAlignment;

    TMap<UEdGraphNode *, FVector2f> NewPositions;
    int32 ComponentsLaidOut = 0;

    for (const TArray<int32> &Component : SelectedComponents) {
        if (Component.IsEmpty()) {
            continue;
        }

        GraphLayout::FLayoutComponentResult LayoutResult;
        FString LayoutError;
        // Run the layout engine per component to keep results isolated.
        if (!GraphLayout::LayoutComponent(LayoutGraph, Component, LayoutSettings, LayoutResult, &LayoutError)) {
            OutResult.Error = LayoutError.IsEmpty() ? TEXT("Layout failed for component.") : LayoutError;
            OutResult.Guidance = TEXT("Verify the graph connectivity and retry.");
            return false;
        }
        ++ComponentsLaidOut;
        // Cache results so we can apply them in one editor transaction.
        for (const TPair<int32, FVector2f> &Pair : LayoutResult.NodePositions) {
            if (!LayoutIdToNode.IsValidIndex(Pair.Key)) {
                continue;
            }
            NewPositions.Add(LayoutIdToNode[Pair.Key], Pair.Value);
        }
    }

    // Apply positions with a transaction for undo/redo support.
    const FScopedTransaction Transaction(NSLOCTEXT("K2AutoLayout", "AutoLayoutNodes", "Auto Layout Blueprint Nodes"));
    Blueprint->Modify();
    Graph->Modify();

    int32 NodesLaidOut = 0;
    for (const TPair<UEdGraphNode *, FVector2f> &Pair : NewPositions) {
        UEdGraphNode *Node = Pair.Key;
        if (!Node) {
            continue;
        }

        Node->Modify();
        const FVector2f Pos = Pair.Value;
        // Round to integer pixels to avoid sub-pixel jitter in the editor.
        Schema->SetNodePosition(Node, FVector2D(FMath::RoundToInt(Pos.X), FMath::RoundToInt(Pos.Y)));
        ++NodesLaidOut;
    }

    // Notify and mark the asset dirty so the editor updates UI/state.
    Graph->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    OutResult.bSuccess = true;
    OutResult.NodesLaidOut = NodesLaidOut;
    OutResult.ComponentsLaidOut = ComponentsLaidOut;
    return true;
}
} // namespace K2AutoLayout
