#include "Slate/SGlueExclusionPanel.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "SPrimaryButton.h"

#define LOCTEXT_NAMESPACE "UnrealSharpGlueExclusion"

const FName SGlueExclusionPanel::TabId(TEXT("UnrealSharpGlueExclusion"));

namespace GlueExclusionPanel
{
	static const TCHAR* StatusExclusionIgnored = TEXT("ExclusionIgnored");
	static const TCHAR* StatusExcludedByDefine = TEXT("ExcludedByDefine");
	static const TCHAR* EngineGroupName = TEXT("Engine");
	static const TCHAR* ProjectGroupName = TEXT("Project");

	static FString GetProjectSettingsPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("UnrealSharp.Settings.json")));
	}

	static FString GetManifestPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealSharp"), TEXT("GlueModules.json")));
	}

	static void ReadStringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || Values == nullptr)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Item;
			if (Value.IsValid() && Value->TryGetString(Item))
			{
				Item.TrimStartAndEndInline();
				if (!Item.IsEmpty())
				{
					OutValues.Add(Item);
				}
			}
		}
	}

	static TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	static void SortNames(TArray<FString>& Names)
	{
		Names.Sort([](const FString& Left, const FString& Right)
		{
			return Left.Compare(Right, ESearchCase::IgnoreCase) < 0;
		});
	}

	static int32 GroupRank(const FString& Name)
	{
		if (Name.Equals(EngineGroupName, ESearchCase::IgnoreCase))
		{
			return 0;
		}
		if (Name.Equals(ProjectGroupName, ESearchCase::IgnoreCase))
		{
			return 1;
		}
		return 2;
	}
}

void SGlueExclusionPanel::RegisterTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateStatic(&SGlueExclusionPanel::SpawnTab))
		.SetDisplayName(LOCTEXT("TabTitle", "C# Glue Modules"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Choose which modules and plugins generate UnrealSharp C# glue."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void SGlueExclusionPanel::UnregisterTab()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
}

TSharedRef<SDockTab> SGlueExclusionPanel::SpawnTab(const FSpawnTabArgs& SpawnTabArgs)
{
	(void)SpawnTabArgs;
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(LOCTEXT("TabTitle", "C# Glue Modules"))
		[
			SNew(SGlueExclusionPanel)
		];
}

void SGlueExclusionPanel::Open()
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabId);
}

void SGlueExclusionPanel::Construct(const FArguments& InArgs)
{
	TArray<TSharedPtr<FGlueModuleItem>> Modules;
	TSet<FString> ExcludedModules;
	TSet<FString> ExcludedPlugins;
	const bool bManifestLoaded = LoadManifest(Modules);
	const bool bSettingsLoaded = bManifestLoaded && LoadSettings(ExcludedModules, ExcludedPlugins);
	bCanApply = bManifestLoaded && bSettingsLoaded;
	if (bCanApply)
	{
		BuildGroups(Modules, ExcludedModules, ExcludedPlugins);
		RebuildVisibleRoots();
		RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SGlueExclusionPanel::ExpandGroups));
	}

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 0.f, 0.f, 8.f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(LOCTEXT("Description", "Checked modules generate C# glue. Uncheck a module, or a plugin, to exclude it. Apply writes Config/UnrealSharp.Settings.json. The next project build picks up the change."))
		];

	if (bCanApply)
	{
		Body->AddSlot()
		.AutoHeight()
		.Padding(0.f, 0.f, 0.f, 8.f)
		[
			SNew(SSearchBox)
			.HintText(LOCTEXT("SearchHint", "Search modules"))
			.OnTextChanged(this, &SGlueExclusionPanel::OnSearchTextChanged)
		];

		if (ConflictLines.Num() > 0)
		{
			Body->AddSlot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(SBox)
				.MaxDesiredHeight(140.f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FStyleColors::Warning)
						.Text(FText::FromString(FString::Printf(TEXT("Dependency conflicts from the last build:\n%s\nThese warnings describe that build. Save and rebuild to refresh them."), *FString::Join(ConflictLines, TEXT("\n")))))
					]
				]
			];
		}

		Body->AddSlot()
		.FillHeight(1.f)
		[
			SAssignNew(TreeView, STreeView<TSharedPtr<FGlueTreeItem>>)
			.TreeItemsSource(&VisibleRoots)
			.OnGenerateRow(this, &SGlueExclusionPanel::OnGenerateRow)
			.OnGetChildren(this, &SGlueExclusionPanel::OnGetChildren)
			.SelectionMode(ESelectionMode::None)
		];
	}
	else
	{
		Body->AddSlot()
		.FillHeight(1.f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Justification(ETextJustify::Center)
			.Text_Lambda([this]() { return StatusText; })
		];
	}

	Body->AddSlot()
	.AutoHeight()
	.Padding(0.f, 8.f, 0.f, 8.f)
	[
		SAssignNew(StatusTextBlock, STextBlock)
		.AutoWrapText(true)
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		.Text_Lambda([this]() { return StatusText; })
	];

	Body->AddSlot()
	.AutoHeight()
	.HAlign(HAlign_Right)
	[
		SNew(SPrimaryButton)
		.Text(LOCTEXT("Apply", "Apply"))
		.IsEnabled(this, &SGlueExclusionPanel::CanApply)
		.OnClicked(this, &SGlueExclusionPanel::OnApplyClicked)
	];

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(12.f)
		[
			SNew(SBox)
			.MinDesiredWidth(720.f)
			.MinDesiredHeight(480.f)
			[
				Body
			]
		]
	];
}

