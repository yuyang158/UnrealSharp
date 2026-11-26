#include "ProjectSourceCodeNavigation.h"

#include "Framework/Notifications/NotificationManager.h"
#include "Features/IModularFeatures.h"
#include "Misc/FileHelper.h"
#include "ISourceCodeAccessor.h"
#include "ISourceCodeAccessModule.h"
#include "Misc/App.h"
#include "Misc/EngineVersionComparison.h"
#include "Modules/ModuleManager.h"
#include "UnrealSharpEditor.h"
#include "Types/CSClass.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "ProjectSourceCodeNavigation"

namespace
{
	// Report failures like native source navigation does: a notification instead of a blocking dialog.
	void NotifyOpenClassFailed(const FText& Reason)
	{
		FNotificationInfo Info(FText::Format(LOCTEXT("OpenClassFailed", "Failed to open C# source: {0}"), Reason));
		Info.ExpireDuration = 5.0f;
		if (const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Notification->SetCompletionState(SNotificationItem::CS_Fail);
		}
	}
}

bool FProjectSourceCodeNavigation::CanNavigateToClass(const UClass* Class)
{
	const UCSClass* ManagedClass = Cast<UCSClass>(Class);
	return IsValid(ManagedClass) && ManagedClass->HasManagedTypeDefinition();
}

bool FProjectSourceCodeNavigation::NavigateToClass(const UClass* Class)
{
	if (!CanNavigateToClass(Class))
	{
		return false;
	}

	OpenClass(Class);
	// The request was handled, including any error. Do not fall back to C++ header lookup.
	return true;
}

bool FProjectSourceCodeNavigation::OpenClass(const UClass* Class)
{
	const UCSClass* ManagedClass = Cast<UCSClass>(Class);
	if (!IsValid(ManagedClass) || !ManagedClass->HasManagedTypeDefinition())
	{
		return false;
	}

	const FCSManagedEditorCallbacks& Callbacks = FUnrealSharpEditorModule::Get().GetManagedEditorCallbacks();
	if (!Callbacks.FindClassSource)
	{
		NotifyOpenClassFailed(LOCTEXT("EditorNotReady", "The C# editor is not initialized yet. Try again after the C# solution has loaded."));
		return false;
	}

	const FCSFieldName& FieldName = ManagedClass->GetReflectionData()->FieldName;
	FString FilePath;
	FString Error;
	int32 Line = 0;
	int32 Column = 0;
	if (!Callbacks.FindClassSource(*FieldName.GetAssemblyName().ToString(), *FieldName.GetFullName(), &FilePath, &Line, &Column, &Error))
	{
		NotifyOpenClassFailed(FText::FromString(Error));
		return false;
	}

	if (TryOpenInRunningVisualStudio(FilePath, Line, Column))
	{
		return true;
	}

	// No project solution is already open. Fall back to the editor's source-code accessor.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	return FSourceCodeNavigation::OpenSourceFile(FilePath, Line, Column);
#else
	FSourceCodeNavigation::OpenSourceFileAsync(FilePath, Line, Column);
	return true;
#endif
}

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/AllowWindowsPlatformAtomics.h"
#include <objbase.h>
#include <oleauto.h>
#include "Microsoft/COMPointer.h"

namespace
{
	struct FComApartmentScope
	{
		HRESULT Result = E_FAIL;

		FComApartmentScope()
		{
			Result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		}

		~FComApartmentScope()
		{
			if (Result == S_OK || Result == S_FALSE)
			{
				CoUninitialize();
			}
		}

		bool CanUseCom() const
		{
			return SUCCEEDED(Result) || Result == RPC_E_CHANGED_MODE;
		}
	};

	static bool IsSelectedEditorVisualStudio()
	{
		const ISourceCodeAccessModule* SourceCodeAccessModule = FModuleManager::GetModulePtr<ISourceCodeAccessModule>(TEXT("SourceCodeAccess"));
		if (!SourceCodeAccessModule)
		{
			return false;
		}

		// Editor Preferences stores a version wrapper such as VisualStudio2022 or VisualStudio2026.
		// VisualStudioSourceCodeAccessor is only the inner implementation and is not the selected name.
		const FString AccessorName = SourceCodeAccessModule->GetAccessor().GetFName().ToString();
		return AccessorName.StartsWith(TEXT("VisualStudio")) && !AccessorName.StartsWith(TEXT("VisualStudioCode"));
	}

