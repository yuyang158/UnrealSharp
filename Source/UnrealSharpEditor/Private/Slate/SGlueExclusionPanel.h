#pragma once

#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STreeView.h"

struct FGlueModuleItem
{
	FString Name;
	FString GroupName;
	bool bIsEngine = false;
	bool bEditorOnly = false;
	bool bLocked = false;
	bool bHardSkipped = false;
	bool bExcluded = false;
	FString Status;
	TArray<FString> ForcedBy;
};

struct FGlueGroup
{
	FString Name;
	bool bIsPlugin = false;
	bool bPluginExcluded = false;
	bool bHasLockedModule = false;
	TArray<TSharedPtr<FGlueModuleItem>> Modules;
};

struct FGlueTreeItem
{
	bool bIsGroup = false;
	TSharedPtr<FGlueGroup> Group;
	TSharedPtr<FGlueModuleItem> Module;
	TArray<TSharedPtr<FGlueTreeItem>> Children;
};

class SGlueExclusionPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGlueExclusionPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	static void RegisterTab();
	static void UnregisterTab();
	static void Open();

private:
	static const FName TabId;

	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& SpawnTabArgs);

	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FGlueTreeItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnGetChildren(TSharedPtr<FGlueTreeItem> Item, TArray<TSharedPtr<FGlueTreeItem>>& OutChildren);
	void OnSearchTextChanged(const FText& NewText);
	void RebuildVisibleRoots();
	bool PassesFilter(const TSharedPtr<FGlueTreeItem>& Item) const;
	ECheckBoxState GetModuleCheckState(TSharedPtr<FGlueModuleItem> Module) const;
	ECheckBoxState GetPluginCheckState(TSharedPtr<FGlueGroup> Group) const;
	void OnModuleCheckStateChanged(ECheckBoxState NewState, TSharedPtr<FGlueModuleItem> Module, TSharedPtr<FGlueGroup> Group);
	void OnPluginCheckStateChanged(ECheckBoxState NewState, TSharedPtr<FGlueGroup> Group);
	void RefreshTree();
	EActiveTimerReturnType ExpandGroups(double CurrentTime, float DeltaTime);
	FReply OnApplyClicked();
	bool CanApply() const { return bCanApply; }
	FText GetModuleStatusText(const TSharedPtr<FGlueModuleItem>& Module) const;

	bool LoadManifest(TArray<TSharedPtr<FGlueModuleItem>>& OutModules);
	bool LoadSettings(TSet<FString>& OutExcludedModules, TSet<FString>& OutExcludedPlugins);
	void BuildGroups(const TArray<TSharedPtr<FGlueModuleItem>>& Modules, const TSet<FString>& ExcludedModules, const TSet<FString>& ExcludedPlugins);
	void SetStatus(const FText& NewStatus);

	TArray<TSharedPtr<FGlueTreeItem>> AllRoots;
	TArray<TSharedPtr<FGlueTreeItem>> VisibleRoots;
	TSharedPtr<STreeView<TSharedPtr<FGlueTreeItem>>> TreeView;
	TSharedPtr<STextBlock> StatusTextBlock;
	FString SearchText;
	TArray<FString> PreservedExcludedModules;
	TArray<FString> PreservedExcludedPlugins;
	TSet<FString> KnownModuleNames;
	TSet<FString> KnownPluginNames;
	FText StatusText;
	bool bCanApply = false;
	TArray<FString> ConflictLines;
};
