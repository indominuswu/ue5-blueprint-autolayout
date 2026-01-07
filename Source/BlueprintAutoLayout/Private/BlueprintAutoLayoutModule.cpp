// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "BlueprintAutoLayoutLog.h"
#include "BlueprintAutoLayoutSettings.h"
#include "BlueprintEditor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GraphEditor.h"
#include "K2/K2AutoLayout.h"
#include "K2/K2AutoLayoutComplexity.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#if !UE_BUILD_SHIPPING && !UE_BUILD_TEST
#include "SGraphNode.h"
#include "SGraphPanel.h"
#endif
#include "ToolMenus.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Notifications/SNotificationList.h"

DEFINE_LOG_CATEGORY(LogBlueprintAutoLayout);

#define LOCTEXT_NAMESPACE "BlueprintAutoLayout"

namespace
{
TArray<const UEdGraphNode *> GatherSelectedNodes(
    const UGraphNodeContextMenuContext &Context)
{
    // Build a stable list of selected nodes relevant to the context menu invocation.
    TArray<const UEdGraphNode *> Nodes;
    // Resolve the graph either from the context graph or the node under the cursor.
    const UEdGraph *ContextGraph =
        Context.Graph ? Context.Graph
                      : (Context.Node ? Context.Node->GetGraph() : nullptr);

    // If a graph is known, try to gather the editor selection scoped to that graph.
    if (ContextGraph) {
        // Use the provided blueprint when available; otherwise, infer it from the
        // graph.
        const UBlueprint *Blueprint =
            Context.Blueprint
                ? Context.Blueprint
                : FBlueprintEditorUtils::FindBlueprintForGraph(ContextGraph);

        if (Blueprint) {
            // Query the open Blueprint editor for the current selection.
            if (const TSharedPtr<IBlueprintEditor> BlueprintEditor =
                    FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint,
                                                                         false)) {
                // Cast to the concrete editor so we can read the graph panel selection.
                if (const TSharedPtr<FBlueprintEditor> ConcreteEditor =
                        StaticCastSharedPtr<FBlueprintEditor>(BlueprintEditor)) {
                    const FGraphPanelSelectionSet Selection =
                        ConcreteEditor->GetSelectedNodes();
                    // Filter to nodes that belong to the same graph as the context
                    // menu.
                    for (UObject *SelectedObject : Selection) {
                        const UEdGraphNode *SelectedNode =
                            Cast<UEdGraphNode>(SelectedObject);
                        if (!SelectedNode || SelectedNode->GetGraph() != ContextGraph) {
                            continue;
                        }

                        Nodes.Add(SelectedNode);
                    }
                }
            }
        }
    }

    // Always include the node that was right-clicked, even if it is not selected.
    if (Context.Node) {
        Nodes.AddUnique(Context.Node);
    }

    // Sort by position (then GUID) to keep auto-layout deterministic for the same
    // selection.
    if (Nodes.Num() > 1) {
        Nodes.Sort([](const UEdGraphNode &Lhs, const UEdGraphNode &Rhs) {
            if (Lhs.NodePosX == Rhs.NodePosX) {
                if (Lhs.NodePosY == Rhs.NodePosY) {
                    return Lhs.NodeGuid < Rhs.NodeGuid;
                }
                return Lhs.NodePosY < Rhs.NodePosY;
            }
            return Lhs.NodePosX < Rhs.NodePosX;
        });
    }

    return Nodes;
}

#if !UE_BUILD_SHIPPING && !UE_BUILD_TEST
FString FormatSizeString(const FVector2D &Size)
{
    return FString::Printf(TEXT("(%.1f, %.1f)"), Size.X, Size.Y);
}

