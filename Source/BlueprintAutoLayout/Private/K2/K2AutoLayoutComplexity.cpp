// Copyright Epic Games, Inc. All Rights Reserved.

// Cyclomatic complexity implementation for Blueprint graphs.
#include "K2/K2AutoLayoutComplexity.h"

// Graph node and pin types used to inspect exec outputs.
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

// Blueprint graph cyclomatic complexity helpers.
namespace K2AutoLayout
{
namespace
{
// Count linked exec output pins on a node for complexity scoring.
int32 CountExecOutputPins(const UEdGraphNode *Node)
{
    // Treat null nodes as having zero exec outputs.
    if (!Node) {
        return 0;
    }

    // Walk pins and count exec outputs.
    int32 ExecOutputPins = 0;
    for (const UEdGraphPin *Pin : Node->Pins) {
        if (!Pin) {
            continue;
        }
        if (Pin->Direction != EGPD_Output) {
            continue;
        }
        if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) {
            continue;
        }

        // Only count exec outputs that have at least one valid link.
        bool bHasLinkedPins = false;
        for (const UEdGraphPin *LinkedPin : Pin->LinkedTo) {
            if (LinkedPin) {
                bHasLinkedPins = true;
                break;
            }
        }
        if (!bHasLinkedPins) {
            continue;
        }
        ++ExecOutputPins;
    }

    // Return the total exec output pin count.
    return ExecOutputPins;
}

// Calculate cyclomatic complexity for a specific node set.
int32 CalculateCyclomaticComplexityForNodes(const TArray<const UEdGraphNode *> &Nodes)
{
    // Treat empty node sets as zero complexity.
    if (Nodes.IsEmpty()) {
        return 0;
    }

    // Start from one to represent the base execution path.
    int32 Complexity = 1;

    // Add fan-out contributions from each node's exec outputs.
    for (const UEdGraphNode *Node : Nodes) {
        const int32 ExecOutPins = CountExecOutputPins(Node);
        Complexity += FMath::Max(0, ExecOutPins - 1);
    }

    // Return the computed complexity.
    return Complexity;
}

// Gather nodes connected by pin links into a component.
void GatherConnectedNodes(const UEdGraphNode *Seed, const UEdGraph *Graph,
                          TSet<const UEdGraphNode *> &Visited,
                          TArray<const UEdGraphNode *> &OutComponentNodes)
{
    // Guard against invalid inputs.
    if (!Seed || !Graph) {
        return;
    }

    // Seed the traversal stack with the starting node.
    TArray<const UEdGraphNode *> Stack;
    Stack.Add(Seed);
    Visited.Add(Seed);

    // Walk the graph via pin links to collect the component.
    while (!Stack.IsEmpty()) {
        const UEdGraphNode *Current = Stack.Pop();
        OutComponentNodes.Add(Current);

        // Traverse linked pins to discover neighbors.
        for (const UEdGraphPin *Pin : Current->Pins) {
            if (!Pin) {
                continue;
            }
            for (const UEdGraphPin *LinkedPin : Pin->LinkedTo) {
                if (!LinkedPin) {
                    continue;
                }
                const UEdGraphNode *LinkedNode = LinkedPin->GetOwningNode();
                if (!LinkedNode || LinkedNode->GetGraph() != Graph) {
                    continue;
                }
                if (Visited.Contains(LinkedNode)) {
                    continue;
                }
                Visited.Add(LinkedNode);
                Stack.Add(LinkedNode);
            }
        }
    }
}
} // namespace

// Calculate cyclomatic complexity using linked exec output fan-out.
int32 CalculateCyclomaticComplexity(const UEdGraph *Graph)
{
    // Treat null graphs as zero complexity to avoid misleading results.
    if (!Graph) {
        return 0;
    }

    // Start from one to represent the base execution path.
    int32 Complexity = 1;

    // Add fan-out contributions from each node's exec outputs.
    for (const UEdGraphNode *Node : Graph->Nodes) {
        const int32 ExecOutPins = CountExecOutputPins(Node);
        Complexity += FMath::Max(0, ExecOutPins - 1);
    }

    // Return the computed complexity.
    return Complexity;
}

// Calculate cyclomatic complexity for islands touched by a selection.
int32 CalculateCyclomaticComplexityForSelectionIslands(
    const UEdGraph *Graph, const TArray<const UEdGraphNode *> &SelectedNodes)
{
    // Treat invalid inputs as zero complexity.
    if (!Graph || SelectedNodes.IsEmpty()) {
        return 0;
    }

    // Filter selection to nodes that belong to the target graph.
    TArray<const UEdGraphNode *> SeedNodes;
    SeedNodes.Reserve(SelectedNodes.Num());
    for (const UEdGraphNode *Node : SelectedNodes) {
        if (!Node || Node->GetGraph() != Graph) {
            continue;
        }
        SeedNodes.AddUnique(Node);
    }

    // Bail out if there are no valid seeds.
    if (SeedNodes.IsEmpty()) {
        return 0;
    }

    // Walk each connected component touched by the selection.
    TSet<const UEdGraphNode *> Visited;
    Visited.Reserve(SeedNodes.Num());
    int32 TotalComplexity = 0;
    for (const UEdGraphNode *Seed : SeedNodes) {
        if (!Seed || Visited.Contains(Seed)) {
            continue;
        }

        // Traverse the component starting from this seed.
        TArray<const UEdGraphNode *> ComponentNodes;
        GatherConnectedNodes(Seed, Graph, Visited, ComponentNodes);
        TotalComplexity += CalculateCyclomaticComplexityForNodes(ComponentNodes);
    }

    // Return the aggregated complexity.
    return TotalComplexity;
}
} // namespace K2AutoLayout