TSharedRef<ITableRow> SGlueExclusionPanel::OnGenerateRow(TSharedPtr<FGlueTreeItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	TSharedRef<SHorizontalBox> Content = SNew(SHorizontalBox);

	if (Item.IsValid() && Item->bIsGroup && Item->Group.IsValid())
	{
		const TSharedPtr<FGlueGroup> Group = Item->Group;
		if (Group->bIsPlugin)
		{
			Content->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsEnabled(!Group->bHasLockedModule)
				.ToolTipText(Group->bHasLockedModule
					? LOCTEXT("LockedPluginTooltip", "This plugin contains modules that always generate C# glue.")
					: LOCTEXT("PluginTooltip", "Exclude every module in this plugin from C# glue generation."))
				.IsChecked_Lambda([this, Group]() { return GetPluginCheckState(Group); })
				.OnCheckStateChanged_Lambda([this, Group](ECheckBoxState NewState) { OnPluginCheckStateChanged(NewState, Group); })
			];
		}

		Content->AddSlot()
		.FillWidth(1.f)
		.VAlign(VAlign_Center)
		.Padding(6.f, 2.f, 0.f, 2.f)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.Text(FText::FromString(FString::Printf(TEXT("%s (%d)"), *Group->Name, Group->Modules.Num())))
		];
	}
	else if (Item.IsValid() && Item->Module.IsValid())
	{
		const TSharedPtr<FGlueModuleItem> Module = Item->Module;
		const TSharedPtr<FGlueGroup> Group = Item->Group;
		const bool bCanToggle = !Module->bLocked && !Module->bHardSkipped;
		FText Tooltip = LOCTEXT("ModuleTooltip", "Include this module in C# glue generation.");
		if (Module->bLocked)
		{
			Tooltip = LOCTEXT("LockedModuleTooltip", "This module always generates C# glue.");
		}
		else if (Module->bHardSkipped)
		{
			Tooltip = LOCTEXT("HardSkipTooltip", "SkipGlueGeneration is a hard skip. This list cannot turn the module back on.");
		}

		Content->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SCheckBox)
			.IsEnabled(bCanToggle)
			.ToolTipText(Tooltip)
			.IsChecked_Lambda([this, Module]() { return GetModuleCheckState(Module); })
			.OnCheckStateChanged_Lambda([this, Module, Group](ECheckBoxState NewState) { OnModuleCheckStateChanged(NewState, Module, Group); })
		];

		Content->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.f, 1.f, 0.f, 1.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Module->Name))
		];

		if (Module->bEditorOnly)
		{
			Content->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("EditorOnly", "Editor Only"))
			];
		}

		Content->AddSlot()
		.FillWidth(1.f)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Center)
		.Padding(12.f, 0.f, 4.f, 0.f)
		[
			SNew(STextBlock)
			.ColorAndOpacity(Module->Status == GlueExclusionPanel::StatusExclusionIgnored ? FStyleColors::Warning : FSlateColor::UseSubduedForeground())
			.Text(GetModuleStatusText(Module))
			.ToolTipText(LOCTEXT("StatusTooltip", "Status from the last build."))
		];
	}

	return SNew(STableRow<TSharedPtr<FGlueTreeItem>>, OwnerTable)
		.Padding(FMargin(2.f, 1.f))
		[
			Content
		];
}

