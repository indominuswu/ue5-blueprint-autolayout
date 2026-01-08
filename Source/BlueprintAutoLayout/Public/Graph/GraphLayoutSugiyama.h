// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Graph/GraphLayout.h"
#include "Graph/GraphLayoutKeyUtils.h"

namespace GraphLayout
{
// Tuning constants for debug output.
inline constexpr int32 kVerboseDumpNodeLimit = 120;
inline constexpr int32 kVerboseDumpEdgeLimit = 240;
inline constexpr int32 kVerboseCrossingDetailLimit = 64;

// Helper wrappers to keep comparisons and formatted keys centralized.
inline int32 CompareNodeKey(const FNodeKey &A, const FNodeKey &B)
{
    return KeyUtils::CompareNodeKey(A, B);
}

inline bool NodeKeyLess(const FNodeKey &A, const FNodeKey &B)
{
    return KeyUtils::NodeKeyLess(A, B);
}

inline FString BuildNodeKeyString(const FNodeKey &Key)
{
    return KeyUtils::BuildNodeKeyString(Key);
}

enum class EPinDirection : uint8
{
    Input = 0,
    Output = 1
};

// Composite pin identity used for stable edge ordering and logs.
struct FPinKey
{
    FNodeKey NodeKey;
    EPinDirection Direction = EPinDirection::Input;
    FName PinName;
    int32 PinIndex = 0;
};

inline int32 ComparePinKey(const FPinKey &A, const FPinKey &B)
{
    return KeyUtils::ComparePinKey(
        A.NodeKey, static_cast<int32>(A.Direction), A.PinName, A.PinIndex, B.NodeKey,
        static_cast<int32>(B.Direction), B.PinName, B.PinIndex);
}

inline bool PinKeyLess(const FPinKey &A, const FPinKey &B)
{
    return ComparePinKey(A, B) < 0;
}

inline FPinKey MakePinKey(const FNodeKey &Owner, EPinDirection Direction,
                          const FName &PinName, int32 PinIndex)
{
    FPinKey Key;
    Key.NodeKey = Owner;
    Key.Direction = Direction;
    Key.PinName = PinName;
    Key.PinIndex = PinIndex;
    return Key;
}

inline FString BuildPinKeyString(const FPinKey &Key)
{
    const TCHAR *Dir = Key.Direction == EPinDirection::Input ? TEXT("I") : TEXT("O");
    return KeyUtils::BuildPinKeyString(Key.NodeKey, Dir, Key.PinName, Key.PinIndex);
}

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
    bool bIsVariableGet = false;
    bool bIsReroute = false;
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
    int32 MinLen = 1;
    bool bReversed = false;
};

struct FSugiyamaGraph
{
    TArray<FSugiyamaNode> Nodes;
    TArray<FSugiyamaEdge> Edges;
};

inline int32 CountDummyNodes(const FSugiyamaGraph &Graph)
{
    int32 Count = 0;
    for (const FSugiyamaNode &Node : Graph.Nodes) {
        if (Node.bIsDummy) {
            ++Count;
        }
    }
    return Count;
}

inline bool ShouldDumpDetail(int32 NodeCount, int32 EdgeCount)
{
    return NodeCount <= kVerboseDumpNodeLimit && EdgeCount <= kVerboseDumpEdgeLimit;
}

inline bool ShouldDumpSugiyamaDetail(const FSugiyamaGraph &Graph)
{
    return ShouldDumpDetail(Graph.Nodes.Num(), Graph.Edges.Num());
}

void AssignInitialOrder(FSugiyamaGraph &Graph, int32 MaxRank,
                        TArray<TArray<int32>> &RankNodes, const TCHAR *Label);
void RunCrossingReduction(FSugiyamaGraph &Graph, int32 MaxRank, int32 NumSweeps,
                          TArray<TArray<int32>> &RankNodes, const TCHAR *Label);
} // namespace GraphLayout