	// Higher is better. Zero means this Visual Studio is not editing the current project.
	static int32 ScoreRunningSolution(const FString& SolutionPath, const FString& SourceFile)
	{
		FString NormalizedSolution = SolutionPath;
		FPaths::NormalizeFilename(NormalizedSolution);
		if (NormalizedSolution.IsEmpty())
		{
			return 0;
		}

		FString ProjectDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(ProjectDirectory);
		if (!FPaths::IsUnderDirectory(NormalizedSolution, ProjectDirectory))
		{
			return 0;
		}

		FString SolutionDirectory = FPaths::GetPath(NormalizedSolution);
		FPaths::NormalizeDirectoryName(SolutionDirectory);
		FString NormalizedFile = SourceFile;
		FPaths::NormalizeFilename(NormalizedFile);

		int32 Score = 1000;
		if (FPaths::IsUnderDirectory(NormalizedFile, SolutionDirectory))
		{
			// A solution closer to the file (ManagedUE_Client.sln) beats the native solution at the project root.
			Score += SolutionDirectory.Len();
		}

		const FString MixedFileName = FString(FApp::GetProjectName()) + TEXT(".Mixed.sln");
		if (FPaths::GetCleanFilename(NormalizedSolution).Equals(MixedFileName, ESearchCase::IgnoreCase))
		{
			// Mixed.sln is the solution that contains both the native projects and the C# projects.
			Score += 100000;
		}

		return Score;
	}

	template <typename FuncType>
	static HRESULT InvokeComWithRetry(FuncType&& Func)
	{
		HRESULT Result = E_FAIL;
		for (int32 Attempt = 0; Attempt < 8; ++Attempt)
		{
			Result = Func();
			if (Result != RPC_E_CALL_REJECTED && Result != RPC_E_SERVERCALL_RETRYLATER)
			{
				return Result;
			}

			FPlatformProcess::Sleep(0.05f);
		}

		return Result;
	}

	static HRESULT GetDispId(IDispatch* Object, const wchar_t* Name, DISPID& OutDispId)
	{
		if (!Object || !Name)
		{
			return E_POINTER;
		}

		wchar_t NameBuffer[128];
		if (wcscpy_s(NameBuffer, Name) != 0)
		{
			return E_INVALIDARG;
		}

		LPOLESTR NamePointer = NameBuffer;
		return Object->GetIDsOfNames(IID_NULL, &NamePointer, 1, LOCALE_USER_DEFAULT, &OutDispId);
	}

	static HRESULT GetProperty(IDispatch* Object, const wchar_t* Name, VARIANT& OutValue)
	{
		VariantClear(&OutValue);
		VariantInit(&OutValue);
		return InvokeComWithRetry([&]()
		{
			VariantClear(&OutValue);
			VariantInit(&OutValue);

			DISPID DispId = DISPID_UNKNOWN;
			const HRESULT NameResult = GetDispId(Object, Name, DispId);
			if (FAILED(NameResult))
			{
				return NameResult;
			}

			DISPPARAMS Params = {};
			return Object->Invoke(DispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &Params, &OutValue, nullptr, nullptr);
		});
	}

	// ReversedArgs is IDispatch order: the last method parameter is ReversedArgs[0].
	static HRESULT InvokeMethod(IDispatch* Object, const wchar_t* Name, VARIANT* ReversedArgs, UINT ArgCount, VARIANT* OutResult)
	{
		if (OutResult)
		{
			VariantClear(OutResult);
			VariantInit(OutResult);
		}

		return InvokeComWithRetry([&]()
		{
			if (OutResult)
			{
				VariantClear(OutResult);
				VariantInit(OutResult);
			}

			DISPID DispId = DISPID_UNKNOWN;
			const HRESULT NameResult = GetDispId(Object, Name, DispId);
			if (FAILED(NameResult))
			{
				return NameResult;
			}

			DISPPARAMS Params = {};
			Params.cArgs = ArgCount;
			Params.rgvarg = ReversedArgs;
			return Object->Invoke(DispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &Params, OutResult, nullptr, nullptr);
		});
	}

