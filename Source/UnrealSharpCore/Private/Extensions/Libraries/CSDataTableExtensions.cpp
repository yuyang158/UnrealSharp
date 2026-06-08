#include "Extensions/Libraries/CSDataTableExtensions.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"

#if WITH_EDITOR

namespace
{
	bool SaveDataTableAssetImmediate(UDataTable* DataTable)
	{
		if (!IsValid(DataTable))
		{
			return false;
		}

		UPackage* Package = DataTable->GetOutermost();
		if (!Package)
		{
			return false;
		}

		FAssetRegistryModule::AssetCreated(DataTable);
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(),
			FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		return UPackage::SavePackage(Package, DataTable, *PackageFileName, SaveArgs);
	}
}

FString UCSDataTableExtensions::GetTableAsJSON(const UDataTable* DataTable)
{
	if (!IsValid(DataTable))
	{
		return FString();
	}

	return DataTable->GetTableAsJSON();
}

FString UCSDataTableExtensions::GetTableAsCSV(const UDataTable* DataTable)
{
	if (!IsValid(DataTable))
	{
		return FString();
	}

	return DataTable->GetTableAsCSV();
}

UDataTable* UCSDataTableExtensions::LoadOrCreateDataTable(const FString& PackagePath, const FString& AssetName, UScriptStruct* RowStruct)
{
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		return nullptr;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);
	if (UDataTable* ExistingTable = LoadObject<UDataTable>(nullptr, *ObjectPath))
	{
		if (RowStruct && ExistingTable->RowStruct != RowStruct)
		{
			ExistingTable->Modify();
			ExistingTable->RowStruct = RowStruct;
		}

		return ExistingTable;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UDataTable* NewTable = Cast<UDataTable>(AssetTools.CreateAsset(AssetName, PackagePath, UDataTable::StaticClass(), nullptr));
	if (!IsValid(NewTable))
	{
		return nullptr;
	}

	if (RowStruct)
	{
		NewTable->RowStruct = RowStruct;
	}

	NewTable->MarkPackageDirty();
	return NewTable;
}

TArray<FString> UCSDataTableExtensions::ImportDataTableFromCSV(UDataTable* DataTable, const FString& CSVString, bool bMergeExistingRows)
{
	TArray<FString> Problems;
	if (!IsValid(DataTable))
	{
		Problems.Add(TEXT("Invalid DataTable."));
		return Problems;
	}

	if (!DataTable->RowStruct)
	{
		Problems.Add(TEXT("DataTable has no RowStruct."));
		return Problems;
	}

	if (CSVString.IsEmpty())
	{
		Problems.Add(TEXT("CSV string is empty."));
		return Problems;
	}

	const bool bOldPreserve = DataTable->bPreserveExistingValues;
	DataTable->bPreserveExistingValues = bMergeExistingRows;
	Problems = DataTable->CreateTableFromCSVString(CSVString);
	DataTable->bPreserveExistingValues = bOldPreserve;
	DataTable->MarkPackageDirty();
	return Problems;
}

bool UCSDataTableExtensions::SaveDataTableAsset(UDataTable* DataTable)
{
	if (!IsValid(DataTable))
	{
		return false;
	}

	DataTable->MarkPackageDirty();
	SaveDataTableAssetImmediate(DataTable);
	return true;
}
#endif
