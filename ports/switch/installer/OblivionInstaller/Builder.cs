using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;

namespace OblivionInstaller;

public sealed record BuildProgress(string Stage, double Fraction, string? Detail = null);

/// <summary>
/// Turns the user's default.xex into oblivion.nro on this PC: ReXGlue codegen -> clang (aarch64) -> lld -> NRO.
/// Only the recompiled game code is built here; everything else comes precompiled in the kit.
/// </summary>
public sealed class Builder
{
    readonly Kit _kit;
    readonly Action<string> _log;

    public Builder(Kit kit, Action<string> log)
    {
        _kit = kit;
        _log = log;
    }

    public static string Sha256Hex(Stream s) => Convert.ToHexStringLower(SHA256.HashData(s));

    /// <summary>Checks the copy against the kit. Returns problems (empty = OK); the xex hash is a hard requirement.</summary>
    public List<string> Verify(GameSource src, out GameFile? xex)
    {
        var problems = new List<string>();
        xex = src.Find(_kit.XexPath);
        if (xex == null)
        {
            problems.Add($"{_kit.XexPath} was not found. Pick the disc image or the folder that holds {_kit.XexPath}.");
            return problems;
        }
        using (var s = src.Open(xex))
        {
            var hash = Sha256Hex(s);
            if (!string.Equals(hash, _kit.XexSha256, StringComparison.OrdinalIgnoreCase))
                problems.Add($"{_kit.XexPath} is a different version (SHA-256 {hash[..16]}...). This port needs {_kit.GameName}.");
        }
        int missing = 0;
        foreach (var f in _kit.Files)
        {
            var have = src.Find(f.Path);
            if (have == null || have.Size != f.Size)
            {
                if (missing++ < 5)
                    problems.Add(have == null ? $"Missing game file: {f.Path}" : $"Different size: {f.Path}");
            }
        }
        if (missing > 5) problems.Add($"... and {missing - 5} more missing or different game files.");
        return problems;
    }

    public string WorkDir => Path.Combine(AppPaths.WorkRoot, $"build-{_kit.Version}-{_kit.XexSha256[..12]}");
    public string NroPath => Path.Combine(WorkDir, "oblivion.nro");
    public bool IsBuilt => File.Exists(NroPath) && File.Exists(NroPath + ".ok") &&
                           (_kit.Shaders?.Outputs.All(o => File.Exists(OutputPath(o))) ?? true);

    /// <summary>A shader stage output's file in the build folder.</summary>
    public string OutputPath(KitOutput o) => Path.Combine(WorkDir, o.From.Replace('/', Path.DirectorySeparatorChar));