	static VARIANT MakeBStrVariant(const wchar_t* Text)
	{
		VARIANT Value;
		VariantInit(&Value);
		Value.vt = VT_BSTR;
		Value.bstrVal = SysAllocString(Text ? Text : L"");
		return Value;
	}

	static bool TryMoveCaret(IDispatch* Instance, int32 LineNumber, int32 ColumnNumber)
	{
		const int32 SafeLine = FMath::Max(LineNumber, 1);
		const int32 SafeColumn = FMath::Max(ColumnNumber, 1);

		for (int32 Attempt = 0; Attempt < 5; ++Attempt)
		{
			VARIANT Document;
			VariantInit(&Document);
			const bool bHasDocument = SUCCEEDED(GetProperty(Instance, L"ActiveDocument", Document)) &&
				Document.vt == VT_DISPATCH && Document.pdispVal;

			VARIANT Selection;
			VariantInit(&Selection);
			const bool bHasSelection = bHasDocument &&
				SUCCEEDED(GetProperty(Document.pdispVal, L"Selection", Selection)) &&
				Selection.vt == VT_DISPATCH && Selection.pdispVal;

			bool bMoved = false;
			if (bHasSelection)
			{
				VARIANT Extend;
				VariantInit(&Extend);
				Extend.vt = VT_BOOL;
				Extend.boolVal = VARIANT_FALSE;

				VARIANT Column;
				VariantInit(&Column);
				Column.vt = VT_I4;
				Column.lVal = SafeColumn;

				VARIANT LineArg;
				VariantInit(&LineArg);
				LineArg.vt = VT_I4;
				LineArg.lVal = SafeLine;

				// MoveToLineAndOffset(Line, Column, Extend) is passed last-argument-first.
				VARIANT MoveArgs[3] = { Extend, Column, LineArg };
				VARIANT MoveResult;
				VariantInit(&MoveResult);
				if (SUCCEEDED(InvokeMethod(Selection.pdispVal, L"MoveToLineAndOffset", MoveArgs, 3, &MoveResult)))
				{
					bMoved = true;
				}
				else
				{
					VARIANT SelectLine;
					VariantInit(&SelectLine);
					SelectLine.vt = VT_BOOL;
					SelectLine.boolVal = VARIANT_TRUE;
					VARIANT GotoArgs[2] = { SelectLine, LineArg };
					if (SUCCEEDED(InvokeMethod(Selection.pdispVal, L"GotoLine", GotoArgs, 2, &MoveResult)))
					{
						bMoved = true;
					}
					VariantClear(&SelectLine);
				}

				VariantClear(&MoveResult);
				VariantClear(&LineArg);
				VariantClear(&Column);
				VariantClear(&Extend);
			}

			VariantClear(&Selection);
			VariantClear(&Document);
			if (bMoved)
			{
				return true;
			}

			FPlatformProcess::Sleep(0.1f);
		}

		return false;
	}
}