void SGlueExclusionPanel::OnGetChildren(TSharedPtr<FGlueTreeItem> Item, TArray<TSharedPtr<FGlueTreeItem>>& OutChildren)
{
	if (!Item.IsValid() || !Item->bIsGroup)
	{
		return;
	}

	for (const TSharedPtr<FGlueTreeItem>& Child : Item->Children)
	{
		if (PassesFilter(Child))
		{
			OutChildren.Add(Child);
		}
	}
}

void SGlueExclusionPanel::OnSearchTextChanged(const FText& NewText)
{
	SearchText = NewText.ToString();
	RefreshTree();
}

void SGlueExclusionPanel::RebuildVisibleRoots()
{
	VisibleRoots.Reset();
	for (const TSharedPtr<FGlueTreeItem>& Root : AllRoots)
	{
		bool bAnyVisible = false;
		for (const TSharedPtr<FGlueTreeItem>& Child : Root->Children)
		{
			if (PassesFilter(Child))
			{
				bAnyVisible = true;
				break;
			}
		}

		if (bAnyVisible)
		{
			VisibleRoots.Add(Root);
		}
	}
}

bool SGlueExclusionPanel::PassesFilter(const TSharedPtr<FGlueTreeItem>& Item) const
{
	if (!Item.IsValid() || !Item->Module.IsValid())
	{
		return false;
	}

	if (SearchText.IsEmpty())
	{
		return true;
	}

	return Item->Module->Name.Contains(SearchText) || (Item->Group.IsValid() && Item->Group->Name.Contains(SearchText));
}

ECheckBoxState SGlueExclusionPanel::GetModuleCheckState(TSharedPtr<FGlueModuleItem> Module) const
{
	if (!Module.IsValid() || Module->bHardSkipped || Module->bExcluded)
	{
		return ECheckBoxState::Unchecked;
	}

	return ECheckBoxState::Checked;
}

ECheckBoxState SGlueExclusionPanel::GetPluginCheckState(TSharedPtr<FGlueGroup> Group) const
{
	if (!Group.IsValid())
	{
		return ECheckBoxState::Unchecked;
	}

	if (Group->bPluginExcluded)
	{
		return ECheckBoxState::Unchecked;
	}

	int32 ToggleableCount = 0;
	int32 ExcludedCount = 0;
	for (const TSharedPtr<FGlueModuleItem>& Module : Group->Modules)
	{
		if (!Module.IsValid() || Module->bLocked)
		{
			continue;
		}

		++ToggleableCount;
		if (Module->bExcluded)
		{
			++ExcludedCount;
		}
	}

	if (ToggleableCount == 0 || ExcludedCount == 0)
	{
		return ECheckBoxState::Checked;
	}

	if (ExcludedCount == ToggleableCount)
	{
		return ECheckBoxState::Unchecked;
	}

	return ECheckBoxState::Undetermined;
}

