using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Text;

namespace OblivionInstaller;

/// <summary>Port of switchbrew switch-tools' nacptool --create and elf2nro (icon + nacp, no romfs).</summary>
public static class Nro
{
    public static byte[] CreateNacp(string name, string author, string version)
    {
        var nacp = new byte[0x4000];
        var nameBytes = Truncate(name, 0x1FF);
        var authorBytes = Truncate(author, 0xFF);
        for (int i = 0; i < 12; i++)
        {
            nameBytes.CopyTo(nacp, i * 0x300);
            authorBytes.CopyTo(nacp, i * 0x300 + 0x200);
        }
        Truncate(version, 0xF).CopyTo(nacp, 0x3060);
        WriteU32(nacp, 0x3024, 0x100);
        WriteU32(nacp, 0x302C, 0xBFF);
        WriteU32(nacp, 0x3034, 0x10000);
        byte[] unk = { 0x0C, 0xFF, 0xFF, 0x0A, 0xFF, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D };
        for (int i = 0; i < 0x20; i++) nacp[0x3040 + i] = i < unk.Length ? unk[i] : (byte)0xFF;
        WriteU32(nacp, 0x3080, 0x3E00000);
        WriteU32(nacp, 0x3088, 0x180000);
        WriteU32(nacp, 0x30F0, 0x102);
        return nacp;
    }

    public static void ElfToNro(string elfPath, string nroPath, byte[] icon, byte[] nacp)
    {
        var elf = File.ReadAllBytes(elfPath);
        if (elf.Length < 64 || BitConverter.ToUInt32(elf, 0) != 0x464C457F) throw new InvalidDataException("not an ELF file");
        if (BitConverter.ToUInt16(elf, 18) != 183) throw new InvalidDataException("ELF is not AArch64");
        long phoff = (long)BitConverter.ToUInt64(elf, 32), shoff = (long)BitConverter.ToUInt64(elf, 40);
        int phentsize = BitConverter.ToUInt16(elf, 54), phnum = BitConverter.ToUInt16(elf, 56);
        int shentsize = BitConverter.ToUInt16(elf, 58), shnum = BitConverter.ToUInt16(elf, 60);

        // The first three PT_LOADs are text, rodata, data; bss is the data segment's memsz beyond its file size.
        var loads = new List<(long off, long vaddr, long filesz, long memsz)>();
        for (int i = 0; i < phnum && loads.Count < 3; i++)
        {
            int p = (int)(phoff + i * phentsize);
            if (BitConverter.ToUInt32(elf, p) != 1) continue;
            loads.Add(((long)BitConverter.ToUInt64(elf, p + 8), (long)BitConverter.ToUInt64(elf, p + 16),
                (long)BitConverter.ToUInt64(elf, p + 32), (long)BitConverter.ToUInt64(elf, p + 40)));
        }
        if (loads.Count != 3) throw new InvalidDataException("expected 3 loadable segments");

        var header = new byte[0x70];
        Encoding.ASCII.GetBytes("NRO0").CopyTo(header, 0);
        long fileOff = 0;
        var segOffsets = new long[3];
        for (int i = 0; i < 3; i++)
        {
            long size = Align(loads[i].filesz);
            segOffsets[i] = loads[i].vaddr;
            WriteU32(header, 0x10 + i * 8, (uint)loads[i].vaddr);
            WriteU32(header, 0x14 + i * 8, (uint)size);
            fileOff = Align(fileOff + size);
        }
        var data = loads[2];
        long dataFile = Align(data.filesz);
        WriteU32(header, 0x28, data.memsz > dataFile ? (uint)Align(data.memsz - dataFile) : 0);
        WriteU32(header, 0x08, (uint)fileOff);

        // GNU build id note -> header build id.
        for (int i = 0; i < shnum; i++)
        {
            int s = (int)(shoff + i * shentsize);
            if (BitConverter.ToUInt32(elf, s + 4) != 7) continue; // SHT_NOTE
            int n = (int)BitConverter.ToUInt64(elf, s + 24);
            int namesz = (int)BitConverter.ToUInt32(elf, n), descsz = (int)BitConverter.ToUInt32(elf, n + 4);
            int type = (int)BitConverter.ToUInt32(elf, n + 8);
            if (type == 3 && namesz == 4 && elf.AsSpan(n + 12, 4).SequenceEqual("GNU\0"u8))
                elf.AsSpan(n + 16, Math.Min(descsz, 0x20)).CopyTo(header.AsSpan(0x30));
        }

        using var outFile = new FileStream(nroPath, FileMode.Create, FileAccess.Write);
        outFile.SetLength(fileOff);
        for (int i = 0; i < 3; i++)
        {
            outFile.Position = segOffsets[i];
            outFile.Write(elf, (int)loads[i].off, (int)loads[i].filesz);
        }
        outFile.Position = 0x10; // after NroStart (crt0 branch + MOD0 offset)
        outFile.Write(header);

        // Asset section: header, icon, nacp.
        var aset = new byte[0x38];
        Encoding.ASCII.GetBytes("ASET").CopyTo(aset, 0);
        WriteU64(aset, 0x08, 0x38);
        WriteU64(aset, 0x10, (ulong)icon.Length);
        WriteU64(aset, 0x18, (ulong)(0x38 + icon.Length));
        WriteU64(aset, 0x20, (ulong)nacp.Length);
        outFile.Position = fileOff;
        outFile.Write(aset);
        outFile.Write(icon);
        outFile.Write(nacp);
    }

    /// <summary>A plain 256x256 JPEG icon drawn here (no game artwork is shipped).</summary>
    public static byte[] CreateIcon(string title, string subtitle)
    {
        using var bmp = new Bitmap(256, 256, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            using var bg = new LinearGradientBrush(new Rectangle(0, 0, 256, 256),
                Color.FromArgb(28, 30, 38), Color.FromArgb(70, 54, 30), 60f);
            g.FillRectangle(bg, 0, 0, 256, 256);
            using var ring = new Pen(Color.FromArgb(150, 214, 178, 110), 3f);
            g.DrawEllipse(ring, 38, 30, 180, 180);
            using var titleFont = new Font("Georgia", 30, FontStyle.Bold, GraphicsUnit.Pixel);
            using var subFont = new Font("Segoe UI", 15, FontStyle.Regular, GraphicsUnit.Pixel);
            var center = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
            using var gold = new SolidBrush(Color.FromArgb(236, 214, 170));
            using var grey = new SolidBrush(Color.FromArgb(190, 190, 196));
            g.DrawString(title.ToUpperInvariant(), titleFont, gold, new RectangleF(0, 92, 256, 56), center);
            g.DrawString(subtitle, subFont, grey, new RectangleF(0, 214, 256, 30), center);
        }
        using var ms = new MemoryStream();
        var jpeg = ImageCodecInfo.GetImageEncoders().First(c => c.FormatID == ImageFormat.Jpeg.Guid);
        using var ep = new EncoderParameters(1);
        ep.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 92L);
        bmp.Save(ms, jpeg, ep);
        return ms.ToArray();
    }

    static long Align(long v) => (v + 0xFFF) & ~0xFFFL;
    static byte[] Truncate(string s, int max)
    {
        var b = Encoding.UTF8.GetBytes(s);
        return b.Length > max ? b[..max] : b;
    }
    static void WriteU32(byte[] b, int o, uint v) => BitConverter.TryWriteBytes(b.AsSpan(o), v);
    static void WriteU64(byte[] b, int o, ulong v) => BitConverter.TryWriteBytes(b.AsSpan(o), v);
}