bool FProjectSourceCodeNavigation::TryOpenInRunningVisualStudio(const FString& FullPath, int32 LineNumber, int32 ColumnNumber)
{
	if (!IsSelectedEditorVisualStudio() || FullPath.IsEmpty() || !FPaths::FileExists(FullPath))
	{
		const ISourceCodeAccessModule* SourceCodeAccessModule = FModuleManager::GetModulePtr<ISourceCodeAccessModule>(TEXT("SourceCodeAccess"));
		const FString AccessorName = SourceCodeAccessModule ? SourceCodeAccessModule->GetAccessor().GetFName().ToString() : TEXT("<missing>");
		UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] TryOpen skipped. Accessor='%s' path='%s' exists=%d"), *AccessorName, *FullPath, FPaths::FileExists(FullPath) ? 1 : 0);
		return false;
	}
	UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] TryOpen '%s' line %d"), *FullPath, LineNumber);

	FComApartmentScope ComScope;
	if (!ComScope.CanUseCom())
	{
		UE_LOG(LogUnrealSharpEditor, Warning, TEXT("Could not initialize COM to reach a running Visual Studio."));
		return false;
	}

	TComPtr<IDispatch> BestInstance;
	int32 BestScore = 0;
	FString BestSolution;

	IRunningObjectTable* RunningObjectTable = nullptr;
	if (FAILED(GetRunningObjectTable(0, &RunningObjectTable)) || !RunningObjectTable)
	{
		return false;
	}

	IEnumMoniker* Monikers = nullptr;
	if (FAILED(RunningObjectTable->EnumRunning(&Monikers)) || !Monikers)
	{
		RunningObjectTable->Release();
		return false;
	}

	Monikers->Reset();
	IMoniker* Moniker = nullptr;
	while (Monikers->Next(1, &Moniker, nullptr) == S_OK)
	{
		IBindCtx* BindContext = nullptr;
		LPOLESTR DisplayName = nullptr;
		if (SUCCEEDED(CreateBindCtx(0, &BindContext)) && BindContext &&
			SUCCEEDED(Moniker->GetDisplayName(BindContext, nullptr, &DisplayName)) && DisplayName &&
			wcsstr(DisplayName, L"VisualStudio.DTE") != nullptr)
		{
			IUnknown* Unknown = nullptr;
			if (SUCCEEDED(RunningObjectTable->GetObject(Moniker, &Unknown)) && Unknown)
			{
				TComPtr<IDispatch> Instance;
				if (SUCCEEDED(Instance.FromQueryInterface(IID_IDispatch, Unknown)))
				{
					VARIANT SolutionValue;
					VariantInit(&SolutionValue);
					if (SUCCEEDED(GetProperty(Instance.Get(), L"Solution", SolutionValue)) &&
						SolutionValue.vt == VT_DISPATCH && SolutionValue.pdispVal)
					{
						VARIANT FullNameValue;
						VariantInit(&FullNameValue);
						if (SUCCEEDED(GetProperty(SolutionValue.pdispVal, L"FullName", FullNameValue)) &&
							FullNameValue.vt == VT_BSTR && FullNameValue.bstrVal)
						{
							const FString SolutionPath(FullNameValue.bstrVal);
							const int32 Score = ScoreRunningSolution(SolutionPath, FullPath);
							if (Score > BestScore)
							{
								BestScore = Score;
								BestSolution = SolutionPath;
								BestInstance = Instance;
							}
						}
						VariantClear(&FullNameValue);
					}
					VariantClear(&SolutionValue);
				}
				Unknown->Release();
			}
		}

		if (DisplayName)
		{
			CoTaskMemFree(DisplayName);
		}
		if (BindContext)
		{
			BindContext->Release();
		}
		Moniker->Release();
		Moniker = nullptr;
	}

	Monikers->Release();
	RunningObjectTable->Release();

	if (!BestInstance.IsValid())
	{
		UE_LOG(LogUnrealSharpEditor, Verbose, TEXT("No running Visual Studio has a solution for this project."));
		return false;
	}

	VARIANT MainWindow;
	VariantInit(&MainWindow);
	if (SUCCEEDED(GetProperty(BestInstance.Get(), L"MainWindow", MainWindow)) &&
		MainWindow.vt == VT_DISPATCH && MainWindow.pdispVal)
	{
		InvokeMethod(MainWindow.pdispVal, L"Activate", nullptr, 0, nullptr);
	}
	VariantClear(&MainWindow);

	VARIANT ItemOperations;
	VariantInit(&ItemOperations);
	if (FAILED(GetProperty(BestInstance.Get(), L"ItemOperations", ItemOperations)) ||
		ItemOperations.vt != VT_DISPATCH || !ItemOperations.pdispVal)
	{
		VariantClear(&ItemOperations);
		UE_LOG(LogUnrealSharpEditor, Warning, TEXT("Visual Studio solution '%s' is open but not ready to open files."), *BestSolution);
		return false;
	}

	FString PlatformPath = FullPath;
	FPaths::MakePlatformFilename(PlatformPath);
	VARIANT FileNameArg = MakeBStrVariant(*PlatformPath);
	// EnvDTE.Constants.vsViewKindTextView. {7651A702-...} is vsViewKindDesigner and opens the [Design] tab.
	VARIANT ViewKindArg = MakeBStrVariant(L"{7651A703-06E5-11D1-8EBD-00A0C90F26EA}");

	// OpenFile(FileName, ViewKind): IDispatch wants the last argument first. These aliases share the BSTRs.
	VARIANT OpenArgs[2] = { ViewKindArg, FileNameArg };
	VARIANT OpenResult;
	VariantInit(&OpenResult);
	HRESULT OpenResultCode = InvokeMethod(ItemOperations.pdispVal, L"OpenFile", OpenArgs, 2, &OpenResult);
	if (FAILED(OpenResultCode))
	{
		VARIANT FileOnlyArg = FileNameArg;
		OpenResultCode = InvokeMethod(ItemOperations.pdispVal, L"OpenFile", &FileOnlyArg, 1, &OpenResult);
	}

	VariantClear(&OpenResult);
	VariantClear(&ViewKindArg);
	VariantClear(&FileNameArg);
	VariantClear(&ItemOperations);

	if (FAILED(OpenResultCode))
	{
		UE_LOG(LogUnrealSharpEditor, Warning, TEXT("Found Visual Studio solution '%s' but could not open '%s' (0x%08x)."), *BestSolution, *FullPath, OpenResultCode);
		return false;
	}

	if (!TryMoveCaret(BestInstance.Get(), LineNumber, ColumnNumber))
	{
		UE_LOG(LogUnrealSharpEditor, Warning, TEXT("Opened '%s' in '%s' but could not move to line %d."), *FullPath, *BestSolution, LineNumber);
	}
	else
	{
		UE_LOG(LogUnrealSharpEditor, Log, TEXT("Opened '%s' in the running Visual Studio solution '%s'."), *FullPath, *BestSolution);
	}

	return true;
}

