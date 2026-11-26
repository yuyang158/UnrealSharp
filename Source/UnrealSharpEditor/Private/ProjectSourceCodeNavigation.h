#pragma once

#include "SourceCodeNavigation.h"

class FProjectSourceCodeNavigation : public ISourceCodeNavigationHandler
{
public:
	// Reuse a Visual Studio that already has this project's solution open.
	// The editor's own opener treats the generated .slnx as the running solution, so a
	// UE_Client.Mixed.sln instance is missed and a second Visual Studio is started.
	static bool TryOpenInRunningVisualStudio(const FString& FullPath, int32 LineNumber, int32 ColumnNumber);

	// Blueprint C++ jumps call the selected source accessor directly, which does not see Mixed.sln.
	static void InstallRunningVisualStudioRedirect();
	static void RemoveRunningVisualStudioRedirect();

	static bool OpenClass(const UClass* Class);

	virtual bool CanNavigateToClass(const UClass* Class) override;
	virtual bool NavigateToClass(const UClass* Class) override;
};
