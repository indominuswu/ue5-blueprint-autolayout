// Copyright Epic Games, Inc. All Rights Reserved.

// Ensure the header is included only once.
#pragma once

// Core UE types for API exposure.
#include "CoreMinimal.h"

// Forward declaration for graph type input.
class UEdGraph;
class UEdGraphNode;

// Cyclomatic complexity helpers for Blueprint graphs.
namespace K2AutoLayout
{
// Compute cyclomatic complexity using linked exec output pin counts.
BLUEPRINTAUTOLAYOUT_API int32 CalculateCyclomaticComplexity(const UEdGraph *Graph);

// Compute cyclomatic complexity for islands containing selected nodes.
BLUEPRINTAUTOLAYOUT_API int32 CalculateCyclomaticComplexityForSelectionIslands(
    const UEdGraph *Graph, const TArray<const UEdGraphNode *> &SelectedNodes);
} // namespace K2AutoLayout