#include "Windows/HideWindowsPlatformAtomics.h"
#include "Windows/HideWindowsPlatformTypes.h"
#else
bool FProjectSourceCodeNavigation::TryOpenInRunningVisualStudio(const FString& FullPath, int32 LineNumber, int32 ColumnNumber)
{
	return false;
}
#endif

namespace
{
	static const FName SourceCodeAccessorFeatureName(TEXT("SourceCodeAccessor"));

	bool LineHasFunctionToken(const FString& Line, const FString& Token)
	{
		int32 SearchFrom = 0;
		while (SearchFrom < Line.Len())
		{
			const int32 Found = Line.Find(Token, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (Found == INDEX_NONE)
			{
				return false;
			}

			const int32 End = Found + Token.Len();
			const bool bContinuesIdentifier = End < Line.Len() && (FChar::IsAlnum(Line[End]) || Line[End] == TEXT('_'));
			if (!bContinuesIdentifier)
			{
				return true;
			}

			SearchFrom = End;
		}

		return false;
	}

	int32 FindFunctionLine(const TArray<FString>& Lines, const FString& QualifiedToken, const FString& ImplementationToken)
	{
		int32 QualifiedLine = INDEX_NONE;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			if (LineHasFunctionToken(Lines[Index], ImplementationToken))
			{
				return Index + 1;
			}

			if (QualifiedLine == INDEX_NONE && LineHasFunctionToken(Lines[Index], QualifiedToken))
			{
				QualifiedLine = Index + 1;
			}
		}

		return QualifiedLine;
	}

	FString FindSiblingCpp(const FString& HeaderPath)
	{
		const FString CppName = FPaths::GetBaseFilename(HeaderPath) + TEXT(".cpp");
		const FString SameFolderCpp = FPaths::GetPath(HeaderPath) / CppName;
		if (FPaths::FileExists(SameFolderCpp))
		{
			return SameFolderCpp;
		}

		FString SwappedHeader = HeaderPath;
		FPaths::NormalizeFilename(SwappedHeader);
		SwappedHeader.ReplaceInline(TEXT("/Public/"), TEXT("/Private/"));
		SwappedHeader.ReplaceInline(TEXT("/Classes/"), TEXT("/Private/"));
		const FString SwappedCpp = FPaths::GetPath(SwappedHeader) / CppName;
		if (FPaths::FileExists(SwappedCpp))
		{
			return SwappedCpp;
		}

		return FString();
	}

