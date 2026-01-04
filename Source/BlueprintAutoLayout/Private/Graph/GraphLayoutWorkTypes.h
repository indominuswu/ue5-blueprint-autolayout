// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Graph/GraphLayout.h"

namespace GraphLayout
{
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
} // namespace GraphLayout
