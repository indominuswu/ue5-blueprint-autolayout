// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Graph/GraphLayout.h"

namespace GraphLayout
{
namespace KeyUtils
{
inline bool GuidLess(const FGuid &A, const FGuid &B)
{
    if (A.A != B.A) {
        return A.A < B.A;
    }
    if (A.B != B.B) {
        return A.B < B.B;
    }
    if (A.C != B.C) {
        return A.C < B.C;
    }
    return A.D < B.D;
}

inline int32 CompareNodeKey(const FNodeKey &A, const FNodeKey &B)
{
    if (A.Guid != B.Guid) {
        return GuidLess(A.Guid, B.Guid) ? -1 : 1;
    }
    return 0;
}

inline bool NodeKeyLess(const FNodeKey &A, const FNodeKey &B)
{
    return CompareNodeKey(A, B) < 0;
}

inline FString BuildNodeKeyString(const FNodeKey &Key)
{
    return Key.Guid.ToString(EGuidFormats::DigitsWithHyphens);
}

inline int32 ComparePinKey(const FNodeKey &NodeKeyA, int32 DirectionA,
                           const FName &PinNameA, int32 PinIndexA,
                           const FNodeKey &NodeKeyB, int32 DirectionB,
                           const FName &PinNameB, int32 PinIndexB)
{
    int32 Compare = CompareNodeKey(NodeKeyA, NodeKeyB);
    if (Compare != 0) {
        return Compare;
    }
    if (DirectionA != DirectionB) {
        return DirectionA < DirectionB ? -1 : 1;
    }
    if (PinNameA != PinNameB) {
        return PinNameA.LexicalLess(PinNameB) ? -1 : 1;
    }
    if (PinIndexA != PinIndexB) {
        return PinIndexA < PinIndexB ? -1 : 1;
    }
    return 0;
}

inline FString BuildPinKeyString(const FNodeKey &NodeKey, const TCHAR *DirectionLabel,
                                 const FName &PinName, int32 PinIndex)
{
    return FString::Printf(TEXT("%s|%s|%s|%d"), *BuildNodeKeyString(NodeKey),
                           DirectionLabel, *PinName.ToString(), PinIndex);
}
} // namespace KeyUtils
} // namespace GraphLayout
