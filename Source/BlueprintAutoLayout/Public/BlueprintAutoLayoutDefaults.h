// Copyright Epic Games, Inc. All Rights Reserved.

// Ensure the header is included only once.
#pragma once

// Core UE types and UENUM macro support.
#include "CoreMinimal.h"

// Alignment options for horizontal rank placement.
UENUM()
enum class EBlueprintAutoLayoutRankAlignment : uint8
{
    Left UMETA(DisplayName = "Left"),
    Center UMETA(DisplayName = "Center"),
    Right UMETA(DisplayName = "Right")
};

// Default values for auto-layout settings.
namespace BlueprintAutoLayout
{
namespace Defaults
{
// Baseline spacing defaults shared by exec and data layout.
inline constexpr float DefaultNodeSpacingX = 300.0f;
inline constexpr float DefaultNodeSpacingY = 60.0f;
inline constexpr float DefaultNodeSpacingXExec = DefaultNodeSpacingX;
inline constexpr float DefaultNodeSpacingXData = DefaultNodeSpacingX;
inline constexpr float DefaultNodeSpacingYExec = DefaultNodeSpacingY;
inline constexpr float DefaultNodeSpacingYData = DefaultNodeSpacingY;

// Placement tuning defaults.
inline constexpr EBlueprintAutoLayoutRankAlignment DefaultRankAlignment =
    EBlueprintAutoLayoutRankAlignment::Center;
inline constexpr int32 DefaultVariableGetMinLength = 1;
} // namespace Defaults
} // namespace BlueprintAutoLayout