void SGlueExclusionPanel::OnModuleCheckStateChanged(ECheckBoxState NewState, TSharedPtr<FGlueModuleItem> Module, TSharedPtr<FGlueGroup> Group)
{
	if (!Module.IsValid() || Module->bLocked || Module->bHardSkipped)
	{
		return;
	}

	const bool bWantExcluded = NewState != ECheckBoxState::Checked;
	if (Group.IsValid() && Group->bPluginExcluded && !bWantExcluded)
	{
		Group->bPluginExcluded = false;
		for (const TSharedPtr<FGlueModuleItem>& Other : Group->Modules)
		{
			if (Other.IsValid() && !Other->bLocked && Other != Module)
			{
				Other->bExcluded = true;
			}
		}
		Module->bExcluded = false;
	}
	else
	{
		Module->bExcluded = bWantExcluded;
		if (!bWantExcluded && Group.IsValid())
		{
			Group->bPluginExcluded = false;
		}
	}

	RefreshTree();
}

void SGlueExclusionPanel::OnPluginCheckStateChanged(ECheckBoxState NewState, TSharedPtr<FGlueGroup> Group)
{
	(void)NewState;
	if (!Group.IsValid() || Group->bHasLockedModule)
	{
		return;
	}

	bool bAnyIncluded = false;
	for (const TSharedPtr<FGlueModuleItem>& Module : Group->Modules)
	{
		if (Module.IsValid() && !Module->bLocked && !Module->bExcluded)
		{
			bAnyIncluded = true;
			break;
		}
	}

	Group->bPluginExcluded = bAnyIncluded;
	for (const TSharedPtr<FGlueModuleItem>& Module : Group->Modules)
	{
		if (Module.IsValid() && !Module->bLocked)
		{
			Module->bExcluded = bAnyIncluded;
		}
	}

	RefreshTree();
}

void SGlueExclusionPanel::RefreshTree()
{
	RebuildVisibleRoots();
	if (!TreeView.IsValid())
	{
		return;
	}

	TreeView->RequestTreeRefresh();
	for (const TSharedPtr<FGlueTreeItem>& Root : VisibleRoots)
	{
		TreeView->SetItemExpansion(Root, true);
	}
}

EActiveTimerReturnType SGlueExclusionPanel::ExpandGroups(double CurrentTime, float DeltaTime)
{
	(void)CurrentTime;
	(void)DeltaTime;
	RefreshTree();
	return EActiveTimerReturnType::Stop;
}

FReply SGlueExclusionPanel::OnApplyClicked()
{
	const FString SettingsPath = GlueExclusionPanel::GetProjectSettingsPath();
	TSharedPtr<FJsonObject> JsonObject;
	if (FPaths::FileExists(SettingsPath))
	{
		FString JsonString;
		if (!FFileHelper::LoadFileToString(JsonString, *SettingsPath))
		{
			SetStatus(FText::Format(LOCTEXT("ReadFailed", "Could not read {0}."), FText::FromString(SettingsPath)));
			return FReply::Handled();
		}

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		{
			SetStatus(FText::Format(LOCTEXT("ParseFailed", "{0} is not valid JSON, so it was left unchanged."), FText::FromString(SettingsPath)));
			return FReply::Handled();
		}
	}
	else
	{
		JsonObject = MakeShared<FJsonObject>();
	}

	TSet<FString> ExcludedModules;
	TSet<FString> ExcludedPlugins;
	for (const FString& Name : PreservedExcludedModules)
	{
		ExcludedModules.Add(Name);
	}
	for (const FString& Name : PreservedExcludedPlugins)
	{
		ExcludedPlugins.Add(Name);
	}

	for (const TSharedPtr<FGlueTreeItem>& Root : AllRoots)
	{
		if (!Root.IsValid() || !Root->Group.IsValid())
		{
			continue;
		}

		const TSharedPtr<FGlueGroup> Group = Root->Group;
		if (Group->bIsPlugin && Group->bPluginExcluded && !Group->bHasLockedModule)
		{
			ExcludedPlugins.Add(Group->Name);
			continue;
		}

		for (const TSharedPtr<FGlueModuleItem>& Module : Group->Modules)
		{
			if (!Module.IsValid() || Module->bLocked || !Module->bExcluded)
			{
				continue;
			}

			ExcludedModules.Add(Module->Name);
		}
	}

	TArray<FString> ModuleNames = ExcludedModules.Array();
	TArray<FString> PluginNames = ExcludedPlugins.Array();
	GlueExclusionPanel::SortNames(ModuleNames);
	GlueExclusionPanel::SortNames(PluginNames);

	JsonObject->SetArrayField(TEXT("GlueExcludedModules"), GlueExclusionPanel::ToJsonArray(ModuleNames));
	JsonObject->SetArrayField(TEXT("GlueExcludedPlugins"), GlueExclusionPanel::ToJsonArray(PluginNames));

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer))
	{
		SetStatus(LOCTEXT("SerializeFailed", "Could not write the settings JSON."));
		return FReply::Handled();
	}
	Writer->Close();

	if (!FFileHelper::SaveStringToFile(Output, *SettingsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		SetStatus(FText::Format(LOCTEXT("WriteFailed", "Could not write {0}."), FText::FromString(SettingsPath)));
		return FReply::Handled();
	}

	PreservedExcludedModules.Reset();
	PreservedExcludedPlugins.Reset();
	for (const FString& Name : ModuleNames)
	{
		if (!KnownModuleNames.Contains(Name))
		{
			PreservedExcludedModules.Add(Name);
		}
	}
	for (const FString& Name : PluginNames)
	{
		if (!KnownPluginNames.Contains(Name))
		{
			PreservedExcludedPlugins.Add(Name);
		}
	}

	SetStatus(LOCTEXT("Saved", "Saved. Rebuild the project so Unreal Header Tool regenerates C# glue."));
	return FReply::Handled();
}