bool TryGetNodeWidgetSizes(const UEdGraphNode *Node, const UEdGraph *Graph,
                           FVector2D &OutAbsoluteSize, FVector2D &OutDesiredSize)
{
    if (!Node || !Graph || !Node->NodeGuid.IsValid()) {
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("TryGetNodeWidgetSizes: missing node/graph/guid (node=%s graph=%s "
                    "guidValid=%d)"),
               Node ? *Node->GetName() : TEXT("null"),
               Graph ? *Graph->GetName() : TEXT("null"),
               (Node && Node->NodeGuid.IsValid()) ? 1 : 0);
        return false;
    }

    const TSharedPtr<SGraphEditor> GraphEditor =
        SGraphEditor::FindGraphEditorForGraph(Graph);
    const UBlueprint *Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
    const TSharedPtr<IBlueprintEditor> BlueprintEditor =
        Blueprint
            ? FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, false)
            : nullptr;
    const UEdGraph *FocusedGraph =
        BlueprintEditor.IsValid() ? BlueprintEditor->GetFocusedGraph() : nullptr;
    const TSharedPtr<SGraphEditor> GraphEditorFromOpen =
        BlueprintEditor.IsValid() ? BlueprintEditor->OpenGraphAndBringToFront(
                                        const_cast<UEdGraph *>(Graph), false)
                                  : nullptr;
    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("TryGetNodeWidgetSizes: GraphEditor find=%p open=%p blueprintEditor=%p "
                "focusedGraph=%s blueprint=%s"),
           GraphEditor.Get(), GraphEditorFromOpen.Get(), BlueprintEditor.Get(),
           FocusedGraph ? *FocusedGraph->GetName() : TEXT("null"),
           Blueprint ? *Blueprint->GetName() : TEXT("null"));
    const TSharedPtr<SGraphEditor> ActiveGraphEditor =
        GraphEditorFromOpen.IsValid() ? GraphEditorFromOpen : GraphEditor;
    if (!ActiveGraphEditor.IsValid()) {
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("TryGetNodeWidgetSizes: no graph editor for graph %s"),
               *Graph->GetName());
        return false;
    }

    SGraphPanel *GraphPanel = ActiveGraphEditor->GetGraphPanel();
    if (!GraphPanel) {
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("TryGetNodeWidgetSizes: graph panel missing for graph %s"),
               *Graph->GetName());
        return false;
    }

    const TSharedPtr<SGraphNode> NodeWidget =
        GraphPanel->GetNodeWidgetFromGuid(Node->NodeGuid);
    if (!NodeWidget.IsValid()) {
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("TryGetNodeWidgetSizes: node widget not found for %s in graph %s"),
               *Node->GetName(), *Graph->GetName());
        const UEdGraph *PanelGraph = GraphPanel->GetGraphObj();
        UE_LOG(LogBlueprintAutoLayout, Verbose,
               TEXT("TryGetNodeWidgetSizes: panel graph=%s nodes=%d"),
               PanelGraph ? *PanelGraph->GetName() : TEXT("null"),
               PanelGraph ? PanelGraph->Nodes.Num() : 0);
        if (PanelGraph) {
            for (const UEdGraphNode *PanelNode : PanelGraph->Nodes) {
                if (!PanelNode) {
                    continue;
                }

                FVector2f MinCorner = FVector2f::ZeroVector;
                FVector2f MaxCorner = FVector2f::ZeroVector;
                const bool bHasWidget =
                    GraphPanel->GetBoundsForNode(PanelNode, MinCorner, MaxCorner, 0.0f);
                const FVector2f Size = MaxCorner - MinCorner;
                UE_LOG(LogBlueprintAutoLayout, Verbose,
                       TEXT("  panelNode=%s class=%s guid=%s hasWidget=%d size=(%.1f, "
                            "%.1f)"),
                       *PanelNode->GetName(), *PanelNode->GetClass()->GetName(),
                       *PanelNode->NodeGuid.ToString(), bHasWidget ? 1 : 0, Size.X,
                       Size.Y);
            }
        }
        return false;
    }

    OutAbsoluteSize = NodeWidget->GetCachedGeometry().GetAbsoluteSize();
    OutDesiredSize = NodeWidget->GetDesiredSize();
    UE_LOG(LogBlueprintAutoLayout, Verbose,
           TEXT("TryGetNodeWidgetSizes: %s abs=(%.1f, %.1f) desired=(%.1f, %.1f)"),
           *Node->GetName(), OutAbsoluteSize.X, OutAbsoluteSize.Y, OutDesiredSize.X,
           OutDesiredSize.Y);
    return true;
}
#endif

