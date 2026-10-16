namespace UnrealSharp.Core;

public enum ManagedExceptionSite
{
    CreateNewManagedObject,
    CreateNewManagedObjectWrapper,
    LookupManagedMethod,
    LookupManagedType,
    InitializeStruct,
    InvokeManagedMethod,
    InvokeDelegate,
    AsyncFunction,
    ExportedFunctions,
    SubclassOf,
    Other,
}

public static class ManagedExceptionHooks
{
    public static event Action<Exception, ManagedExceptionSite>? ExceptionCaught;

    [ThreadStatic] private static bool _raising;

    public static void Raise(Exception exception, ManagedExceptionSite site)
    {
        if (_raising || exception is null)
        {
            return;
        }
        _raising = true;
        try
        {
            ExceptionCaught?.Invoke(exception, site);
        }
        catch
        {
            // Reporting must not replace the original catch behavior.
        }
        finally
        {
            _raising = false;
        }
    }
}