FText SGlueExclusionPanel::GetModuleStatusText(const TSharedPtr<FGlueModuleItem>& Module) const
{
	if (!Module.IsValid())
	{
		return FText::GetEmpty();
	}

	if (Module->Status == GlueExclusionPanel::StatusExclusionIgnored)
	{
		const FString Forcers = Module->ForcedBy.Num() > 0 ? FString::Join(Module->ForcedBy, TEXT(", ")) : TEXT("another generated module");
		return FText::Format(LOCTEXT("KeptBy", "Kept (used by {0})"), FText::FromString(Forcers));
	}

	if (Module->Status == GlueExclusionPanel::StatusExcludedByDefine)
	{
		return LOCTEXT("HardSkipStatus", "SkipGlueGeneration");
	}

	if (Module->bLocked)
	{
		return LOCTEXT("RequiredStatus", "Required");
	}

	if (Module->Status == TEXT("ExcludedByConfig"))
	{
		return LOCTEXT("ExcludedStatus", "Excluded");
	}

	return LOCTEXT("GeneratedStatus", "Generated");
}

bool SGlueExclusionPanel::LoadManifest(TArray<TSharedPtr<FGlueModuleItem>>& OutModules)
{
	const FString ManifestPath = GlueExclusionPanel::GetManifestPath();
	if (!FPaths::FileExists(ManifestPath))
	{
		SetStatus(LOCTEXT("ManifestMissing", "No glue module list was found. Build the project once so Unreal Header Tool can write Intermediate/UnrealSharp/GlueModules.json."));
		return false;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *ManifestPath))
	{
		SetStatus(FText::Format(LOCTEXT("ManifestReadFailed", "Could not read {0}."), FText::FromString(ManifestPath)));
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		SetStatus(FText::Format(LOCTEXT("ManifestParseFailed", "{0} is not valid JSON."), FText::FromString(ManifestPath)));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ModulesJson = nullptr;
	if (!Root->TryGetArrayField(TEXT("modules"), ModulesJson) || ModulesJson == nullptr)
	{
		SetStatus(FText::Format(LOCTEXT("ManifestSchema", "{0} has no modules array."), FText::FromString(ManifestPath)));
		return false;
	}

	ConflictLines.Reset();
	KnownModuleNames.Reset();
	KnownPluginNames.Reset();

	for (const TSharedPtr<FJsonValue>& Value : *ModulesJson)
	{
		const TSharedPtr<FJsonObject>* ModuleObjectPtr = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(ModuleObjectPtr) || ModuleObjectPtr == nullptr || !ModuleObjectPtr->IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject> ModuleObject = *ModuleObjectPtr;
		FString Name;
		if (!ModuleObject->TryGetStringField(TEXT("name"), Name))
		{
			continue;
		}
		Name.TrimStartAndEndInline();
		if (Name.IsEmpty() || KnownModuleNames.Contains(Name))
		{
			continue;
		}

		bool bIsEngine = false;
		ModuleObject->TryGetBoolField(TEXT("isEngine"), bIsEngine);

		FString PluginName;
		if (!ModuleObject->TryGetStringField(TEXT("plugin"), PluginName))
		{
			PluginName = bIsEngine ? GlueExclusionPanel::EngineGroupName : GlueExclusionPanel::ProjectGroupName;
		}

		bool bIsPlugin = false;
		if (!ModuleObject->TryGetBoolField(TEXT("isPlugin"), bIsPlugin))
		{
			bIsPlugin = !PluginName.Equals(GlueExclusionPanel::EngineGroupName, ESearchCase::IgnoreCase)
				&& !PluginName.Equals(GlueExclusionPanel::ProjectGroupName, ESearchCase::IgnoreCase);
		}

		TSharedRef<FGlueModuleItem> Module = MakeShared<FGlueModuleItem>();
		Module->Name = Name;
		Module->GroupName = PluginName;
		Module->bIsEngine = bIsEngine;
		ModuleObject->TryGetBoolField(TEXT("editorOnly"), Module->bEditorOnly);
		ModuleObject->TryGetBoolField(TEXT("locked"), Module->bLocked);
		ModuleObject->TryGetStringField(TEXT("status"), Module->Status);
		Module->bHardSkipped = Module->Status == GlueExclusionPanel::StatusExcludedByDefine;

		const TArray<TSharedPtr<FJsonValue>>* ForcedByJson = nullptr;
		if (ModuleObject->TryGetArrayField(TEXT("forcedBy"), ForcedByJson) && ForcedByJson != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& ForcedValue : *ForcedByJson)
			{
				FString ForcedName;
				if (ForcedValue.IsValid() && ForcedValue->TryGetString(ForcedName) && !ForcedName.IsEmpty())
				{
					Module->ForcedBy.Add(ForcedName);
				}
			}
		}

		if (Module->Status == GlueExclusionPanel::StatusExclusionIgnored)
		{
			const FString Forcers = Module->ForcedBy.Num() > 0 ? FString::Join(Module->ForcedBy, TEXT(", ")) : TEXT("another generated module");
			ConflictLines.Add(FString::Printf(TEXT("%s was excluded in config, but the last build still generated it because these modules depend on it: %s."), *Module->Name, *Forcers));
		}

		KnownModuleNames.Add(Name);
		if (bIsPlugin)
		{
			KnownPluginNames.Add(PluginName);
		}
		OutModules.Add(Module);
	}

	GlueExclusionPanel::SortNames(ConflictLines);
	return true;
}

