using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using EpicGames.UHT.Types;
using UnrealSharp.Automation.Utilities;

namespace UnrealSharpManagedGlue.Utilities;

public static class GlueExclusion
{
	public const string StatusGenerated = "Generated";
	public const string StatusExcludedByConfig = "ExcludedByConfig";
	public const string StatusExcludedByDefine = "ExcludedByDefine";
	public const string StatusExclusionIgnored = "ExclusionIgnored";
	public const string EngineGroupName = "Engine";
	public const string ProjectGroupName = "Project";

	const string ExcludedModulesKey = "GlueExcludedModules";
	const string ExcludedPluginsKey = "GlueExcludedPlugins";

	static readonly Dictionary<string, GlueModuleDecision> Decisions = new(StringComparer.OrdinalIgnoreCase);
	static bool _resolved;

	public static void Resolve()
	{
		if (_resolved)
		{
			return;
		}

		Decisions.Clear();
		DiscoverCandidates();
		ApplyConfig();
		KeepReferencedExclusions();
		List<string> logLines = [];
		List<GlueModuleManifestEntry> manifestEntries = [];
		int generatedCount = 0;
		int excludedCount = 0;
		int ignoredCount = 0;
		int defineSkipCount = 0;

		foreach (GlueModuleDecision decision in Decisions.Values)
		{
			switch (decision.Status)
			{
				case StatusExcludedByDefine:
					defineSkipCount++;
					break;
				case StatusExclusionIgnored:
					ignoredCount++;
					logLines.Add($"Warning: Glue exclusion of module '{decision.Name}' was ignored because generated module(s) depend on it: {string.Join(", ", decision.ForcedBy)}.");
					break;
				case StatusExcludedByConfig:
					excludedCount++;
					string owner = decision.HasPlugin ? $"plugin {decision.PluginName}" : decision.GroupName;
					logLines.Add($"Excluding module '{decision.Name}' ({owner}) from C# glue generation.");
					break;
				default:
					generatedCount++;
					break;
			}

			if (decision.LockedWarning != null)
			{
				logLines.Add(decision.LockedWarning);
			}

			manifestEntries.Add(decision.ToManifestEntry());
		}

		logLines.Sort(StringComparer.Ordinal);
		foreach (string line in logLines)
		{
			ConsoleUtilities.Log(line);
		}

		ConsoleUtilities.Log($"C# glue modules: {generatedCount} generated, {excludedCount} excluded by config, {ignoredCount} exclusion(s) ignored because of dependencies, {defineSkipCount} skipped by SkipGlueGeneration.");

		manifestEntries.Sort(static (left, right) =>
		{
			int groupOrder = string.Compare(left.Plugin, right.Plugin, StringComparison.OrdinalIgnoreCase);
			return groupOrder != 0 ? groupOrder : string.Compare(left.Name, right.Name, StringComparison.OrdinalIgnoreCase);
		});
		WriteManifest(manifestEntries);
		_resolved = true;
	}

	public static bool ShouldExport(UhtPackage package)
	{
		if (!_resolved)
		{
			throw new InvalidOperationException("Glue exclusion has not been resolved.");
		}

		if (!Decisions.TryGetValue(package.GetModuleShortName(), out GlueModuleDecision? decision))
		{
			return true;
		}

		return decision.Status is StatusGenerated or StatusExclusionIgnored;
	}

	public static void CleanExcludedOutputs()
	{
		if (!_resolved)
		{
			return;
		}

		List<GlueModuleDecision> excluded = [];
		foreach (GlueModuleDecision decision in Decisions.Values)
		{
			if (decision.Status == StatusExcludedByConfig)
			{
				excluded.Add(decision);
			}
		}

		excluded.Sort(static (left, right) => string.Compare(left.Name, right.Name, StringComparison.OrdinalIgnoreCase));

		foreach (GlueModuleDecision decision in excluded)
		{
			bool deleteSucceeded = true;
			foreach (string directory in GetCleanupDirectories(decision))
			{
				if (!TryDeleteGlueDirectory(decision, directory))
				{
					deleteSucceeded = false;
				}
			}

			if (deleteSucceeded)
			{
				PackageHeadersTracker.RemoveModule(decision.Name);
			}
		}
	}

	static void DiscoverCandidates()
	{
		Queue<UhtPackage> pending = new();
		HashSet<UhtPackage> queued = [];

		void Enqueue(UhtPackage package)
		{
			if (queued.Add(package))
			{
				pending.Enqueue(package);
			}
		}

		foreach (UhtModule module in GeneratorStatics.Factory.Session.Modules)
		{
			foreach (UhtPackage package in module.Packages)
			{
				Enqueue(package);
			}
		}

		while (pending.Count > 0)
		{
			UhtPackage package = pending.Dequeue();
			string moduleName = package.GetModuleShortName();
			if (!Decisions.TryGetValue(moduleName, out GlueModuleDecision? decision))
			{
				decision = GlueModuleDecision.Create(package);
				Decisions.Add(decision.Name, decision);
			}

			if (package.IsDefineActive(PackageUtilities.SkipGlueGenerationDefine))
			{
				decision.HardSkip = true;
				continue;
			}

			if (decision.HardSkip)
			{
				continue;
			}

			foreach (UhtPackage reference in ModuleUtilities.CollectReferencedPackages(package))
			{
				string referencedName = reference.GetModuleShortName();
				if (!referencedName.Equals(moduleName, StringComparison.OrdinalIgnoreCase))
				{
					decision.ReferencedModules.Add(referencedName);
				}

				Enqueue(reference);
			}
		}
	}

