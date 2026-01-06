// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "BlueprintAutoLayoutDefaults.h"

#include "BlueprintAutoLayoutSettings.generated.h"

namespace K2AutoLayout
{
struct FAutoLayoutSettings;
}

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "Blueprint Auto Layout"))
class BLUEPRINTAUTOLAYOUT_API UBlueprintAutoLayoutSettings : public UDeveloperSettings
{
    GENERATED_BODY()

  public:
    virtual FName GetCategoryName() const override;

    UPROPERTY(EditAnywhere, config, Category = "Spacing", meta = (ClampMin = "0.0", UIMin = "0.0"))
    float NodeSpacingX = BlueprintAutoLayout::Defaults::DefaultNodeSpacingX;

    UPROPERTY(EditAnywhere, config, Category = "Spacing",
              meta = (ClampMin = "0.0", UIMin = "0.0", DisplayName = "Node Spacing Y (Exec)"))
    float NodeSpacingYExec = BlueprintAutoLayout::Defaults::DefaultNodeSpacingYExec;

    UPROPERTY(EditAnywhere, config, Category = "Spacing",
              meta = (ClampMin = "0.0", UIMin = "0.0", DisplayName = "Node Spacing Y (Data)"))
    float NodeSpacingYData = BlueprintAutoLayout::Defaults::DefaultNodeSpacingYData;

    UPROPERTY(EditAnywhere, config, Category = "Placement", meta = (DisplayName = "Rank Alignment"))
    EBlueprintAutoLayoutRankAlignment RankAlignment = BlueprintAutoLayout::Defaults::DefaultRankAlignment;

    K2AutoLayout::FAutoLayoutSettings ToAutoLayoutSettings() const;
};