bool SGlueExclusionPanel::LoadSettings(TSet<FString>& OutExcludedModules, TSet<FString>& OutExcludedPlugins)
{
	PreservedExcludedModules.Reset();
	PreservedExcludedPlugins.Reset();

	const FString SettingsPath = GlueExclusionPanel::GetProjectSettingsPath();
	if (!FPaths::FileExists(SettingsPath))
	{
		return true;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *SettingsPath))
	{
		SetStatus(FText::Format(LOCTEXT("SettingsReadFailed", "Could not read {0}."), FText::FromString(SettingsPath)));
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		SetStatus(FText::Format(LOCTEXT("SettingsParseFailed", "{0} is not valid JSON, so it was left unchanged."), FText::FromString(SettingsPath)));
		return false;
	}

	TArray<FString> ExcludedModules;
	TArray<FString> ExcludedPlugins;
	GlueExclusionPanel::ReadStringArray(Root, TEXT("GlueExcludedModules"), ExcludedModules);
	GlueExclusionPanel::ReadStringArray(Root, TEXT("GlueExcludedPlugins"), ExcludedPlugins);

	for (const FString& Name : ExcludedModules)
	{
		OutExcludedModules.Add(Name);
		if (!KnownModuleNames.Contains(Name))
		{
			PreservedExcludedModules.Add(Name);
		}
	}

	for (const FString& Name : ExcludedPlugins)
	{
		OutExcludedPlugins.Add(Name);
		if (!KnownPluginNames.Contains(Name))
		{
			PreservedExcludedPlugins.Add(Name);
		}
	}

	return true;
}

