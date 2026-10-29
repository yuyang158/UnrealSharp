using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using EpicGames.Core;
using EpicGames.UHT.Types;

namespace UnrealSharpManagedGlue.Utilities;

public static class PackageUtilities
{
    public const string SkipGlueGenerationDefine = "SkipGlueGeneration";

    public static string GetModuleShortName(this UhtPackage package)
    {
        return package.Module.ShortName;
    }

    public static bool IsPartOfEngine(this UhtPackage package)
    {
        return package.Module.IsPartOfEngine || package.IsForcedAsEngineGlue();
    }
    
    public static bool IsDefineActive(this UhtPackage package, string define)
    {
        UHTManifest.Module module = package.GetModule();
        return module.TryGetDefine(define, out int value) && value != 0;
    }
    
    public static bool IsEditorOnly(this UhtPackage package)
    {
        return package.PackageFlags.HasAnyFlags(EPackageFlags.EditorOnly | EPackageFlags.UncookedOnly | EPackageFlags.Developer);
    }

    public static bool IsForcedAsEngineGlue(this UhtPackage package)
    {
        bool hasDefine = package.GetModule().TryGetDefine("ForceAsEngineGlue", out int treatedAsEngineGlue);
        return hasDefine && treatedAsEngineGlue != 0;
    }

    public static string GetPackageExtensions(this UhtPackage package)
    {
        package.GetModule().TryGetDefine("ExtendModule", out string? extension);
        return extension ?? string.Empty;
    }
    
    public static List<string> GetAdditionalExtensionFolders(this UhtPackage package)
    {
        package.GetModule().TryGetDefine("AdditionalExtensionFolder", out string? extensionFolders);
        
        if (string.IsNullOrEmpty(extensionFolders))
        {
            return [];
        }
        
        return extensionFolders.Split(';').ToList();
    }

    public static UHTManifest.Module GetModule(this UhtPackage package)
    {
        return package.Module.Module;
    }

    public static bool ShouldExportPackage(this UhtPackage package)
    {
        // SkipGlueGeneration is a hard skip. Config exclusion cannot force the module back on.
        if (package.IsDefineActive(SkipGlueGenerationDefine))
        {
            return false;
        }

        return GlueExclusion.ShouldExport(package);
    }

    public static bool TryGetOwningPluginName(this UhtPackage package, out string pluginName, out string pluginRoot)
    {
        pluginName = string.Empty;
        pluginRoot = string.Empty;

        // UHT treats a module as a plugin only when its directory sits under a Plugins folder.
        if (!package.Module.IsPlugin)
        {
            return false;
        }

        string engineRoot = NormalizeDirectory(GeneratorStatics.EngineDirectory);
        string projectRoot = NormalizeDirectory(GeneratorStatics.Factory.Session.ProjectDirectory!);
        DirectoryInfo? currentDirectory = new DirectoryInfo(package.GetModule().BaseDirectory);

        while (currentDirectory != null)
        {
            FileInfo[] pluginFiles = currentDirectory.GetFiles("*.uplugin");
            if (pluginFiles.Length > 0)
            {
                Array.Sort(pluginFiles, static (left, right) => string.Compare(left.Name, right.Name, StringComparison.OrdinalIgnoreCase));
                pluginName = Path.GetFileNameWithoutExtension(pluginFiles[0].Name);
                pluginRoot = currentDirectory.FullName;
                return true;
            }

            if (currentDirectory.GetFiles("*.uproject").Length > 0)
            {
                return false;
            }

            string currentPath = NormalizeDirectory(currentDirectory.FullName);
            if (string.Equals(currentPath, engineRoot, StringComparison.OrdinalIgnoreCase) ||
                string.Equals(currentPath, projectRoot, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            currentDirectory = currentDirectory.Parent;
        }

        return false;
    }

    private static string NormalizeDirectory(string directory)
    {
        return Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory));
    }

    public static string GetBaseDirectoryForPackage(this UhtPackage package)
    {
        return LocateProjectOrPluginRoot(package.GetModule().BaseDirectory);
    }
    
    public static string LocateProjectOrPluginRoot(string directory)
    {
        DirectoryInfo? currentDirectory = new DirectoryInfo(directory);
        FileInfo? rootFile = null;

        while (currentDirectory != null)
        {
            IEnumerable<FileInfo> foundFiles = currentDirectory.EnumerateFiles("*.*", SearchOption.TopDirectoryOnly);
            
            rootFile = foundFiles.FirstOrDefault(f => IsUPluginFile(f.FullName) || IsUProjectFile(f.FullName));
            if (rootFile != null)
            {
                break;
            }
                
            currentDirectory = currentDirectory.Parent;
        }

        if (rootFile == null)
        {
            throw new InvalidOperationException($"Could not find .uproject or .uplugin file in any parent directory of {directory}");
        }

        return currentDirectory!.FullName;
    }
    
    public static string GetPackageOutputDirectory(this UhtPackage package)
    {
        ModuleInfo moduleInfo = package.GetModuleInfo();
        return moduleInfo.GlueOutputDirectory;
    }
    
    public static string GetUHTBaseDirectory(this UhtPackage package)
    {
        DirectoryInfo glueOutputDirectory = new DirectoryInfo(package.GetPackageOutputDirectory());
        return glueOutputDirectory.Parent!.FullName;
    }
    
    private static bool IsUPluginFile(string filePath)
    {
        return HasExtension(filePath, ".uplugin");
    }
    
    private static bool IsUProjectFile(string filePath)
    {
        return HasExtension(filePath, ".uproject");
    }
    
    private static bool HasExtension(string filePath, string extension)
    {
        string extensionName = Path.GetExtension(filePath);
        return string.Equals(extensionName, extension, StringComparison.OrdinalIgnoreCase);
    }
}
