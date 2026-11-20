using System.Buffers.Binary;

namespace OblivionInstaller;

/// <summary>
/// The game's shader containers from the player's copy, for the reNut build's shader stage (tools/renut/shader_dump.py,
/// ported): Data/Shaders/shaderpackage.sdp's entries and the containers stored whole in the other files under
/// Data/Shaders (Gamebryo's fragment cache), each written as &lt;vs|ps&gt;_&lt;XXH3-64 hex&gt;.bin, the name reNut's translator
/// looks a shader up by. The three shaders the game compiles at boot are in no file: the kit has port shaders for them.
/// </summary>
public static class ShaderDump
{
    public static int Run(GameSource src, string shaderDir, string outDir, Action<string> log)
    {
        Directory.CreateDirectory(outDir);
        var seen = new HashSet<string>();
        string prefix = shaderDir.TrimEnd('/') + "/";
        var files = src.Files.Where(f => f.Path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            .OrderBy(f => f.Path, StringComparer.OrdinalIgnoreCase).ToList();
        var sdp = files.FirstOrDefault(f => f.Path.Equals(prefix + "shaderpackage.sdp", StringComparison.OrdinalIgnoreCase))
                  ?? throw new InvalidOperationException($"{prefix}shaderpackage.sdp is not in the game copy");

        byte[] data = ReadAll(src, sdp);
        uint version = BinaryPrimitives.ReadUInt32BigEndian(data), count = BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(4));
        if (version != 100) throw new InvalidDataException($"shaderpackage.sdp version {version} (expected 100)");
        int p = 12, fromSdp = 0;
        for (uint i = 0; i < count; i++)
        {
            int size = (int)BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(p + 256));
            var blob = data.AsSpan(p + 260, size);
            p += 260 + size;
            if (ContainerSize(blob, 0) != size) throw new InvalidDataException("a shaderpackage.sdp entry is not one whole container");
            fromSdp += Save(outDir, blob, seen) ? 1 : 0;
        }
        int scanned = 0;
        foreach (var f in files)
        {
            if (f == sdp) continue;
            byte[] d = ReadAll(src, f);
            for (int o = 0; o + 4 <= d.Length; o++)
            {
                // The container magic: 10 2A, version 0E or 11, then 00 / 01 (low bit: vertex shader).
                if (d[o] != 0x10 || d[o + 1] != 0x2A || (d[o + 2] != 0x0E && d[o + 2] != 0x11) || d[o + 3] > 1) continue;
                int size = ContainerSize(d, o);
                if (size > 0) scanned += Save(outDir, d.AsSpan(o, size), seen) ? 1 : 0;
            }
        }
        log($"shaders: {fromSdp} from shaderpackage.sdp, {scanned} more from the other shader files ({seen.Count} in all)");
        return seen.Count;
    }

    /// <summary>Header + physical block size of the container at o, or 0 when the header does not hold together.</summary>
    static int ContainerSize(ReadOnlySpan<byte> d, int o)
    {
        if (o + 0x18 > d.Length) return 0;
        uint virt = BinaryPrimitives.ReadUInt32BigEndian(d[(o + 4)..]), phys = BinaryPrimitives.ReadUInt32BigEndian(d[(o + 8)..]);
        if (virt < 0x18 || virt >= 0x10000 || phys == 0 || phys >= 0x100000 || o + virt + phys > d.Length) return 0;
        return (int)(virt + phys);
    }

    static bool Save(string outDir, ReadOnlySpan<byte> blob, HashSet<string> seen)
    {
        string name = $"{((blob[3] & 1) != 0 ? "vs" : "ps")}_{Xxh3.Hash64(blob):x16}.bin";
        if (!seen.Add(name)) return false;
        File.WriteAllBytes(Path.Combine(outDir, name), blob.ToArray());
        return true;
    }

    static byte[] ReadAll(GameSource src, GameFile f)
    {
        using var s = src.Open(f);
        var buf = new byte[f.Size];
        s.ReadExactly(buf);
        return buf;
    }
}
