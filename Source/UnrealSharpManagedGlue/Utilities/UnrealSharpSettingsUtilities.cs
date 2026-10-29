using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace UnrealSharpManagedGlue.Utilities;

public static class UnrealSharpSettingsUtilities
{
    private static Dictionary<string, JsonElement>? _config;

    public static string PluginConfigPath { get; private set; } = string.Empty;
    public static string ProjectConfigPath { get; private set; } = string.Empty;
    
    public static void InitializeConfigFile(string projectRoot, string unrealSharpRoot)
    {
        PluginConfigPath = GetConfigFile(unrealSharpRoot);
        ProjectConfigPath = GetConfigFile(projectRoot);

        if (_config != null)
        {
            return;
        }
        
        string pluginConfigPath = PluginConfigPath;
        string projectConfigPath = ProjectConfigPath;

        _config = LoadJsonAsDictionary(pluginConfigPath);
        Dictionary<string, JsonElement> projectDict = File.Exists(projectConfigPath) ? LoadJsonAsDictionary(projectConfigPath) : new Dictionary<string, JsonElement>();
        
        foreach (KeyValuePair<string, JsonElement> kvp in projectDict)
        {
            _config[kvp.Key] = kvp.Value;
        }
    }

    public static JsonElement GetElement(string elementName)
    {
        if (_config == null)
        {
            throw new Exception("Run InitializeConfigFile first.");
        }
        
        return _config[elementName];
    }

    public static bool TryGetStringArray(string elementName, out List<string> values)
    {
        values = [];

        if (_config == null)
        {
            throw new Exception("Run InitializeConfigFile first.");
        }

        if (!_config.TryGetValue(elementName, out JsonElement element))
        {
            return false;
        }

        if (element.ValueKind != JsonValueKind.Array)
        {
            ConsoleUtilities.Log($"Warning: Config key '{elementName}' is not a JSON array and was ignored.");
            return false;
        }

        foreach (JsonElement item in element.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.String)
            {
                continue;
            }

            string? text = item.GetString();
            if (!string.IsNullOrWhiteSpace(text))
            {
                values.Add(text);
            }
        }

        return true;
    }
    
    static string GetConfigFile(string rootDirectory)
    {
        string configDirectory = Path.Combine(rootDirectory, "Config");
        if (!Directory.Exists(configDirectory))
        {
            return string.Empty;
        }
        
        EnumerationOptions enumerationOptions = new EnumerationOptions
        {
            RecurseSubdirectories = true
        };

        string[] foundConfigs = Directory.GetFiles(configDirectory, "UnrealSharp.Settings.json", enumerationOptions);

        if (foundConfigs.Length > 1)
        {
            throw new Exception("Found multiple config files");
        }

        if (foundConfigs.Length == 0)
        {
            return string.Empty;
        }

        return foundConfigs[0];
    }

    static Dictionary<string, JsonElement> LoadJsonAsDictionary(string path)
    {
        string json = File.ReadAllText(path);
        return JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(json) ?? new Dictionary<string, JsonElement>();
    }
}