void ShowAutoLayoutNotification(const FString &Message, bool bSuccess)
{
    // Configure a short-lived notification with success/failure styling.
    FNotificationInfo Info(FText::FromString(Message));
    Info.ExpireDuration = 3.0f;
    Info.bUseSuccessFailIcons = true;
    Info.FadeOutDuration = 0.3f;

    // Post the notification and mark the result state for icon selection.
    if (TSharedPtr<SNotificationItem> Item =
            FSlateNotificationManager::Get().AddNotification(Info)) {
        Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success
                                          : SNotificationItem::CS_Fail);
    }
}

void HandleAutoLayoutSelectedNodes(const FToolMenuContext &InContext)
{
    // Retrieve the node context used to launch the graph context menu.
    const UGraphNodeContextMenuContext *NodeContext =
        InContext.FindContext<UGraphNodeContextMenuContext>();
    if (!NodeContext) {
        return;
    }

    // Resolve the nodes we will attempt to auto layout.
    const TArray<const UEdGraphNode *> SelectedNodes =
        GatherSelectedNodes(*NodeContext);
    if (SelectedNodes.IsEmpty()) {
        ShowAutoLayoutNotification(TEXT("Select one or more nodes to auto layout."),
                                   false);
        return;
    }

    // Resolve the target graph from the context or the clicked node.
    const UEdGraph *Graph =
        NodeContext->Graph
            ? NodeContext->Graph
            : (NodeContext->Node ? NodeContext->Node->GetGraph() : nullptr);
    if (!Graph) {
        ShowAutoLayoutNotification(TEXT("No graph resolved for auto layout."), false);
        return;
    }

    // Resolve the owning Blueprint so auto layout can apply transactional changes.
    const UBlueprint *Blueprint =
        NodeContext->Blueprint ? NodeContext->Blueprint
                               : FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
    if (!Blueprint) {
        ShowAutoLayoutNotification(TEXT("No Blueprint resolved for auto layout."),
                                   false);
        return;
    }

    // Convert the const selection into a mutable list for the auto-layout API.
    TArray<UEdGraphNode *> MutableNodes;
    MutableNodes.Reserve(SelectedNodes.Num());
    for (const UEdGraphNode *Node : SelectedNodes) {
        if (Node) {
            MutableNodes.AddUnique(const_cast<UEdGraphNode *>(Node));
        }
    }

    // Pull editor-configured settings and attempt the auto layout.
    const K2AutoLayout::FAutoLayoutSettings Settings =
        GetDefault<UBlueprintAutoLayoutSettings>()->ToAutoLayoutSettings();
    K2AutoLayout::FAutoLayoutResult Result;
    if (!K2AutoLayout::AutoLayoutIslands(const_cast<UBlueprint *>(Blueprint),
                                         const_cast<UEdGraph *>(Graph), MutableNodes,
                                         Settings, Result)) {
        // Surface detailed error guidance when available.
        FString Message = Result.Error;
        if (!Result.Guidance.IsEmpty()) {
            Message += TEXT("\n") + Result.Guidance;
        }
        if (Message.IsEmpty()) {
            Message = TEXT("Auto layout failed.");
        }
        ShowAutoLayoutNotification(Message, false);
        return;
    }

    // Report the successful application with a node count.
    const FString Message =
        FString::Printf(TEXT("Auto layout applied (%d nodes)."), Result.NodesLaidOut);
    ShowAutoLayoutNotification(Message, true);
}