	// PDB line info for a UFUNCTION points at the UHT thunk in Intermediate/.../UHT/*.gen.cpp.
	// The authored declaration and body live in the module source named by ModuleRelativePath.
	bool TryRedirectUhtGeneratedFile(FString& InOutPath, int32& InOutLine, int32& InOutColumn)
	{
		FString NormalizedPath = InOutPath;
		FPaths::NormalizeFilename(NormalizedPath);
		UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Redirect check '%s'"), *NormalizedPath);
		if (!NormalizedPath.Contains(TEXT("/Intermediate/")) || !NormalizedPath.EndsWith(TEXT(".gen.cpp")))
		{
			UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Not a UHT .gen.cpp, leave the path unchanged."));
			return false;
		}

		const int32 IncIndex = NormalizedPath.Find(TEXT("/Inc/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (IncIndex == INDEX_NONE)
		{
			UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] .gen.cpp has no /Inc/ segment."));
			return false;
		}

		FString ModuleName;
		FString Remainder;
		if (!NormalizedPath.Mid(IncIndex + 5).Split(TEXT("/"), &ModuleName, &Remainder) || ModuleName.IsEmpty())
		{
			UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Could not parse module name from '%s'."), *NormalizedPath.Mid(IncIndex));
			return false;
		}

		FString ModuleBasePath;
		if (!FSourceCodeNavigation::FindModulePath(ModuleName, ModuleBasePath))
		{
			const FString ProjectModulePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source") / ModuleName);
			UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] FindModulePath failed for '%s'. Fallback '%s' exists=%d."), *ModuleName, *ProjectModulePath, FPaths::DirectoryExists(ProjectModulePath));
			if (!FPaths::DirectoryExists(ProjectModulePath))
			{
				return false;
			}
			ModuleBasePath = ProjectModulePath;
		}
		UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Module '%s' base '%s'."), *ModuleName, *ModuleBasePath);

		TArray<FString> GeneratedLines;
		if (!FFileHelper::LoadFileToStringArray(GeneratedLines, *InOutPath) || GeneratedLines.IsEmpty())
		{
			UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Failed to read generated file '%s'."), *InOutPath);
			return false;
		}

		FString RelativeHeaderPath;
		FString FunctionName;
		const int32 StartLine = FMath::Clamp(InOutLine - 1, 0, GeneratedLines.Num() - 1);
		for (int32 Index = StartLine; Index >= 0; --Index)
		{
			const FString& Line = GeneratedLines[Index];
			if (RelativeHeaderPath.IsEmpty())
			{
				const FString PathMarker = TEXT("\"ModuleRelativePath\", \"");
				const int32 MarkerIndex = Line.Find(PathMarker, ESearchCase::CaseSensitive);
				if (MarkerIndex != INDEX_NONE)
				{
					const FString AfterMarker = Line.Mid(MarkerIndex + PathMarker.Len());
					int32 QuoteIndex = INDEX_NONE;
					if (AfterMarker.FindChar(TEXT('"'), QuoteIndex))
					{
						RelativeHeaderPath = AfterMarker.Left(QuoteIndex);
					}
				}
			}

			if (FunctionName.IsEmpty() && Line.Contains(TEXT("Begin Class")) && Line.Contains(TEXT(" Function ")))
			{
				const int32 FunctionIndex = Line.Find(TEXT(" Function "), ESearchCase::CaseSensitive);
				FString Name = Line.Mid(FunctionIndex + 10);
				int32 StarIndex = INDEX_NONE;
				if (Name.FindChar(TEXT('*'), StarIndex))
				{
					Name = Name.Left(StarIndex);
				}
				Name.TrimStartAndEndInline();
				FunctionName = Name;
			}

			if (!RelativeHeaderPath.IsEmpty() && !FunctionName.IsEmpty())
			{
				break;
			}

			if (Index < StartLine && Line.Contains(TEXT("End Class")) && Line.Contains(TEXT("Function")))
			{
				break;
			}
		}

		UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Parsed function '%s' header '%s' from line %d."), *FunctionName, *RelativeHeaderPath, InOutLine);
		if (RelativeHeaderPath.IsEmpty())
		{
			UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] No ModuleRelativePath above line %d."), InOutLine);
			return false;
		}

