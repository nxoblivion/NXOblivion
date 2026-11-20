using System.Text.Json;
using System.Text.Json.Serialization;

namespace OblivionInstaller;

/// <summary>
/// kit/kit.json, written by ports/switch/installer/build-kit.sh. Everything that depends on the port build (flags,
/// link order, expected game files) lives there so the app itself never changes between port versions.
/// Placeholders in args: {kit} = kit folder, {work} = build folder, {gen} = generated sources folder.
/// </summary>
public sealed class Kit
{
    public string Version { get; set; } = "";
    public string Title { get; set; } = "Oblivion";
    public string Author { get; set; } = "ReXGlue port";
    public string XexPath { get; set; } = "default.xex";
    public string XexSha256 { get; set; } = "";
    public string GameName { get; set; } = "";
    public List<KitFile> Files { get; set; } = new();
    public string Manifest { get; set; } = "codegen/oblivion_manifest.toml";
    public string Codegen { get; set; } = "codegen/rexglue.exe";
    public string Clang { get; set; } = "toolchain/bin/clang.exe";
    public string Lld { get; set; } = "toolchain/bin/ld.lld.exe";
    public List<string> CompileArgs { get; set; } = new();
    public List<PchPatch> PchPatches { get; set; } = new();
    public string PchAppend { get; set; } = "pch-append.h";
    public string PchName { get; set; } = "oblivion_pch.h";
    /// <summary>Link args; "{objects}" expands to the compiled game objects.</summary>
    public List<string> LinkArgs { get; set; } = new();
    public int ExpectedSources { get; set; }
    /// <summary>Where the NRO goes on the SD card.</summary>
    public string NroPath { get; set; } = "switch/oblivion/oblivion.nro";
    /// <summary>The reNut build's shaders, made from the player's files (null: the build has none to make).</summary>
    public ShaderStage? Shaders { get; set; }

    [JsonIgnore] public string Dir { get; private set; } = "";

    static readonly JsonSerializerOptions JsonOpts = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        ReadCommentHandling = JsonCommentHandling.Skip,
    };

    public static Kit Load(string dir)
    {
        var file = Path.Combine(dir, "kit.json");
        if (!File.Exists(file))
            throw new FileNotFoundException(
                $"The port kit is missing ({file}). Keep the 'kit' folder next to the installer exe.", file);
        var kit = JsonSerializer.Deserialize<Kit>(File.ReadAllText(file), JsonOpts)
                  ?? throw new InvalidDataException("kit.json is empty");
        kit.Dir = Path.GetFullPath(dir);
        return kit;
    }

    public string PathOf(string rel) => Path.Combine(Dir, rel.Replace('/', Path.DirectorySeparatorChar));

    public long TotalGameBytes => Files.Sum(f => f.Size);
}

public sealed class KitFile
{
    public string Path { get; set; } = "";
    public long Size { get; set; }
}

public sealed class PchPatch
{
    public string Find { get; set; } = "";
    public string Replace { get; set; } = "";
}

/// <summary>
/// The reNut build's shader stage (nx/tools/build_kit.sh): the game's shader containers are written to DumpDir
/// (ShaderDump), then the steps run in order in the build folder (XenosRecomp, the GLSL chain, the shader table, the
/// DKSH cache). Sources are generated C++ compiled with the game code (CompileArgs + SourceArgs); Outputs are files of
/// the build folder installed next to the NRO. Placeholders as in kit.json's args, plus {jobs}.
/// </summary>
public sealed class ShaderStage
{
    public string GameShaderDir { get; set; } = "Data/Shaders";
    public string DumpDir { get; set; } = "shaders/dump";
    public List<KitStep> Steps { get; set; } = new();
    public List<string> Sources { get; set; } = new();
    public List<string> SourceArgs { get; set; } = new();
    public List<KitOutput> Outputs { get; set; } = new();
}

public sealed class KitStep
{
    public string Label { get; set; } = "";
    public string Exe { get; set; } = "";
    public List<string> Args { get; set; } = new();
    public Dictionary<string, string> Env { get; set; } = new();
}

public sealed class KitOutput
{
    /// <summary>Path in the build folder.</summary>
    public string From { get; set; } = "";
    /// <summary>Name next to the NRO on the SD card.</summary>
    public string To { get; set; } = "";
}
