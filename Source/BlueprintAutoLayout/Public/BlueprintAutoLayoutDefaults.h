// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

UENUM()
enum class EBlueprintAutoLayoutRankAlignment : uint8
{
    Left UMETA(DisplayName = "Left"),
    Center UMETA(DisplayName = "Center"),
    Right UMETA(DisplayName = "Right")
};

namespace BlueprintAutoLayout
{
namespace Defaults
{
inline constexpr float DefaultNodeSpacingX = 300.0f;
inline constexpr float DefaultNodeSpacingY = 60.0f;
inline constexpr float DefaultNodeSpacingYExec = DefaultNodeSpacingY;
inline constexpr float DefaultNodeSpacingYData = DefaultNodeSpacingY;
inline constexpr EBlueprintAutoLayoutRankAlignment DefaultRankAlignment = EBlueprintAutoLayoutRankAlignment::Center;
inline constexpr int32 DefaultVariableGetMinLength = 1;
} // namespace Defaults
} // namespace BlueprintAutoLayout
