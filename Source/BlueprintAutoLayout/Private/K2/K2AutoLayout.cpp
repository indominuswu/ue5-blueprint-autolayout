// Copyright Epic Games, Inc. All Rights Reserved.

// Auto layout public API.
#include "K2/K2AutoLayout.h"

// Editor graph dependencies for layout and selection.
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
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/ObjectKey.h"

// Blueprint graph auto-layout implementation.
namespace K2AutoLayout
{
namespace
{
// Default sizes used when Slate geometry is not available.
constexpr float kDefaultNodeWidth = 300.0f;
constexpr float kDefaultNodeHeight = 100.0f;
constexpr float kEstimatedPinHeight = 24.0f;
constexpr float kEstimatedNodeHeaderHeight = 48.0f;

// Cache key for per-graph node size lookup.
struct FNodeSizeCacheKey
{
    FObjectKey GraphKey;
    FGuid NodeGuid;

    // Allow default construction for map storage.
    FNodeSizeCacheKey() = default;
    FNodeSizeCacheKey(const UEdGraph *Graph, const FGuid &Guid)
        : GraphKey(Graph), NodeGuid(Guid)
    {
    }

    // Compare cache keys by graph and GUID.
    bool operator==(const FNodeSizeCacheKey &Other) const
    {
        return GraphKey == Other.GraphKey && NodeGuid == Other.NodeGuid;
    }
};

// Hash helper for FNodeSizeCacheKey so it can be used in TMap.
uint32 GetTypeHash(const FNodeSizeCacheKey &Key)
{
    return HashCombine(GetTypeHash(Key.GraphKey), GetTypeHash(Key.NodeGuid));
}

// Cache last-known node sizes so off-screen nodes can reuse valid measurements.
TMap<FNodeSizeCacheKey, FVector2f> GNodeSizeCache;

// Read cached node size data when available and valid.
bool TryGetCachedNodeSize(const UEdGraph *Graph, const FGuid &Guid, FVector2f &OutSize)
{
    if (!Graph || !Guid.IsValid()) {
        return false;
    }

    // Lookup the cached entry for this node.
    const FVector2f *Found = GNodeSizeCache.Find(FNodeSizeCacheKey(Graph, Guid));
    if (!Found || Found->X <= KINDA_SMALL_NUMBER || Found->Y <= KINDA_SMALL_NUMBER) {
        return false;
    }

    // Return the cached size.
    OutSize = *Found;
    return true;
}

// Update the cached node size using the latest valid measurement.
void UpdateNodeSizeCache(const UEdGraph *Graph, const FGuid &Guid,
                         const FVector2f &Size)
{
    // Skip invalid graph or GUID inputs.
    if (!Graph || !Guid.IsValid()) {
        return;
    }

    // Ignore non-positive sizes so we only cache usable geometry.
    if (Size.X <= KINDA_SMALL_NUMBER || Size.Y <= KINDA_SMALL_NUMBER) {
        return;
    }

    // Promote the cached size only when the new measurement increases dimensions.
    const FNodeSizeCacheKey CacheKey(Graph, Guid);
    const FVector2f *Found = GNodeSizeCache.Find(CacheKey);
    if (Found) {
        const FVector2f Updated(FMath::Max(Found->X, Size.X),
                                FMath::Max(Found->Y, Size.Y));
        if (Updated.X <= Found->X && Updated.Y <= Found->Y) {
            return;
        }
        GNodeSizeCache.Add(CacheKey, Updated);
        return;
    }

    // Store the first valid size for this node.
    GNodeSizeCache.Add(CacheKey, Size);
}

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

// Compare pin keys deterministically for sorting.
bool PinKeyLess(const FPinKey &A, const FPinKey &B)
{
    return GraphLayout::KeyUtils::ComparePinKey(
               A.NodeKey, static_cast<int32>(A.Direction), A.PinName, A.PinIndex,
               B.NodeKey, static_cast<int32>(B.Direction), B.PinName, B.PinIndex) < 0;
}

// Human-readable and stable pin key string for edge identifiers.
FString BuildPinKeyString(const FPinKey &Key)
{
    const TCHAR *DirString = Key.Direction == EGPD_Input ? TEXT("I") : TEXT("O");
    return GraphLayout::KeyUtils::BuildPinKeyString(Key.NodeKey, DirString, Key.PinName,
                                                    Key.PinIndex);
}

// Estimate node height based on pin counts when geometry is unavailable.
float EstimateNodeHeightFromPins(int32 InputPinCount, int32 OutputPinCount)
{
    const int32 MaxPins = FMath::Max(InputPinCount, OutputPinCount);
    if (MaxPins <= 0) {
        return kDefaultNodeHeight;
    }

    // Derive a height from header plus per-pin spacing.
    const float EstimatedHeight = kEstimatedNodeHeaderHeight +
                                  (kEstimatedPinHeight * static_cast<float>(MaxPins));
    return FMath::Max(kDefaultNodeHeight, EstimatedHeight);
}

// Find an open Blueprint editor instance for the target asset.
FBlueprintEditor *FindBlueprintEditor(UAssetEditorSubsystem *AssetEditorSubsystem,
                                      UBlueprint *Blueprint)
{
    // Find an existing Blueprint editor instance for the target asset.
    if (!AssetEditorSubsystem || !Blueprint) {
        return nullptr;
    }

    // Cache known editor names for comparison.
    static const FName BlueprintEditorName(TEXT("BlueprintEditor"));
    static const FName LegacyBlueprintEditorName(TEXT("Kismet"));
    const TArray<IAssetEditorInstance *> Editors =
        AssetEditorSubsystem->FindEditorsForAsset(Blueprint);
    for (IAssetEditorInstance *EditorInstance : Editors) {
        if (!EditorInstance) {
            continue;
        }

        // Match Blueprint editor instances by name.
        const FName EditorName = EditorInstance->GetEditorName();
        if (EditorName != BlueprintEditorName &&
            EditorName != LegacyBlueprintEditorName) {
            continue;
        }

        // Return the matching editor instance.
        return static_cast<FBlueprintEditor *>(EditorInstance);
    }

    // No matching editor was found.
    return nullptr;
}

// Resolve the graph panel widget for a Blueprint graph.
bool TryResolveGraphPanel(UBlueprint *Blueprint, UEdGraph *Graph,
                          SGraphPanel *&OutPanel)
{
    // Force the graph to be open to capture geometry for widgets/pins.
    OutPanel = nullptr;

    // Validate required editor context and inputs.
    if (!GEditor || !Blueprint || !Graph) {
        return false;
    }

    // Access the asset editor subsystem to open the Blueprint editor.
    UAssetEditorSubsystem *AssetEditorSubsystem =
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSubsystem) {
        return false;
    }

