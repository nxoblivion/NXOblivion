using System.Text;

namespace OblivionInstaller;

/// <summary>A file inside the user's game copy. Path uses '/' separators, relative to the disc root.</summary>
public sealed record GameFile(string Path, long Size, long Offset);

/// <summary>The user's own game copy: an Xbox 360 disc image (XDVDFS or ISO9660/Joliet) or an extracted folder.</summary>
public abstract class GameSource
{
    public abstract string Kind { get; }
    public abstract IReadOnlyList<GameFile> Files { get; }
    public abstract Stream Open(GameFile file);

    public static GameSource Load(string path)
    {
        if (Directory.Exists(path)) return new FolderSource(path);
        if (!File.Exists(path)) throw new FileNotFoundException("Not found", path);
        return XdvdfsSource.TryOpen(path) ?? (GameSource?)Iso9660Source.TryOpen(path)
            ?? throw new InvalidDataException(
                "This file is not a disc image the installer can read (expected an Xbox 360 ISO: XDVDFS, or ISO9660/Joliet).");
    }

    public GameFile? Find(string path)
    {
        foreach (var f in Files)
            if (string.Equals(f.Path, path, StringComparison.OrdinalIgnoreCase)) return f;
        return null;
    }
}

public sealed class FolderSource : GameSource
{
    readonly string _root;
    readonly List<GameFile> _files = new();

    public FolderSource(string root)
    {
        _root = Path.GetFullPath(root);
        foreach (var f in Directory.EnumerateFiles(_root, "*", SearchOption.AllDirectories))
            _files.Add(new GameFile(Path.GetRelativePath(_root, f).Replace('\\', '/'), new FileInfo(f).Length, 0));
    }

    public override string Kind => "extracted folder";
    public override IReadOnlyList<GameFile> Files => _files;
    public override Stream Open(GameFile file) =>
        new FileStream(Path.Combine(_root, file.Path), FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
}

/// <summary>Files inside one image file, addressed by byte offset.</summary>
public abstract class ImageSource : GameSource
{
    protected readonly string ImagePath;
    protected readonly List<GameFile> FileList = new();

    protected ImageSource(string imagePath) => ImagePath = imagePath;

    public override IReadOnlyList<GameFile> Files => FileList;

    public override Stream Open(GameFile file)
    {
        var fs = new FileStream(ImagePath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        return new SubStream(fs, file.Offset, file.Size);
    }

    protected static byte[] ReadAt(FileStream fs, long offset, int count)
    {
        var buf = new byte[count];
        fs.Position = offset;
        fs.ReadExactly(buf);
        return buf;
    }
}

/// <summary>Xbox disc filesystem (XGD2/XGD3 game partition, or a trimmed "xiso").</summary>
public sealed class XdvdfsSource : ImageSource
{
    const long Sector = 2048;
    static readonly byte[] Magic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");
    // Game partition offsets: plain xiso, XGD2 (most 360 games), XGD3, XGD1 (original Xbox).
    static readonly long[] PartitionOffsets = { 0, 0xFD90000, 0x2080000, 0x18300000 };

    readonly long _partition;

    XdvdfsSource(string path, long partition) : base(path) => _partition = partition;

    public override string Kind => _partition == 0 ? "Xbox disc image (XISO)" : "Xbox 360 disc image (XDVDFS)";

    public static XdvdfsSource? TryOpen(string path)
    {
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        foreach (var part in PartitionOffsets)
        {
            if (part + 33 * Sector > fs.Length) continue;
            var vd = ReadAt(fs, part + 32 * Sector, (int)Sector);
            if (!vd.AsSpan(0, 20).SequenceEqual(Magic)) continue;
            var src = new XdvdfsSource(path, part);
            uint rootSector = BitConverter.ToUInt32(vd, 20);
            uint rootSize = BitConverter.ToUInt32(vd, 24);
            src.ReadDirectory(fs, rootSector, rootSize, "");
            return src;
        }
        return null;
    }

    void ReadDirectory(FileStream fs, uint sector, uint size, string prefix)
    {
        if (size == 0) return;
        var table = ReadAt(fs, _partition + sector * Sector, (int)size);
        // Entries form a binary tree; offsets are in 4-byte units from the start of the table.
        var pending = new Stack<int>();
        var seen = new HashSet<int>();
        pending.Push(0);
        while (pending.Count > 0)
        {
            int o = pending.Pop();
            if (o + 14 > table.Length || !seen.Add(o)) continue;
            ushort left = BitConverter.ToUInt16(table, o), right = BitConverter.ToUInt16(table, o + 2);
            if (left == 0xFFFF) continue; // sector padding
            uint start = BitConverter.ToUInt32(table, o + 4), length = BitConverter.ToUInt32(table, o + 8);
            byte attr = table[o + 12], nameLen = table[o + 13];
            string name = Encoding.ASCII.GetString(table, o + 14, nameLen);
            if (left != 0) pending.Push(left * 4);
            if (right != 0) pending.Push(right * 4);
            string full = prefix + name;
            if ((attr & 0x10) != 0) ReadDirectory(fs, start, length, full + "/");
            else FileList.Add(new GameFile(full, length, _partition + start * Sector));
        }
    }
}

/// <summary>ISO9660 with the Joliet extension (case-preserving names), e.g. images made by disc-imaging tools.</summary>
public sealed class Iso9660Source : ImageSource
{
    const long Sector = 2048;
    readonly bool _joliet;

