using MediaDevices;

namespace OblivionInstaller;

public sealed record InstallProgress(string Stage, long Done, long Total, string? File = null);

/// <summary>Where the SD card layout goes. Paths are '/'-separated and relative to the SD card root.</summary>
public interface IInstallTarget : IDisposable
{
    string Describe { get; }
    long? ExistingSize(string path);
    void Write(string path, Stream data, long length, Action<long> onBytes, CancellationToken ct);
}

public static class Installer
{
    public const string TomlPath = "oblivion/oblivion.toml";
    public const string AssetsDir = "oblivion/assets";

    /// <summary>
    /// Writes the NRO and the shader stage's outputs next to it (always), oblivion.toml (only if missing: it holds the
    /// user's settings) and the game files (skipped when the same size is already there, so an interrupted install
    /// resumes).
    /// </summary>
    public static void Run(IInstallTarget target, Kit kit, string nro, GameSource? src, bool includeGame,
        Action<string> log, IProgress<InstallProgress> progress, CancellationToken ct)
    {
        var todo = new List<(string dst, Func<Stream> open, long size)>();
        todo.Add((kit.NroPath, () => File.OpenRead(nro), new FileInfo(nro).Length));
        var nroDir = kit.NroPath.Contains('/') ? kit.NroPath[..kit.NroPath.LastIndexOf('/')] + "/" : "";
        foreach (var o in kit.Shaders?.Outputs ?? new())
        {
            var file = Path.Combine(Path.GetDirectoryName(nro)!, o.From.Replace('/', Path.DirectorySeparatorChar));
            todo.Add((nroDir + o.To, () => File.OpenRead(file), new FileInfo(file).Length));
        }
        var toml = kit.PathOf("payload/oblivion.toml");
        if (File.Exists(toml) && target.ExistingSize(TomlPath) == null)
            todo.Add((TomlPath, () => File.OpenRead(toml), new FileInfo(toml).Length));

        if (includeGame)
        {
            if (src == null) throw new InvalidOperationException("no game copy selected");
            int skipped = 0;
            foreach (var f in kit.Files)
            {
                var have = src.Find(f.Path) ?? throw new InvalidOperationException($"missing game file {f.Path}");
                var dst = AssetsDir + "/" + f.Path; // the kit's spelling, so case matches what the port expects
                if (target.ExistingSize(dst) == f.Size) { skipped++; continue; }
                todo.Add((dst, () => src.Open(have), f.Size));
            }
            if (skipped > 0) log($"{skipped} game files are already on the target with the right size (skipped)");
        }

        long total = todo.Sum(t => t.size), done = 0;
        log($"writing {todo.Count} files ({total / 1048576.0:F0} MB) to {target.Describe}");
        foreach (var (dst, open, size) in todo)
        {
            ct.ThrowIfCancellationRequested();
            progress.Report(new("Copying", done, total, dst));
            using var s = open();
            long before = done;
            target.Write(dst, s, size, n => progress.Report(new("Copying", before + n, total, dst)), ct);
            done = before + size;
        }
        progress.Report(new("Done", total, total));
    }
}

public sealed class FolderTarget : IInstallTarget
{
    readonly string _root;
    public FolderTarget(string root) => _root = root;
    public string Describe => _root;

    string Full(string path) => Path.Combine(_root, path.Replace('/', Path.DirectorySeparatorChar));

    public long? ExistingSize(string path)
    {
        var fi = new FileInfo(Full(path));
        return fi.Exists ? fi.Length : null;
    }

    public void Write(string path, Stream data, long length, Action<long> onBytes, CancellationToken ct)
    {
        var full = Full(path);
        Directory.CreateDirectory(Path.GetDirectoryName(full)!);
        var tmp = full + ".part";
        using (var d = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            Copy(data, d, onBytes, ct);
        File.Move(tmp, full, true);
    }

    internal static void Copy(Stream src, Stream dst, Action<long> onBytes, CancellationToken ct)
    {
        var buf = new byte[4 << 20];
        long n = 0;
        int r;
        while ((r = src.Read(buf, 0, buf.Length)) > 0)
        {
            ct.ThrowIfCancellationRequested();
            dst.Write(buf, 0, r);
            n += r;
            onBytes(n);
        }
    }

    public void Dispose() { }
}

/// <summary>A Switch running DBI in MTP mode ("MTP responder"), seen by Windows as a portable device.</summary>
public sealed class MtpTarget : IInstallTarget
{
    readonly MediaDevice _dev;
    readonly string _storage;
    readonly HashSet<string> _knownDirs = new(StringComparer.OrdinalIgnoreCase);

    public MtpTarget(MediaDevice dev, string storage)
    {
        _dev = dev;
        _storage = storage;
    }

    public string Describe => $"{_dev.FriendlyName} \\{_storage}";

    /// <summary>Portable devices with their storages, e.g. ("Nintendo Switch", ["1: SD Card", ...]).</summary>
    public static List<(MediaDevice dev, string name, List<string> storages)> List()
    {
        var result = new List<(MediaDevice, string, List<string>)>();
        foreach (var d in MediaDevice.GetDevices())
        {
            try
            {
                d.Connect();
                var storages = d.GetDirectories(@"\").Select(s => s.TrimStart('\\')).ToList();
                var name = string.IsNullOrWhiteSpace(d.FriendlyName) ? d.Description : d.FriendlyName;
                result.Add((d, name, storages));
            }
            catch
            {
                // a device that refuses the connection (locked phone, busy) is simply not offered
            }
        }
        return result;
    }

    string Full(string path) => @"\" + _storage + @"\" + path.Replace('/', '\\');

    public long? ExistingSize(string path)
    {
        var full = Full(path);
        try
        {
            if (!_dev.FileExists(full)) return null;
            return (long)_dev.GetFileInfo(full).Length;
        }
        catch
        {
            return null;
        }
    }

    void EnsureDir(string dir)
    {
        if (_knownDirs.Contains(dir)) return;
        if (!_dev.DirectoryExists(dir)) _dev.CreateDirectory(dir);
        _knownDirs.Add(dir);
    }

    public void Write(string path, Stream data, long length, Action<long> onBytes, CancellationToken ct)
    {
        var full = Full(path);
        var parts = path.Split('/');
        var dir = @"\" + _storage;
        for (int i = 0; i < parts.Length - 1; i++)
        {
            dir += @"\" + parts[i];
            EnsureDir(dir);
        }
        if (_dev.FileExists(full)) _dev.DeleteFile(full);
        using var counting = new ProgressStream(data, onBytes, ct);
        _dev.UploadFile(counting, full);
    }

    public void Dispose()
    {
        try { _dev.Disconnect(); } catch { }
    }
}

/// <summary>Reports bytes read (MTP uploads have no progress callback of their own).</summary>
sealed class ProgressStream : Stream
{
    readonly Stream _inner;
    readonly Action<long> _onBytes;
    readonly CancellationToken _ct;
    long _read;

    public ProgressStream(Stream inner, Action<long> onBytes, CancellationToken ct)
    {
        _inner = inner; _onBytes = onBytes; _ct = ct;
    }

    public override int Read(byte[] buffer, int offset, int count)
    {
        _ct.ThrowIfCancellationRequested();
        int n = _inner.Read(buffer, offset, count);
        _read += n;
        _onBytes(_read);
        return n;
    }

    public override bool CanRead => true;
    public override bool CanSeek => _inner.CanSeek;
    public override bool CanWrite => false;
    public override long Length => _inner.Length;
    public override long Position { get => _inner.Position; set => _inner.Position = value; }
    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) => _inner.Seek(offset, origin);
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}