bool IsAutoLayoutEntryVisible(const FToolMenuContext &InContext)
{
    // Only show the entry when there is a context node or selection to operate on.
    const UGraphNodeContextMenuContext *NodeContext =
        InContext.FindContext<UGraphNodeContextMenuContext>();
    if (!NodeContext) {
        return false;
    }

    if (NodeContext->Node) {
        return true;
    }

    const TArray<const UEdGraphNode *> SelectedNodes =
        GatherSelectedNodes(*NodeContext);
    return !SelectedNodes.IsEmpty();
}
} // namespace

class FBlueprintAutoLayoutModule : public IModuleInterface
{
  public:
    virtual void StartupModule() override
    {
        // Register menu extensions when tool menus are ready.
        UToolMenus::RegisterStartupCallback(
            FSimpleMulticastDelegate::FDelegate::CreateRaw(
                this, &FBlueprintAutoLayoutModule::RegisterMenus));
    }

    virtual void ShutdownModule() override
    {
        // Unregister menu hooks owned by this module.
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
    }

  private:
    void RegisterMenus()
    {
        // Scope ownership so menu entries are cleaned up when the module shuts down.
        FToolMenuOwnerScoped OwnerScoped(this);

        // Prepare reusable delegates for executing and showing the auto layout entry.
        const FToolMenuExecuteAction AutoLayoutActionExec =
            FToolMenuExecuteAction::CreateStatic(&HandleAutoLayoutSelectedNodes);
        const FToolMenuIsActionButtonVisible AutoLayoutActionVisible =
            FToolMenuIsActionButtonVisible::CreateStatic(&IsAutoLayoutEntryVisible);

        // Helper to inject the Auto Layout section into any graph context menu.
        auto AddAutoLayoutSection = [AutoLayoutActionExec,
                                     AutoLayoutActionVisible](UToolMenu *InMenu) {
            // Skip menus that are not graph context menus.
            const UGraphNodeContextMenuContext *Context =
                InMenu->FindContext<UGraphNodeContextMenuContext>();
            if (!Context) {
                return;
            }

            // Find or create the plugin section inside the menu.
            FToolMenuSection *Section = InMenu->FindSection("BlueprintAutoLayout");
            if (!Section) {
                Section = &InMenu->AddSection(
                    "BlueprintAutoLayout",
                    LOCTEXT("BlueprintAutoLayoutSection", "Auto Layout"));
            }

            // Add the Auto Layout action if it is not already present.
            if (Section && !Section->FindEntry("BlueprintAutoLayout.AutoLayout")) {
                FToolUIAction AutoLayoutAction;
                AutoLayoutAction.ExecuteAction = AutoLayoutActionExec;
                AutoLayoutAction.IsActionVisibleDelegate = AutoLayoutActionVisible;

                Section->AddMenuEntry(
                    "BlueprintAutoLayout.AutoLayout",
                    LOCTEXT("BlueprintAutoLayoutLabel", "Auto Layout"),
                    LOCTEXT("BlueprintAutoLayoutTooltip",
                            "Auto layout the connected island containing the selected "
                            "nodes."),
                    FSlateIcon(), AutoLayoutAction);
            }

            // Add a read-only complexity label for the selected island(s).
            if (Section) {
                const TArray<const UEdGraphNode *> SelectedNodes =
                    GatherSelectedNodes(*Context);
                if (!SelectedNodes.IsEmpty()) {
                    const UEdGraph *ContextGraph =
                        Context->Graph
                            ? Context->Graph
                            : (Context->Node ? Context->Node->GetGraph() : nullptr);
                    const int32 Complexity =
                        K2AutoLayout::CalculateCyclomaticComplexityForSelectionIslands(
                            ContextGraph, SelectedNodes);
                    const FText ComplexityLabel = FText::Format(
                        LOCTEXT("BlueprintAutoLayoutCyclomaticComplexityLabel",
                                "Cyclomatic Complexity: {0}"),
                        FText::AsNumber(Complexity));
                    if (!Section->FindEntry(
                            "BlueprintAutoLayout.CyclomaticComplexity")) {
                        Section->AddMenuEntry(
                            "BlueprintAutoLayout.CyclomaticComplexity", ComplexityLabel,
                            LOCTEXT("BlueprintAutoLayoutCyclomaticComplexityTooltip",
                                    "Cyclomatic complexity for the island(s) "
                                    "containing the selection."),
                            FSlateIcon(), FToolUIAction(),
                            EUserInterfaceActionType::None);
                    }
                }
            }

            // Optionally add debug-only entries for the clicked node.
#if !UE_BUILD_SHIPPING && !UE_BUILD_TEST
            if (Section && Context->Node) {
                const FString NodeGuidString = Context->Node->NodeGuid.ToString();
                const FText NodeGuidLabel = FText::Format(
                    LOCTEXT("BlueprintAutoLayoutNodeGuidLabel", "Node GUID: {0}"),
                    FText::FromString(NodeGuidString));

                const UEdGraph *ContextGraph =
                    Context->Graph
                        ? Context->Graph
                        : (Context->Node ? Context->Node->GetGraph() : nullptr);
                FString AbsoluteSizeString = TEXT("N/A");
                FString DesiredSizeString = TEXT("N/A");
                FVector2D AbsoluteSize = FVector2D::ZeroVector;
                FVector2D DesiredSize = FVector2D::ZeroVector;
                if (TryGetNodeWidgetSizes(Context->Node, ContextGraph, AbsoluteSize,
                                          DesiredSize)) {
                    AbsoluteSizeString = FormatSizeString(AbsoluteSize);
                    DesiredSizeString = FormatSizeString(DesiredSize);
                }
                const FString NodeWidthString =
                    FString::Printf(TEXT("%.1f"), Context->Node->GetWidth());
                const FString NodeHeightString =
                    FString::Printf(TEXT("%.1f"), Context->Node->GetHeight());
                const FText AbsoluteSizeLabel =
                    FText::Format(LOCTEXT("BlueprintAutoLayoutNodeAbsoluteSizeLabel",
                                          "GetAbsoluteSize: {0}"),
                                  FText::FromString(AbsoluteSizeString));
                const FText DesiredSizeLabel =
                    FText::Format(LOCTEXT("BlueprintAutoLayoutNodeDesiredSizeLabel",
                                          "GetDesiredSize: {0}"),
                                  FText::FromString(DesiredSizeString));
                const FText NodeWidthLabel = FText::Format(
                    LOCTEXT("BlueprintAutoLayoutNodeWidthLabel", "Node->GetWidth: {0}"),
                    FText::FromString(NodeWidthString));
                const FText NodeHeightLabel =
                    FText::Format(LOCTEXT("BlueprintAutoLayoutNodeHeightLabel",
                                          "Node->GetHeight: {0}"),
                                  FText::FromString(NodeHeightString));

                if (!Section->FindEntry("BlueprintAutoLayout.NodeGuid")) {
                    Section->AddMenuEntry("BlueprintAutoLayout.NodeGuid", NodeGuidLabel,
                                          LOCTEXT("BlueprintAutoLayoutNodeGuidTooltip",
                                                  "Debug: GUID for the clicked node."),
                                          FSlateIcon(), FToolUIAction(),
                                          EUserInterfaceActionType::None);
                }

                if (!Section->FindEntry("BlueprintAutoLayout.NodeAbsoluteSize")) {
                    Section->AddMenuEntry(
                        "BlueprintAutoLayout.NodeAbsoluteSize", AbsoluteSizeLabel,
                        LOCTEXT("BlueprintAutoLayoutNodeAbsoluteSizeTooltip",
                                "Debug: SGraphNode size from GetAbsoluteSize."),
                        FSlateIcon(), FToolUIAction(), EUserInterfaceActionType::None);
                }

                if (!Section->FindEntry("BlueprintAutoLayout.NodeDesiredSize")) {
                    Section->AddMenuEntry(
                        "BlueprintAutoLayout.NodeDesiredSize", DesiredSizeLabel,
                        LOCTEXT("BlueprintAutoLayoutNodeDesiredSizeTooltip",
                                "Debug: SGraphNode size from GetDesiredSize."),
                        FSlateIcon(), FToolUIAction(), EUserInterfaceActionType::None);
                }

                if (!Section->FindEntry("BlueprintAutoLayout.NodeWidth")) {
                    Section->AddMenuEntry(
                        "BlueprintAutoLayout.NodeWidth", NodeWidthLabel,
                        LOCTEXT("BlueprintAutoLayoutNodeWidthTooltip",
                                "Debug: UEdGraphNode width value."),
                        FSlateIcon(), FToolUIAction(), EUserInterfaceActionType::None);
                }

                if (!Section->FindEntry("BlueprintAutoLayout.NodeHeight")) {
                    Section->AddMenuEntry(
                        "BlueprintAutoLayout.NodeHeight", NodeHeightLabel,
                        LOCTEXT("BlueprintAutoLayoutNodeHeightTooltip",
                                "Debug: UEdGraphNode height value."),
                        FSlateIcon(), FToolUIAction(), EUserInterfaceActionType::None);
                }
            }
#endif
        };

        // Ensure the common graph context menu also gets the Auto Layout entry.
        if (UToolMenu *CommonMenu =
                UToolMenus::Get()->ExtendMenu("GraphEditor.GraphContextMenu.Common")) {
            FToolMenuSection *Section = CommonMenu->FindSection("EdGraphSchema");
            if (!Section) {
                Section = &CommonMenu->AddSection(
                    "EdGraphSchema", LOCTEXT("GraphSchemaSection", "Graph"));
            }

            // Inject the action once, so it appears for general graph schemas.
            if (Section && !Section->FindEntry("BlueprintAutoLayout.AutoLayout")) {
                FToolUIAction AutoLayoutAction;
                AutoLayoutAction.ExecuteAction = AutoLayoutActionExec;
                AutoLayoutAction.IsActionVisibleDelegate = AutoLayoutActionVisible;

                Section->AddMenuEntry(
                    "BlueprintAutoLayout.AutoLayout",
                    LOCTEXT("BlueprintAutoLayoutLabel", "Auto Layout"),
                    LOCTEXT("BlueprintAutoLayoutTooltip",
                            "Auto layout the connected island containing the selected "
                            "nodes."),
                    FSlateIcon(), AutoLayoutAction);
            }
        }

        UToolMenus *ToolMenus = UToolMenus::Get();

        // Add dynamic sections so the entry appears in node-specific menus.
        const FNewToolMenuDelegate AutoLayoutDelegate =
            FNewToolMenuDelegate::CreateLambda(AddAutoLayoutSection);
        auto AddDynamicSectionToMenu = [&](const FName &MenuName) {
            if (UToolMenu *Menu = ToolMenus->ExtendMenu(MenuName)) {
                Menu->AddDynamicSection("BlueprintAutoLayout.Section",
                                        AutoLayoutDelegate);
            }
        };

        // Register against common K2/graph menu names that Unreal uses by default.
        const TArray<FName> MenusToExtend = {
            TEXT("GraphEditor.GraphContextMenu.UEdGraphSchema"),
            TEXT("GraphEditor.GraphContextMenu.UEdGraphSchema_K2"),
            TEXT("GraphEditor.GraphNodeContextMenu.UEdGraphNode"),
            TEXT("GraphEditor.GraphNodeContextMenu.UK2Node")};

        for (const FName &MenuName : MenusToExtend) {
            AddDynamicSectionToMenu(MenuName);
        }

        // Also attach to any graph node class-specific context menus.
        for (TObjectIterator<UClass> It; It; ++It) {
            UClass *Class = *It;
            if (!Class || !Class->IsChildOf(UEdGraphNode::StaticClass())) {
                continue;
            }

            const FName MenuName = *FString::Printf(
                TEXT("GraphEditor.GraphNodeContextMenu.%s"), *Class->GetName());
            AddDynamicSectionToMenu(MenuName);
        }
    }
};

IMPLEMENT_MODULE(FBlueprintAutoLayoutModule, BlueprintAutoLayout)

#undef LOCTEXT_NAMESPACE