    // Ensure the Blueprint editor is open for the asset.
    AssetEditorSubsystem->OpenEditorForAsset(Blueprint);

    // Find the Blueprint editor instance for this asset.
    FBlueprintEditor *BlueprintEditor =
        FindBlueprintEditor(AssetEditorSubsystem, Blueprint);
    if (!BlueprintEditor) {
        return false;
    }

    // Resolve a graph editor and its panel for the target graph.
    const TSharedPtr<SGraphEditor> GraphEditorFromOpen =
        BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
    const TSharedPtr<SGraphEditor> GraphEditor =
        GraphEditorFromOpen.IsValid() ? GraphEditorFromOpen
                                      : SGraphEditor::FindGraphEditorForGraph(Graph);
    if (!GraphEditor.IsValid()) {
        return false;
    }

    // Fetch the graph panel widget.
    SGraphPanel *Panel = GraphEditor->GetGraphPanel();
    if (!Panel) {
        return false;
    }

    // Ensure node widgets are created and have desired sizes before capturing geometry.
    // Panel->Update();
    // Panel->SlatePrepass();

    // Return the resolved panel to the caller.
    OutPanel = Panel;
    return true;
}

// End of anonymous namespace helpers.
} // namespace

// Auto-layout connected components that intersect the selection.
bool AutoLayoutIslands(UBlueprint *Blueprint, UEdGraph *Graph,
                       const TArray<UEdGraphNode *> &StartNodes,
                       const FAutoLayoutSettings &Settings,
                       FAutoLayoutResult &OutResult)
{
    OutResult = FAutoLayoutResult();

    // Validate inputs up-front so we can return actionable feedback early.
    if (!Blueprint || !Graph) {
        OutResult.Error = TEXT("Missing Blueprint or graph.");
        OutResult.Guidance = TEXT("Provide a valid Blueprint and graph.");
        return false;
    }

    // Reject graphs that cannot be modified.
    if (FBlueprintEditorUtils::IsGraphReadOnly(Graph)) {
        OutResult.Error = TEXT("Graph is read-only.");
        OutResult.Guidance = TEXT("Choose a writable graph and retry.");
        return false;
    }

    // Reject intermediate graphs that should not be edited.
    if (FBlueprintEditorUtils::IsGraphIntermediate(Graph)) {
        OutResult.Error = TEXT("Graph is intermediate.");
        OutResult.Guidance = TEXT("Choose a non-intermediate graph.");
        return false;
    }

    // Ensure the graph uses the expected K2 schema.
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

    // Bail out if the selection yields no valid nodes.
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

    // Bail out if the graph contains no nodes.
    if (AllNodes.IsEmpty()) {
        OutResult.Error = TEXT("Graph has no nodes to layout.");
        OutResult.Guidance = TEXT("Add nodes to the graph and retry.");
        return false;
    }

    // Per-node layout data collected from editor nodes.
    struct FNodeLayoutData
    {
        GraphLayout::FNodeKey Key;
        FVector2f Size = FVector2f::ZeroVector;
        bool bHasExecPins = false;
        bool bIsVariableGet = false;
        bool bIsReroute = false;
        int32 ExecInputPinCount = 0;
        int32 ExecOutputPinCount = 0;
        int32 InputPinCount = 0;
        int32 OutputPinCount = 0;
    };

    // Per-pin layout data used for stable edge identification.
    struct FPinLayoutData
    {
        FPinKey Key;
        bool bIsExec = false;
    };

    // If the graph panel is open, we can grab accurate geometry for pins/nodes.
    SGraphPanel *GraphPanel = nullptr;
    TryResolveGraphPanel(Blueprint, Graph, GraphPanel);

    // Cache any live node widgets for geometry lookup.
    TMap<UEdGraphNode *, TSharedPtr<SGraphNode>> NodeWidgets;
    if (GraphPanel) {
        // Only nodes with valid GUIDs can be resolved to widget instances.
        // This is best-effort; layout still works without live widgets.
        NodeWidgets.Reserve(AllNodes.Num());
        for (UEdGraphNode *Node : AllNodes) {
            if (!Node || !Node->NodeGuid.IsValid()) {
                continue;
            }
            const TSharedPtr<SGraphNode> NodeWidget =
                GraphPanel->GetNodeWidgetFromGuid(Node->NodeGuid);
            if (NodeWidget.IsValid()) {
                NodeWidgets.Add(Node, NodeWidget);
            }
        }
    }

    // Store collected node and pin metadata for layout input.
    TMap<UEdGraphNode *, FNodeLayoutData> NodeData;
    NodeData.Reserve(AllNodes.Num());
    TMap<UEdGraphPin *, FPinLayoutData> PinData;
    PinData.Reserve(AllNodes.Num() * 4);

    // Helper to gather pin metadata and counts for a direction.
    auto GatherPinsForDirection = [&PinData](UEdGraphNode *Node,
                                             EEdGraphPinDirection Direction,
                                             FNodeLayoutData &Data, int32 &PinCount,
                                             int32 &ExecPinCount, const TCHAR *Label) {
        int32 LocalPinIndex = 0;
        for (UEdGraphPin *Pin : Node->Pins) {
            if (!Pin || Pin->Direction != Direction) {
                continue;
            }

            // Capture deterministic pin metadata for later edge ordering.
            FPinLayoutData PinInfo;
            PinInfo.Key.NodeKey = Data.Key;
            PinInfo.Key.Direction = Pin->Direction;
            PinInfo.Key.PinName = Pin->PinName;
            PinInfo.Key.PinIndex = LocalPinIndex;
            PinInfo.bIsExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
            PinData.Add(Pin, PinInfo);

            // Emit verbose pin diagnostics for troubleshooting.
            UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("  %s Pin: %s PinIndex: %d"),
                   Label, *Pin->PinName.ToString(), LocalPinIndex);

            // Update pin counters for sizing and exec classification.
            ++LocalPinIndex;
            ++PinCount;
            if (PinInfo.bIsExec) {
                ++ExecPinCount;
            }
        }
    };

    // Log a high-level summary for the layout run.
    UE_LOG(
        LogBlueprintAutoLayout, Verbose,
        TEXT(
            "AutoLayoutIslands: Processing %d total nodes (selection=%d) in graph: %s"),
        AllNodes.Num(), FilteredStartNodes.Num(), *Graph->GetName());
    for (UEdGraphNode *Node : AllNodes) {
        if (!Node) {
            continue;
        }

        // Log per-node processing for verbose diagnostics.
        UE_LOG(LogBlueprintAutoLayout, Verbose, TEXT("  Processing node: %s"),
               *Node->GetName());

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
        Data.bIsVariableGet = Node->IsA<UK2Node_VariableGet>();
        Data.bIsReroute = Node->IsA<UK2Node_Knot>();

        // Resolve a node widget so we can capture live geometry.
        const TSharedPtr<SGraphNode> NodeWidget = NodeWidgets.FindRef(Node);
        bool bHasGeometry = false;
        FVector2f CapturedSize = FVector2f::ZeroVector;
        if (NodeWidget.IsValid()) {
            // Prefer geometry sizes from Slate when possible.
            const FGeometry &Geometry = NodeWidget->GetCachedGeometry();
            const FVector2f AbsoluteSize = Geometry.GetAbsoluteSize();
            const FVector2D DesiredSize = NodeWidget->GetDesiredSize();
            const bool bHasAbsoluteSize = AbsoluteSize.X > KINDA_SMALL_NUMBER &&
                                          AbsoluteSize.Y > KINDA_SMALL_NUMBER;
            const bool bHasDesiredSize = DesiredSize.X > KINDA_SMALL_NUMBER &&
                                         DesiredSize.Y > KINDA_SMALL_NUMBER;
            if (bHasAbsoluteSize || bHasDesiredSize) {
                const FVector2f DesiredSize2f(DesiredSize);
                const float SizeX =
                    FMath::Max(bHasAbsoluteSize ? AbsoluteSize.X : 0.0f,
                               bHasDesiredSize ? DesiredSize2f.X : 0.0f);
                const float SizeY =
                    FMath::Max(bHasAbsoluteSize ? AbsoluteSize.Y : 0.0f,
                               bHasDesiredSize ? DesiredSize2f.Y : 0.0f);
                if (SizeX > KINDA_SMALL_NUMBER && SizeY > KINDA_SMALL_NUMBER) {
                    CapturedSize = FVector2f(SizeX, SizeY);
                    bHasGeometry = true;
                    UpdateNodeSizeCache(Graph, Node->NodeGuid, CapturedSize);
                    UE_LOG(LogBlueprintAutoLayout, Verbose,
                           TEXT("  Captured max widget size: (%.1f, %.1f) abs=(%.1f, "
                                "%.1f) desired=(%.1f, %.1f) "
                                "for node: %s"),
                           SizeX, SizeY, AbsoluteSize.X, AbsoluteSize.Y, DesiredSize.X,
                           DesiredSize.Y, *Node->GetName());
                }
            }
        } else {
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("  No widget found for node: %s; cannot capture geometry."),
                   *Node->GetName());
        }

        // Capture pin metadata so edge ordering is deterministic and fallback sizing
        // can use pin counts.
        GatherPinsForDirection(Node, EGPD_Input, Data, Data.InputPinCount,
                               Data.ExecInputPinCount, TEXT("Input"));
        GatherPinsForDirection(Node, EGPD_Output, Data, Data.OutputPinCount,
                               Data.ExecOutputPinCount, TEXT("Output"));

        // Resolve the final size using cached data, captured geometry, or fallback.
        FVector2f CachedSize = FVector2f::ZeroVector;
        if (TryGetCachedNodeSize(Graph, Node->NodeGuid, CachedSize)) {
            Data.Size = CachedSize;
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("  Using cached size: (%.1f, %.1f) for node: %s"), CachedSize.X,
                   CachedSize.Y, *Node->GetName());
        } else if (bHasGeometry) {
            Data.Size = CapturedSize;
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("  Using captured size: (%.1f, %.1f) for node: %s"),
                   CapturedSize.X, CapturedSize.Y, *Node->GetName());
        } else {
            // Fallback to node dimensions or default settings.
            float Width = Node->GetWidth();
            float Height = Node->GetHeight();
            if (Width <= KINDA_SMALL_NUMBER) {
                Width = kDefaultNodeWidth;
            }
            if (Height <= KINDA_SMALL_NUMBER) {
                Height =
                    EstimateNodeHeightFromPins(Data.InputPinCount, Data.OutputPinCount);
            }
            Data.Size = FVector2f(Width, Height);
            UE_LOG(LogBlueprintAutoLayout, Verbose,
                   TEXT("  Using fallback size: (%.1f, %.1f) for node: %s"), Width,
                   Height, *Node->GetName());
        }

        // Mark exec participation for layout heuristics downstream.
        Data.bHasExecPins = (Data.ExecInputPinCount + Data.ExecOutputPinCount) > 0;
        NodeData.Add(Node, Data);
    }

    // Initialize the layout graph that feeds the layout engine.
    GraphLayout::FLayoutGraph LayoutGraph;
    LayoutGraph.Nodes.Reserve(AllNodes.Num());

    // Build mappings between editor nodes and layout node ids.
    TMap<UEdGraphNode *, int32> NodeToLayoutId;
    NodeToLayoutId.Reserve(AllNodes.Num());
    TArray<UEdGraphNode *> LayoutIdToNode;
    LayoutIdToNode.Reserve(AllNodes.Num());

    // Populate layout nodes with stable identifiers and sizes.
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
        LayoutNode.bIsVariableGet = Data.bIsVariableGet;
        LayoutNode.bIsReroute = Data.bIsReroute;
        LayoutNode.ExecInputPinCount = Data.ExecInputPinCount;
        LayoutNode.ExecOutputPinCount = Data.ExecOutputPinCount;
        LayoutNode.InputPinCount = Data.InputPinCount;
        LayoutNode.OutputPinCount = Data.OutputPinCount;
        LayoutGraph.Nodes.Add(LayoutNode);
        NodeToLayoutId.Add(Node, LayoutNode.Id);
        LayoutIdToNode.Add(Node);
    }

    // Fail if no layout nodes were produced.
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

        // Resolve the layout id for this source node.
        const int32 SrcLayoutId = NodeToLayoutId.FindRef(Node);

        // Traverse output pins to emit edges.
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

            // Sort linked pins by deterministic pin keys.
            LinkedPins.Sort([&PinData](const UEdGraphPin &A, const UEdGraphPin &B) {
                const FPinLayoutData *KeyA = PinData.Find(&A);
                const FPinLayoutData *KeyB = PinData.Find(&B);
                if (KeyA && KeyB) {
                    return PinKeyLess(KeyA->Key, KeyB->Key);
                }
                return &A < &B;
            });

            // Emit layout edges for each linked input pin.
            for (UEdGraphPin *Linked : LinkedPins) {
                UEdGraphNode *TargetNode = Linked->GetOwningNode();
                if (!TargetNode) {
                    continue;
                }

                // Resolve the destination layout id for this link.
                const int32 DstLayoutId = NodeToLayoutId.FindRef(TargetNode);
                // Skip self edges; they do not contribute to layout adjacency.
                if (SrcLayoutId == DstLayoutId) {
                    continue;
                }

                // Retrieve destination pin metadata for edge construction.
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
                Edge.Kind = (SrcPinData->bIsExec && DstPinData->bIsExec)
                                ? GraphLayout::EEdgeKind::Exec
                                : GraphLayout::EEdgeKind::Data;
                Edge.StableKey = BuildPinKeyString(SrcPinData->Key) + TEXT("->") +
                                 BuildPinKeyString(DstPinData->Key);
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
    LayoutNodeOrder.Sort([&LayoutGraph](int32 A, int32 B) {
        return NodeKeyLess(LayoutGraph.Nodes[A].Key, LayoutGraph.Nodes[B].Key);
    });

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

        // Seed the DFS stack for this component.
        TArray<int32> Stack;
        Stack.Add(NodeIndex);
        VisitedLayoutNodes.Add(NodeIndex);

        // Accumulate node indices for this connected component.
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

        // Sort component nodes by stable key for determinism.
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

    // Require at least one selected layout node to proceed.
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

    // Require at least one connected component to process.
    if (SelectedComponents.IsEmpty()) {
        OutResult.Error = TEXT("No connected components found for the selected nodes.");
        OutResult.Guidance = TEXT("Select nodes connected by pins and retry.");
        return false;
    }

    // Map UI settings into the layout engine configuration.
    GraphLayout::FLayoutSettings LayoutSettings;
    LayoutSettings.NodeSpacingX = Settings.NodeSpacingX;
    LayoutSettings.NodeSpacingXExec = Settings.NodeSpacingXExec;
    LayoutSettings.NodeSpacingXData = Settings.NodeSpacingXData;
    LayoutSettings.NodeSpacingYExec = Settings.NodeSpacingYExec;
    LayoutSettings.NodeSpacingYData = Settings.NodeSpacingYData;
    LayoutSettings.VariableGetMinLength = Settings.VariableGetMinLength;
    LayoutSettings.RankAlignment = Settings.RankAlignment;
    LayoutSettings.bAlignExecChainsHorizontally = Settings.bAlignExecChainsHorizontally;

    // Accumulate new positions across all selected components.
    TMap<UEdGraphNode *, FVector2f> NewPositions;
    int32 ComponentsLaidOut = 0;

    // Run layout for each selected component.
    for (const TArray<int32> &Component : SelectedComponents) {
        if (Component.IsEmpty()) {
            continue;
        }

        // Prepare per-component layout outputs and errors.
        GraphLayout::FLayoutComponentResult LayoutResult;
        FString LayoutError;
        // Run the layout engine per component to keep results isolated.
        if (!GraphLayout::LayoutComponent(LayoutGraph, Component, LayoutSettings,
                                          LayoutResult, &LayoutError)) {
            OutResult.Error = LayoutError.IsEmpty()
                                  ? TEXT("Layout failed for component.")
                                  : LayoutError;
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
    const FScopedTransaction Transaction(
        NSLOCTEXT("K2AutoLayout", "AutoLayoutNodes", "Auto Layout Blueprint Nodes"));
    Blueprint->Modify();
    Graph->Modify();

    // Apply new positions back onto editor nodes.
    int32 NodesLaidOut = 0;
    for (const TPair<UEdGraphNode *, FVector2f> &Pair : NewPositions) {
        UEdGraphNode *Node = Pair.Key;
        if (!Node) {
            continue;
        }

        // Apply the new position under the current transaction.
        Node->Modify();
        const FVector2f Pos = Pair.Value;
        // Round to integer pixels to avoid sub-pixel jitter in the editor.
        Schema->SetNodePosition(
            Node, FVector2D(FMath::RoundToInt(Pos.X), FMath::RoundToInt(Pos.Y)));
        ++NodesLaidOut;
    }

    // Notify and mark the asset dirty so the editor updates UI/state.
    Graph->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    // Populate the success result payload.
    OutResult.bSuccess = true;
    OutResult.NodesLaidOut = NodesLaidOut;
    OutResult.ComponentsLaidOut = ComponentsLaidOut;
    return true;
}
} // namespace K2AutoLayout
