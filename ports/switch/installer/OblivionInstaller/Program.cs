using System.Runtime.InteropServices;

namespace OblivionInstaller;

static class Program
{
    [DllImport("kernel32.dll")]
    static extern bool AttachConsole(int pid);

    [STAThread]
    static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--cli")
        {
            AttachConsole(-1);
            return Cli(args.Skip(1).ToArray());
        }
        ApplicationConfiguration.Initialize();
        var form = new MainForm(Arg(args, "--kit") ?? Path.Combine(AppPaths.ExeDir, "kit"));
        if (Arg(args, "--snapshot") is { } png)
        {
            // Layout check without a desktop: render the window (optionally after picking a source) to a PNG.
            form.Shown += async (_, _) =>
            {
                if (Arg(args, "--source") is { } src) await form.SelectSourceForTestAsync(src);
                await Task.Delay(300);
                using var bmp = new Bitmap(form.Width, form.Height);
                form.DrawToBitmap(bmp, new Rectangle(0, 0, form.Width, form.Height));
                bmp.Save(png);
                form.Close();
            };
        }
        Application.Run(form);
        return 0;
    }

    static string? Arg(string[] args, string name)
    {
        int i = Array.IndexOf(args, name);
        return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
    }

    /// <summary>
    /// OblivionNX-Installer.exe --cli --source game.iso [--out SDROOT] [--mtp "Nintendo Switch"] [--no-game] [--kit DIR]
    /// [--dump-shaders DIR]
    /// Builds the NRO (cached) and writes the SD layout to a folder or an MTP device.
    /// </summary>
    static int Cli(string[] args)
    {
        Action<string> log = s => Console.WriteLine(s);
        try
        {
            var kit = Kit.Load(Arg(args, "--kit") ?? Path.Combine(AppPaths.ExeDir, "kit"));
            var srcPath = Arg(args, "--source") ?? throw new ArgumentException("--source <disc image or folder> is required");
            var src = GameSource.Load(srcPath);
            log($"source: {src.Kind}, {src.Files.Count} files");
            if (args.Contains("--list"))
                foreach (var f in src.Files) log($"  {f.Size,12}  {f.Path}");
            if (Arg(args, "--dump-shaders") is { } dumpDir) // test aid: the shader stage's first step alone
                return ShaderDump.Run(src, kit.Shaders?.GameShaderDir ?? "Data/Shaders", dumpDir, log) > 0 ? 0 : 1;
            var builder = new Builder(kit, log);
            var problems = builder.Verify(src, out var xex);
            foreach (var p in problems) log("problem: " + p);
            if (problems.Count > 0 || xex == null) return 2;
            log($"{kit.XexPath} matches {kit.GameName}");
            if (!builder.IsBuilt || args.Contains("--rebuild"))
            {
                int lastPct = -1;
                var prog = new Progress<BuildProgress>(p =>
                {
                    int pct = (int)(p.Fraction * 100);
                    if (pct / 5 != lastPct / 5) { lastPct = pct; log($"[{pct,3}%] {p.Stage} {p.Detail}"); }
                });
                builder.BuildAsync(src, xex, prog, CancellationToken.None).GetAwaiter().GetResult();
            }
            else log($"already built: {builder.NroPath}");

            var outDir = Arg(args, "--out");
            var mtp = Arg(args, "--mtp");
            if (outDir == null && mtp == null) return 0;
            IInstallTarget target;
            if (outDir != null) target = new FolderTarget(outDir);
            else
            {
                var dev = MtpTarget.List().FirstOrDefault(d => d.name.Contains(mtp!, StringComparison.OrdinalIgnoreCase));
                if (dev.dev == null) throw new InvalidOperationException($"no MTP device matching '{mtp}'");
                var storage = dev.storages.FirstOrDefault(s => s.Contains("SD", StringComparison.OrdinalIgnoreCase))
                              ?? throw new InvalidOperationException("the device has no SD card storage");
                target = new MtpTarget(dev.dev, storage);
            }
            using (target)
            {
                int lastPct = -1;
                var prog = new Progress<InstallProgress>(p =>
                {
                    int pct = p.Total > 0 ? (int)(p.Done * 100 / p.Total) : 100;
                    if (pct / 5 != lastPct / 5) { lastPct = pct; log($"[{pct,3}%] {p.File}"); }
                });
                Installer.Run(target, kit, builder.NroPath, src, !args.Contains("--no-game"), log, prog, CancellationToken.None);
            }
            log("done");
            return 0;
        }
        catch (Exception e)
        {
            log("error: " + e.Message);
            return 1;
        }
    }
}
