// Copyright Epic Games, Inc. All Rights Reserved.

// Ensure the header is included only once.
#pragma once

// Core UE types and developer settings base.
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

// Default values for layout settings.
#include "BlueprintAutoLayoutDefaults.h"

// Generated settings definition.
#include "BlueprintAutoLayoutSettings.generated.h"

// Forward declaration for settings translation.
namespace K2AutoLayout
{
struct FAutoLayoutSettings;
}

// Editor-facing settings for auto layout defaults.
UCLASS(config = EditorPerProjectUserSettings, defaultconfig,
       meta = (DisplayName = "Blueprint Auto Layout"))
class BLUEPRINTAUTOLAYOUT_API UBlueprintAutoLayoutSettings : public UDeveloperSettings
{
    GENERATED_BODY()

  public:
    // Report the settings category shown in the editor UI.
    virtual FName GetCategoryName() const override;

    // Legacy single-value spacing used for config migration.
    UPROPERTY(config)
    float NodeSpacingX = BlueprintAutoLayout::Defaults::DefaultNodeSpacingX;

    // Per-type spacing controls for exec/data nodes.
    UPROPERTY(EditAnywhere, config, Category = "Spacing",
              meta = (ClampMin = "0.0", UIMin = "0.0",
                      DisplayName = "Node Spacing X (Exec)"))
    float NodeSpacingXExec = BlueprintAutoLayout::Defaults::DefaultNodeSpacingXExec;
    UPROPERTY(EditAnywhere, config, Category = "Spacing",
              meta = (ClampMin = "0.0", UIMin = "0.0",
                      DisplayName = "Node Spacing X (Data)"))
    float NodeSpacingXData = BlueprintAutoLayout::Defaults::DefaultNodeSpacingXData;
    UPROPERTY(EditAnywhere, config, Category = "Spacing",
              meta = (ClampMin = "0.0", UIMin = "0.0",
                      DisplayName = "Node Spacing Y (Exec)"))
    float NodeSpacingYExec = BlueprintAutoLayout::Defaults::DefaultNodeSpacingYExec;
    UPROPERTY(EditAnywhere, config, Category = "Spacing",
              meta = (ClampMin = "0.0", UIMin = "0.0",
                      DisplayName = "Node Spacing Y (Data)"))
    float NodeSpacingYData = BlueprintAutoLayout::Defaults::DefaultNodeSpacingYData;

    // Placement tuning parameters.
    UPROPERTY(EditAnywhere, config, Category = "Placement",
              meta = (DisplayName = "Place Variable Get Under Destination Node",
                      ToolTip = "Place Variable Get nodes under destination nodes."))
    bool bPlaceVariableGetUnderDestination =
        BlueprintAutoLayout::Defaults::DefaultPlaceVariableGetUnderDestination;
    UPROPERTY(EditAnywhere, config, Category = "Placement",
              meta = (DisplayName = "Column Alignment"))
    EBlueprintAutoLayoutRankAlignment RankAlignment =
        BlueprintAutoLayout::Defaults::DefaultRankAlignment;
    UPROPERTY(EditAnywhere, config, Category = "Placement",
              meta = (DisplayName = "Align Exec Chains Horizontally",
                      ToolTip = "Align exec chains to be as horizontal as possible."))
    bool bAlignExecChainsHorizontally =
        BlueprintAutoLayout::Defaults::DefaultAlignExecChainsHorizontally;

    // Convert editor settings to runtime layout settings.
    K2AutoLayout::FAutoLayoutSettings ToAutoLayoutSettings() const;
};