    Iso9660Source(string path, bool joliet) : base(path) => _joliet = joliet;

    public override string Kind => _joliet ? "disc image (ISO9660 + Joliet)" : "disc image (ISO9660)";

    public static Iso9660Source? TryOpen(string path)
    {
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        byte[]? primary = null, joliet = null;
        for (long s = 16; s < 64 && (s + 1) * Sector <= fs.Length; s++)
        {
            var vd = ReadAt(fs, s * Sector, (int)Sector);
            if (Encoding.ASCII.GetString(vd, 1, 5) != "CD001") break;
            if (vd[0] == 255) break;
            if (vd[0] == 1) primary ??= vd;
            // Joliet: supplementary descriptor with a UCS-2 escape sequence (%/@, %/C or %/E).
            if (vd[0] == 2 && vd[88] == 0x25 && vd[89] == 0x2F && (vd[90] is 0x40 or 0x43 or 0x45)) joliet ??= vd;
        }
        var desc = joliet ?? primary;
        if (desc == null) return null;
        var src = new Iso9660Source(path, joliet != null);
        uint rootLba = BitConverter.ToUInt32(desc, 156 + 2);
        uint rootLen = BitConverter.ToUInt32(desc, 156 + 10);
        src.ReadDirectory(fs, rootLba, rootLen, "", 0);
        return src;
    }

    void ReadDirectory(FileStream fs, uint lba, uint length, string prefix, int depth)
    {
        if (depth > 32) throw new InvalidDataException("ISO directory tree is too deep (corrupt image?)");
        var data = ReadAt(fs, lba * Sector, (int)length);
        int o = 0;
        while (o < data.Length)
        {
            int len = data[o];
            if (len == 0) { o = (int)((o / Sector + 1) * Sector); continue; } // records never cross sectors
            uint extent = BitConverter.ToUInt32(data, o + 2), size = BitConverter.ToUInt32(data, o + 10);
            byte flags = data[o + 25], nameLen = data[o + 32];
            bool self = nameLen == 1 && (data[o + 33] == 0 || data[o + 33] == 1);
            if (!self)
            {
                string name = _joliet
                    ? Encoding.BigEndianUnicode.GetString(data, o + 33, nameLen & ~1)
                    : Encoding.ASCII.GetString(data, o + 33, nameLen);
                int semi = name.IndexOf(';');
                if (semi >= 0) name = name[..semi];
                if (!_joliet && name.EndsWith('.')) name = name[..^1];
                if ((flags & 0x80) != 0)
                    throw new InvalidDataException($"'{prefix + name}' is split into several extents; this image layout is not supported.");
                if ((flags & 0x02) != 0) ReadDirectory(fs, extent, size, prefix + name + "/", depth + 1);
                else FileList.Add(new GameFile(prefix + name, size, extent * Sector));
            }
            o += len;
        }
    }
}

/// <summary>Read-only window [offset, offset+length) of another stream; owns it.</summary>
public sealed class SubStream : Stream
{
    readonly Stream _inner;
    readonly long _start, _length;
    long _pos;

    public SubStream(Stream inner, long start, long length)
    {
        _inner = inner; _start = start; _length = length;
    }

    public override bool CanRead => true;
    public override bool CanSeek => true;
    public override bool CanWrite => false;
    public override long Length => _length;
    public override long Position { get => _pos; set => _pos = Math.Clamp(value, 0, _length); }

    public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));

    public override int Read(Span<byte> buffer)
    {
        long left = _length - _pos;
        if (left <= 0) return 0;
        if (buffer.Length > left) buffer = buffer[..(int)left];
        _inner.Position = _start + _pos;
        int n = _inner.Read(buffer);
        _pos += n;
        return n;
    }

    public override long Seek(long offset, SeekOrigin origin)
    {
        Position = origin switch
        {
            SeekOrigin.Begin => offset,
            SeekOrigin.Current => _pos + offset,
            _ => _length + offset,
        };
        return _pos;
    }

    public override void Flush() { }
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

    protected override void Dispose(bool disposing)
    {
        if (disposing) _inner.Dispose();
        base.Dispose(disposing);
    }
}