	static void ApplyConfig()
	{
		UnrealSharpSettingsUtilities.TryGetStringArray(ExcludedModulesKey, out List<string> excludedModuleNames);
		UnrealSharpSettingsUtilities.TryGetStringArray(ExcludedPluginsKey, out List<string> excludedPluginNames);
		HashSet<string> excludedModules = new(excludedModuleNames, StringComparer.OrdinalIgnoreCase);
		HashSet<string> excludedPlugins = new(excludedPluginNames, StringComparer.OrdinalIgnoreCase);

		foreach (GlueModuleDecision decision in Decisions.Values)
		{
			bool listed = excludedModules.Contains(decision.Name) || (decision.HasPlugin && excludedPlugins.Contains(decision.PluginName));
			if (decision.Locked && listed && !decision.HardSkip)
			{
				decision.LockedWarning = $"Warning: Module '{decision.Name}' always generates C# glue and was not excluded.";
			}

			decision.ConfigExcluded = !decision.Locked && !decision.HardSkip && listed;
		}
	}

	static void KeepReferencedExclusions()
	{
		HashSet<string> generated = new(StringComparer.OrdinalIgnoreCase);
		Queue<string> keepQueue = new();
		Dictionary<string, HashSet<string>> forcedBy = new(StringComparer.OrdinalIgnoreCase);

		foreach (GlueModuleDecision decision in Decisions.Values)
		{
			decision.Status = decision.HardSkip ? StatusExcludedByDefine : decision.ConfigExcluded ? StatusExcludedByConfig : StatusGenerated;
			if (decision.Status != StatusGenerated)
			{
				continue;
			}

			if (generated.Add(decision.Name))
			{
				keepQueue.Enqueue(decision.Name);
			}
		}

		while (keepQueue.Count > 0)
		{
			string name = keepQueue.Dequeue();
			GlueModuleDecision decision = Decisions[name];

			foreach (string referencedName in decision.ReferencedModules)
			{
				if (!Decisions.TryGetValue(referencedName, out GlueModuleDecision? referenced) || referenced.HardSkip || !referenced.ConfigExcluded)
				{
					continue;
				}

				if (!forcedBy.TryGetValue(referenced.Name, out HashSet<string>? forcers))
				{
					forcers = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
					forcedBy.Add(referenced.Name, forcers);
				}

				forcers.Add(decision.Name);
				if (generated.Add(referenced.Name))
				{
					keepQueue.Enqueue(referenced.Name);
				}
			}
		}

		foreach (GlueModuleDecision decision in Decisions.Values)
		{
			if (!decision.ConfigExcluded || !generated.Contains(decision.Name))
			{
				continue;
			}

			decision.Status = StatusExclusionIgnored;
			if (!forcedBy.TryGetValue(decision.Name, out HashSet<string>? forcers))
			{
				continue;
			}

			decision.ForcedBy.AddRange(forcers);
			decision.ForcedBy.Sort(StringComparer.OrdinalIgnoreCase);
		}
	}

	static void WriteManifest(List<GlueModuleManifestEntry> entries)
	{
		string? projectDirectory = GeneratorStatics.Factory.Session.ProjectDirectory;
		if (string.IsNullOrEmpty(projectDirectory))
		{
			return;
		}

		try
		{
			string manifestPath = Path.Combine(projectDirectory, "Intermediate", "UnrealSharp", "GlueModules.json");
			Directory.CreateDirectory(Path.GetDirectoryName(manifestPath)!);
			GlueModuleManifest manifest = new()
			{
				Modules = entries
			};
			string json = JsonSerializer.Serialize(manifest, new JsonSerializerOptions
			{
				WriteIndented = true
			});
			File.WriteAllText(manifestPath, json);
			ConsoleUtilities.Log($"Wrote C# glue module manifest to {manifestPath}");
		}
		catch (Exception exception)
		{
			ConsoleUtilities.Log($"Warning: Failed to write C# glue module manifest: {exception.Message}");
		}
	}

	static List<string> GetCleanupDirectories(GlueModuleDecision decision)
	{
		List<string> directories = [];
		if (decision.IsEngine)
		{
			// Engine glue is written under the UnrealSharp plugin unless EmitsToProjectDirectory redirected it.
			// Module names are unique, so removing both candidate folders cannot collide with another module.
			AddCleanupDirectory(directories, GeneratorStatics.PluginDirectory, decision.Name);
			AddCleanupDirectory(directories, GeneratorStatics.Factory.Session.ProjectDirectory!, decision.Name);
			return directories;
		}

		try
		{
			AddCleanupDirectory(directories, decision.Package.GetBaseDirectoryForPackage(), decision.Name);
		}
		catch (Exception exception)
		{
			ConsoleUtilities.Log($"Warning: Could not resolve glue output for excluded module '{decision.Name}': {exception.Message}");
		}

		return directories;
	}