		const FString HeaderPath = FPaths::ConvertRelativePathToFull(ModuleBasePath / RelativeHeaderPath);
		if (!FPaths::FileExists(HeaderPath))
		{
			UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Header does not exist: '%s'."), *HeaderPath);
			return false;
		}

		FString OpenPath = HeaderPath;
		int32 OpenLine = 1;
		if (!FunctionName.IsEmpty())
		{
			const FString QualifiedToken = FString::Printf(TEXT("::%s"), *FunctionName);
			const FString ImplementationToken = FString::Printf(TEXT("::%s_Implementation"), *FunctionName);
			const FString CppPath = FindSiblingCpp(HeaderPath);
			if (!CppPath.IsEmpty())
			{
				TArray<FString> CppLines;
				if (FFileHelper::LoadFileToStringArray(CppLines, *CppPath))
				{
					const int32 CppLine = FindFunctionLine(CppLines, QualifiedToken, ImplementationToken);
					if (CppLine != INDEX_NONE)
					{
						OpenPath = CppPath;
						OpenLine = CppLine;
					}
				}
			}

			if (OpenPath == HeaderPath)
			{
				TArray<FString> HeaderLines;
				if (FFileHelper::LoadFileToStringArray(HeaderLines, *HeaderPath))
				{
					for (int32 Index = 0; Index < HeaderLines.Num(); ++Index)
					{
						if (LineHasFunctionToken(HeaderLines[Index], FunctionName) && HeaderLines[Index].Contains(TEXT("(")))
						{
							OpenLine = Index + 1;
							break;
						}
					}
				}
			}
		}

		UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Redirected UHT file '%s' to '%s:%d'."), *NormalizedPath, *OpenPath, OpenLine);
		InOutPath = OpenPath;
		InOutLine = OpenLine;
		InOutColumn = 1;
		return true;
	}

	class FRunningVisualStudioSourceAccessor final : public ISourceCodeAccessor
	{
	public:
		explicit FRunningVisualStudioSourceAccessor(ISourceCodeAccessor& InInner)
			: Inner(&InInner)
		{
		}

		virtual void RefreshAvailability() override { Inner->RefreshAvailability(); }
		virtual bool CanAccessSourceCode() const override { return Inner->CanAccessSourceCode(); }
		virtual FName GetFName() const override { return Inner->GetFName(); }
		virtual FText GetNameText() const override { return Inner->GetNameText(); }
		virtual FText GetDescriptionText() const override { return Inner->GetDescriptionText(); }
		virtual bool OpenSolution() override { return Inner->OpenSolution(); }
		virtual bool OpenSolutionAtPath(const FString& InSolutionPath) override { return Inner->OpenSolutionAtPath(InSolutionPath); }
		virtual bool DoesSolutionExist() const override { return Inner->DoesSolutionExist(); }

		virtual bool OpenFileAtLine(const FString& FullPath, int32 LineNumber, int32 ColumnNumber) override
		{
			UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] OpenFileAtLine '%s' line %d column %d"), *FullPath, LineNumber, ColumnNumber);
			FString Path = FullPath;
			int32 Line = LineNumber;
			int32 Column = ColumnNumber;
			const bool bRedirected = TryRedirectUhtGeneratedFile(Path, Line, Column);
			UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] OpenFileAtLine redirected=%d path='%s' line=%d"), bRedirected ? 1 : 0, *Path, Line);

			if (FProjectSourceCodeNavigation::TryOpenInRunningVisualStudio(Path, Line, Column))
			{
				UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Opened in the running Visual Studio."));
				return true;
			}

			UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Running Visual Studio did not take the file. Falling back to the editor accessor."));
			return Inner->OpenFileAtLine(Path, Line, Column);
		}

		virtual bool OpenSourceFiles(const TArray<FString>& AbsoluteSourcePaths) override
		{
			TArray<FString> Remaining;
			bool bOpenedAny = false;
			for (const FString& SourcePath : AbsoluteSourcePaths)
			{
				FString Path = SourcePath;
				int32 Line = 1;
				int32 Column = 1;
				TryRedirectUhtGeneratedFile(Path, Line, Column);
				if (FProjectSourceCodeNavigation::TryOpenInRunningVisualStudio(Path, Line, Column))
				{
					bOpenedAny = true;
				}
				else
				{
					Remaining.Add(Path);
				}
			}

			if (Remaining.IsEmpty())
			{
				return bOpenedAny;
			}

			return Inner->OpenSourceFiles(Remaining) || bOpenedAny;
		}

		virtual bool AddSourceFiles(const TArray<FString>& AbsoluteSourcePaths, const TArray<FString>& AvailableModules) override
		{
			return Inner->AddSourceFiles(AbsoluteSourcePaths, AvailableModules);
		}

		virtual bool SaveAllOpenDocuments() const override { return Inner->SaveAllOpenDocuments(); }
		virtual void Tick(const float DeltaTime) override { Inner->Tick(DeltaTime); }
		virtual FName GetOpenIconName() const override { return Inner->GetOpenIconName(); }
		virtual FName GetRefreshIconName() const override { return Inner->GetRefreshIconName(); }
		virtual FName GetStyleSet() const override { return Inner->GetStyleSet(); }

	private:
		ISourceCodeAccessor* Inner = nullptr;
	};

	FRunningVisualStudioSourceAccessor* GRunningVisualStudioRedirect = nullptr;
	ISourceCodeAccessor* GRedirectedVisualStudioAccessor = nullptr;

	bool IsVisualStudioAccessorName(const FString& AccessorName)
	{
		return AccessorName.StartsWith(TEXT("VisualStudio")) && !AccessorName.StartsWith(TEXT("VisualStudioCode"));
	}
}

