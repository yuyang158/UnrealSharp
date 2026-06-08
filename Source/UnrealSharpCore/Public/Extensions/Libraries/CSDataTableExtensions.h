#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CSDataTableExtensions.generated.h"

UCLASS()
class UCSDataTableExtensions : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
#if WITH_EDITOR
	UFUNCTION(meta=(ScriptMethod))
	static FString GetTableAsJSON(const UDataTable* DataTable);

	UFUNCTION(meta=(ScriptMethod))
	static FString GetTableAsCSV(const UDataTable* DataTable);

	UFUNCTION(meta=(ScriptMethod))
	static UDataTable* LoadOrCreateDataTable(const FString& PackagePath, const FString& AssetName, UScriptStruct* RowStruct);

	UFUNCTION(meta=(ScriptMethod))
	static TArray<FString> ImportDataTableFromCSV(UDataTable* DataTable, const FString& CSVString, bool bMergeExistingRows);

	UFUNCTION(meta=(ScriptMethod))
	static bool SaveDataTableAsset(UDataTable* DataTable);
#endif
};