	static void AddCleanupDirectory(List<string> directories, string root, string moduleName)
	{
		string directory = Path.GetFullPath(PathUtilities.GetUhtGeneratedModuleOutputPath(root, GeneratorStatics.TargetType, moduleName));
		if (!directory.EndsWith(moduleName, StringComparison.OrdinalIgnoreCase) ||
		    directory.IndexOf(Path.Combine("Intermediate", "UnrealSharp"), StringComparison.OrdinalIgnoreCase) < 0)
		{
			ConsoleUtilities.Log($"Warning: Refusing to delete unexpected glue path '{directory}'.");
			return;
		}

		directories.Add(directory);
	}

	static bool TryDeleteGlueDirectory(GlueModuleDecision decision, string directory)
	{
		if (!Directory.Exists(directory))
		{
			return true;
		}

		try
		{
			// The module csproj lives inside this directory for non-engine modules.
			Directory.Delete(directory, true);
		}
		catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
		{
			ConsoleUtilities.Log($"Warning: Failed to delete glue output for excluded module '{decision.Name}' at '{directory}': {exception.Message}");
			return false;
		}

		ConsoleUtilities.Log($"Removed stale C# glue for excluded module '{decision.Name}' at '{directory}'.");
		if (decision.IsEngine)
		{
			FileExporter.MarkEngineGlueModified();
		}

		return true;
	}

	sealed class GlueModuleDecision
	{
		public string Name = string.Empty;
		public UhtPackage Package = null!;
		public string PluginName = string.Empty;
		public bool HasPlugin;
		public bool IsEngine;
		public bool EditorOnly;
		public bool Locked;
		public bool HardSkip;
		public bool ConfigExcluded;
		public string Status = StatusGenerated;
		public string? LockedWarning;
		public readonly HashSet<string> ReferencedModules = new(StringComparer.OrdinalIgnoreCase);
		public readonly List<string> ForcedBy = [];

		public string GroupName => HasPlugin ? PluginName : IsEngine ? EngineGroupName : ProjectGroupName;

		public static GlueModuleDecision Create(UhtPackage package)
		{
			UhtPackage scriptPackage = package.Module.ScriptPackage;
			GlueModuleDecision decision = new()
			{
				Name = scriptPackage.GetModuleShortName(),
				Package = scriptPackage,
				IsEngine = scriptPackage.IsPartOfEngine(),
				EditorOnly = scriptPackage.IsEditorOnly()
			};

			if (scriptPackage.TryGetOwningPluginName(out string pluginName, out string pluginRoot))
			{
				decision.HasPlugin = true;
				decision.PluginName = pluginName;
				decision.Locked = IsUnrealSharpPlugin(pluginRoot);
			}

			if (decision.Name.Equals("CoreUObject", StringComparison.OrdinalIgnoreCase) ||
			    decision.Name.Equals("Engine", StringComparison.OrdinalIgnoreCase) ||
			    decision.Name.Equals(GeneratorStatics.PluginModule.Name, StringComparison.OrdinalIgnoreCase))
			{
				decision.Locked = true;
			}

			return decision;
		}

		public GlueModuleManifestEntry ToManifestEntry()
		{
			return new GlueModuleManifestEntry
			{
				Name = Name,
				Plugin = GroupName,
				IsPlugin = HasPlugin,
				IsEngine = IsEngine,
				EditorOnly = EditorOnly,
				Locked = Locked,
				Status = Status,
				ForcedBy = ForcedBy
			};
		}

		static bool IsUnrealSharpPlugin(string pluginRoot)
		{
			if (string.IsNullOrEmpty(GeneratorStatics.PluginDirectory))
			{
				return false;
			}

			string left = Path.TrimEndingDirectorySeparator(Path.GetFullPath(pluginRoot));
			string right = Path.TrimEndingDirectorySeparator(Path.GetFullPath(GeneratorStatics.PluginDirectory));
			return string.Equals(left, right, StringComparison.OrdinalIgnoreCase);
		}
	}
}

public sealed class GlueModuleManifest
{
	[JsonPropertyName("modules")]
	public List<GlueModuleManifestEntry> Modules { get; set; } = [];
}

public sealed class GlueModuleManifestEntry
{
	[JsonPropertyName("name")]
	public string Name { get; set; } = string.Empty;

	[JsonPropertyName("plugin")]
	public string Plugin { get; set; } = string.Empty;

	[JsonPropertyName("isPlugin")]
	public bool IsPlugin { get; set; }

	[JsonPropertyName("isEngine")]
	public bool IsEngine { get; set; }

	[JsonPropertyName("editorOnly")]
	public bool EditorOnly { get; set; }

	[JsonPropertyName("locked")]
	public bool Locked { get; set; }

	[JsonPropertyName("status")]
	public string Status { get; set; } = GlueExclusion.StatusGenerated;

	[JsonPropertyName("forcedBy")]
	public List<string> ForcedBy { get; set; } = [];
}