    public async Task BuildAsync(GameSource src, GameFile xex, IProgress<BuildProgress> progress, CancellationToken ct)
    {
        var sw = Stopwatch.StartNew();
        var work = WorkDir;
        Directory.CreateDirectory(work);
        File.Delete(NroPath + ".ok");

        // 1. default.xex from the user's copy.
        progress.Report(new("Reading default.xex", 0));
        var xexOut = Path.Combine(work, "assets", "default.xex");
        Directory.CreateDirectory(Path.GetDirectoryName(xexOut)!);
        using (var s = src.Open(xex))
        using (var d = File.Create(xexOut))
            await s.CopyToAsync(d, ct);

        // 2. Codegen (PowerPC -> C++).
        progress.Report(new("Translating the game code (ReXGlue codegen)", 0.02));
        var gen = Path.Combine(work, "generated", "default");
        if (Directory.Exists(gen)) Directory.Delete(gen, true);
        File.Copy(_kit.PathOf(_kit.Manifest), Path.Combine(work, "oblivion_manifest.toml"), true);
        await RunAsync(_kit.PathOf(_kit.Codegen), new[] { "codegen", "oblivion_manifest.toml" }, work, ct, quiet: true);
        var sources = Directory.GetFiles(gen, "*.cpp").OrderBy(f => f, StringComparer.Ordinal).ToList();
        if (sources.Count == 0) throw new InvalidOperationException("codegen produced no sources (see the log)");
        if (_kit.ExpectedSources > 0 && sources.Count != _kit.ExpectedSources)
            _log($"note: codegen wrote {sources.Count} sources (kit was made with {_kit.ExpectedSources})");

        // 3. Switch patches to the generated header.
        var pch = Path.Combine(gen, _kit.PchName);
        var text = File.ReadAllText(pch);
        foreach (var p in _kit.PchPatches)
        {
            if (!text.Contains(p.Find)) throw new InvalidOperationException($"pch patch did not apply: {p.Find[..Math.Min(60, p.Find.Length)]}");
            text = text.Replace(p.Find, p.Replace);
        }
        text += "\n" + File.ReadAllText(_kit.PathOf(_kit.PchAppend));
        File.WriteAllText(pch, text);

        // 3b. The reNut build's shaders, from the player's files.
        var extra = new List<string>();
        if (_kit.Shaders is { } sh)
        {
            // The stage depends on the kit and the game copy alone (both name the build folder): a finished one is kept
            // when the game code is built again (delete the build folder to make the shaders again).
            var shaders = Path.Combine(work, "shaders");
            var stamp = Path.Combine(shaders, "stage.ok");
            extra = Expand(sh.Sources, work, gen);
            if (File.Exists(stamp) && File.ReadAllText(stamp) == _kit.Version &&
                sh.Outputs.All(o => File.Exists(OutputPath(o))) && extra.All(File.Exists))
                _log("the game's shaders are already translated for this kit (kept)");
            else
            {
                progress.Report(new("Reading the game's shaders", 0.03));
                if (Directory.Exists(shaders)) Directory.Delete(shaders, true);
                ShaderDump.Run(src, sh.GameShaderDir, Path.Combine(work, sh.DumpDir.Replace('/', Path.DirectorySeparatorChar)), _log);
                for (int i = 0; i < sh.Steps.Count; i++)
                {
                    var step = sh.Steps[i];
                    progress.Report(new("Translating the game's shaders", 0.035 + 0.045 * i / sh.Steps.Count, step.Label));
                    var env = step.Env.ToDictionary(kv => kv.Key, kv => Expand(new[] { kv.Value }, work, gen)[0]);
                    await RunAsync(Expand(new[] { step.Exe }, work, gen)[0], Expand(step.Args, work, gen), work, ct, env: env);
                }
                foreach (var o in sh.Outputs)
                    if (!File.Exists(OutputPath(o))) throw new InvalidOperationException($"the shader stage did not make {o.From}");
                File.WriteAllText(stamp, _kit.Version);
            }
        }

        // 4. Compile (the slow part), largest files first so the tail is short.
        var obj = Path.Combine(work, "obj");
        if (Directory.Exists(obj)) Directory.Delete(obj, true);
        Directory.CreateDirectory(obj);
        sources = sources.Concat(extra).OrderByDescending(f => new FileInfo(f).Length).ToList();
        int jobs = CompileJobs();
        _log($"compiling {sources.Count} files with {jobs} parallel jobs");
        int done = 0;
        var objects = new List<string>();
        var compileArgs = Expand(_kit.CompileArgs, work, gen);
        var extraArgs = compileArgs.Concat(Expand(_kit.Shaders?.SourceArgs ?? new(), work, gen)).ToList();
        await Parallel.ForEachAsync(sources, new ParallelOptions { MaxDegreeOfParallelism = jobs, CancellationToken = ct },
            async (cpp, token) =>
            {
                var o = Path.Combine(obj, Path.GetFileNameWithoutExtension(cpp) + ".o");
                var args = new List<string>(extra.Contains(cpp) ? extraArgs : compileArgs) { "-c", cpp, "-o", o };
                await RunAsync(_kit.PathOf(_kit.Clang), args, gen, token, quiet: true);
                lock (objects) objects.Add(o);
                int n = Interlocked.Increment(ref done);
                progress.Report(new("Compiling the game code for Switch", 0.05 + 0.85 * n / sources.Count,
                    $"{n} / {sources.Count}"));
            });

        // 5. Link against the precompiled port kit.
        progress.Report(new("Linking", 0.92));
        var elf = Path.Combine(work, "oblivion.elf");
        var linkArgs = new List<string>();
        foreach (var a in Expand(_kit.LinkArgs, work, gen))
        {
            if (a == "{objects}") linkArgs.AddRange(objects.OrderBy(o => o, StringComparer.Ordinal));
            else linkArgs.Add(a);
        }
        linkArgs.AddRange(new[] { "-o", elf });
        var rsp = Path.Combine(work, "link.rsp");
        File.WriteAllLines(rsp, linkArgs.Select(QuoteRsp));
        await RunAsync(_kit.PathOf(_kit.Lld), new[] { "@" + rsp }, work, ct);

        // 6. NRO with our own icon and NACP.
        progress.Report(new("Packaging oblivion.nro", 0.98));
        var nacp = Nro.CreateNacp(_kit.Title, _kit.Author, _kit.Version);
        var icon = Nro.CreateIcon(_kit.Title, "ReXGlue port");
        Nro.ElfToNro(elf, NroPath, icon, nacp);
        File.Delete(elf); // 240 MB, not needed after packaging
        File.WriteAllText(NroPath + ".ok", $"{_kit.Version} {DateTime.Now:s} {sw.Elapsed.TotalSeconds:F0}s");
        _log($"built {NroPath} ({new FileInfo(NroPath).Length / 1048576.0:F1} MB) in {sw.Elapsed:mm\\:ss}");
        progress.Report(new("Built", 1));
    }