void SGlueExclusionPanel::BuildGroups(const TArray<TSharedPtr<FGlueModuleItem>>& Modules, const TSet<FString>& ExcludedModules, const TSet<FString>& ExcludedPlugins)
{
	TMap<FString, TSharedPtr<FGlueGroup>> Groups;
	for (const TSharedPtr<FGlueModuleItem>& Module : Modules)
	{
		if (!Module.IsValid())
		{
			continue;
		}

		TSharedPtr<FGlueGroup>& Group = Groups.FindOrAdd(Module->GroupName);
		if (!Group.IsValid())
		{
			Group = MakeShared<FGlueGroup>();
			Group->Name = Module->GroupName;
			Group->bIsPlugin = KnownPluginNames.Contains(Module->GroupName);
		}

		if (Module->bLocked)
		{
			Group->bHasLockedModule = true;
		}
		Group->Modules.Add(Module);
	}

	for (const TPair<FString, TSharedPtr<FGlueGroup>>& Pair : Groups)
	{
		const TSharedPtr<FGlueGroup>& Group = Pair.Value;
		Group->Modules.Sort([](const TSharedPtr<FGlueModuleItem>& Left, const TSharedPtr<FGlueModuleItem>& Right)
		{
			return Left->Name.Compare(Right->Name, ESearchCase::IgnoreCase) < 0;
		});

		Group->bPluginExcluded = Group->bIsPlugin && !Group->bHasLockedModule && ExcludedPlugins.Contains(Group->Name);
		for (const TSharedPtr<FGlueModuleItem>& Module : Group->Modules)
		{
			if (Module->bLocked)
			{
				Module->bExcluded = false;
				continue;
			}

			Module->bExcluded = Group->bPluginExcluded || ExcludedModules.Contains(Module->Name);
		}

		TSharedRef<FGlueTreeItem> GroupItem = MakeShared<FGlueTreeItem>();
		GroupItem->bIsGroup = true;
		GroupItem->Group = Group;
		for (const TSharedPtr<FGlueModuleItem>& Module : Group->Modules)
		{
			TSharedRef<FGlueTreeItem> ModuleItem = MakeShared<FGlueTreeItem>();
			ModuleItem->Module = Module;
			ModuleItem->Group = Group;
			GroupItem->Children.Add(ModuleItem);
		}
		AllRoots.Add(GroupItem);
	}

	AllRoots.Sort([](const TSharedPtr<FGlueTreeItem>& Left, const TSharedPtr<FGlueTreeItem>& Right)
	{
		const int32 LeftRank = GlueExclusionPanel::GroupRank(Left->Group->Name);
		const int32 RightRank = GlueExclusionPanel::GroupRank(Right->Group->Name);
		if (LeftRank != RightRank)
		{
			return LeftRank < RightRank;
		}
		return Left->Group->Name.Compare(Right->Group->Name, ESearchCase::IgnoreCase) < 0;
	});
}

void SGlueExclusionPanel::SetStatus(const FText& NewStatus)
{
	StatusText = NewStatus;
	if (StatusTextBlock.IsValid())
	{
		StatusTextBlock->SetText(StatusText);
	}
}

#undef LOCTEXT_NAMESPACE