void FProjectSourceCodeNavigation::InstallRunningVisualStudioRedirect()
{
	if (GRunningVisualStudioRedirect)
	{
		return;
	}

	ISourceCodeAccessModule& SourceCodeAccessModule = FModuleManager::LoadModuleChecked<ISourceCodeAccessModule>(TEXT("SourceCodeAccess"));
	ISourceCodeAccessor& CurrentAccessor = SourceCodeAccessModule.GetAccessor();
	const FString PreviousName = CurrentAccessor.GetFName().ToString();
	if (!IsVisualStudioAccessorName(PreviousName))
	{
		UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Redirect not installed. Current accessor is '%s'."), *PreviousName);
		return;
	}

	GRedirectedVisualStudioAccessor = &CurrentAccessor;
	GRunningVisualStudioRedirect = new FRunningVisualStudioSourceAccessor(CurrentAccessor);
	IModularFeatures::Get().UnregisterModularFeature(SourceCodeAccessorFeatureName, GRedirectedVisualStudioAccessor);
	IModularFeatures::Get().RegisterModularFeature(SourceCodeAccessorFeatureName, GRunningVisualStudioRedirect);
	UE_LOG(LogUnrealSharpEditor, Warning, TEXT("[SourceNav] Redirect installed over '%s'. Active accessor is now '%s'."), *PreviousName, *SourceCodeAccessModule.GetAccessor().GetFName().ToString());
}

void FProjectSourceCodeNavigation::RemoveRunningVisualStudioRedirect()
{
	if (!GRunningVisualStudioRedirect)
	{
		return;
	}

	IModularFeatures::Get().UnregisterModularFeature(SourceCodeAccessorFeatureName, GRunningVisualStudioRedirect);
	if (GRedirectedVisualStudioAccessor)
	{
		IModularFeatures::Get().RegisterModularFeature(SourceCodeAccessorFeatureName, GRedirectedVisualStudioAccessor);
	}

	delete GRunningVisualStudioRedirect;
	GRunningVisualStudioRedirect = nullptr;
	GRedirectedVisualStudioAccessor = nullptr;
}

#undef LOCTEXT_NAMESPACE