    List<string> Expand(IEnumerable<string> args, string work, string gen) =>
        args.Select(a => a.Replace("{kit}", _kit.Dir.Replace('\\', '/'))
            .Replace("{work}", work.Replace('\\', '/'))
            .Replace("{gen}", gen.Replace('\\', '/'))
            .Replace("{jobs}", CompileJobs().ToString())).ToList();

    static string QuoteRsp(string a) => a.Contains(' ') || a.Contains('"') ? "\"" + a.Replace("\"", "\\\"") + "\"" : a;

    /// <summary>One clang per core, but leave about 1.5 GB of RAM per job (the largest files need ~1 GB).</summary>
    static int CompileJobs()
    {
        long totalMb = GC.GetGCMemoryInfo().TotalAvailableMemoryBytes / 1048576;
        int byMem = (int)Math.Max(1, (totalMb - 2048) / 1536);
        return Math.Clamp(Math.Min(Environment.ProcessorCount, byMem), 1, 32);
    }

    async Task RunAsync(string exe, IEnumerable<string> args, string cwd, CancellationToken ct, bool quiet = false,
        Dictionary<string, string>? env = null)
    {
        var psi = new ProcessStartInfo(exe)
        {
            WorkingDirectory = cwd,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        foreach (var kv in env ?? new()) psi.Environment[kv.Key] = kv.Value;
        using var p = Process.Start(psi) ?? throw new InvalidOperationException($"could not start {exe}");
        using (ct.Register(() => { try { p.Kill(true); } catch { } }))
        {
            var output = new StringBuilder();
            var so = p.StandardOutput.ReadToEndAsync(ct);
            var se = p.StandardError.ReadToEndAsync(ct);
            await p.WaitForExitAsync(ct);
            output.Append(await so).Append(await se);
            var text = output.ToString().Trim();
            if (p.ExitCode != 0)
            {
                _log(text.Length > 4000 ? text[..4000] + "\n..." : text);
                throw new InvalidOperationException($"{Path.GetFileName(exe)} failed (exit code {p.ExitCode})");
            }
            if (!quiet && text.Length > 0) _log(text);
            else if (quiet && text.Length > 0)
            {
                var last = text.Split('\n').Last().Trim();
                if (last.Length > 0 && Path.GetFileName(exe).StartsWith("rexglue")) _log(last);
            }
        }
    }
}

public static class AppPaths
{
    public static string ExeDir => AppContext.BaseDirectory;

    /// <summary>Build files go next to the exe ("work"), or to %LOCALAPPDATA% when that folder is read-only.</summary>
    public static string WorkRoot
    {
        get
        {
            var local = Path.Combine(ExeDir, "work");
            try
            {
                Directory.CreateDirectory(local);
                var probe = Path.Combine(local, ".write-test");
                File.WriteAllText(probe, "");
                File.Delete(probe);
                return local;
            }
            catch
            {
                return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OblivionNX", "work");
            }
        }
    }
}
