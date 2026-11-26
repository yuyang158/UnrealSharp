#pragma once

#include "SourceCodeNavigation.h"

class FCSSourceCodeNavigation : public ISourceCodeNavigationHandler
{
public:
	// Reuse a Visual Studio that already has this project's solution open.
	// The editor's own opener treats the generated .slnx as the running solution, so a
	// UE_Client.Mixed.sln instance is missed and a second Visual Studio is started.
	static bool TryOpenInRunningVisualStudio(const FString& FullPath, int32 LineNumber, int32 ColumnNumber);

	static bool OpenClass(const UClass* Class);

	virtual bool CanNavigateToClass(const UClass* Class) override;
	virtual bool NavigateToClass(const UClass* Class) override;
};
