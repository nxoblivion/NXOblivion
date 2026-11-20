using System.Buffers.Binary;
using System.Numerics;

namespace OblivionInstaller;

/// <summary>
/// XXH3-64 with seed 0 and the default secret (xxHash 0.8, BSD-2-Clause, Yann Collet): the name reNut's translator
/// gives a shader container (tools/renut/shader_dump.py, Python's xxhash.xxh3_64). Scalar code, written from the
/// reference implementation's algorithm.
/// </summary>
public static class Xxh3
{
    const ulong P32_1 = 0x9E3779B1UL, P32_2 = 0x85EBCA77UL, P32_3 = 0xC2B2AE3DUL;
    const ulong P64_1 = 0x9E3779B185EBCA87UL, P64_2 = 0xC2B2AE3D27D4EB4FUL, P64_3 = 0x165667B19E3779F9UL;
    const ulong P64_4 = 0x85EBCA77C2B2AE63UL, P64_5 = 0x27D4EB2F165667C5UL;

    static readonly byte[] Secret =
    {
        0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
        0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
        0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
        0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
        0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
        0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
        0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
        0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
        0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
        0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
        0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
        0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
    };

    static ulong R64(ReadOnlySpan<byte> s, int o) => BinaryPrimitives.ReadUInt64LittleEndian(s[o..]);
    static uint R32(ReadOnlySpan<byte> s, int o) => BinaryPrimitives.ReadUInt32LittleEndian(s[o..]);

    static ulong Fold64(ulong a, ulong b)
    {
        ulong hi = Math.BigMul(a, b, out ulong lo);
        return lo ^ hi;
    }

    static ulong Avalanche(ulong h)
    {
        h ^= h >> 37;
        h *= 0x165667919E3779F9UL;
        return h ^ (h >> 32);
    }

    static ulong Xxh64Avalanche(ulong h)
    {
        h ^= h >> 33;
        h *= P64_2;
        h ^= h >> 29;
        h *= P64_3;
        return h ^ (h >> 32);
    }

    static ulong Rrmxmx(ulong h, ulong len)
    {
        h ^= BitOperations.RotateLeft(h, 49) ^ BitOperations.RotateLeft(h, 24);
        h *= 0x9FB21C651E98DF25UL;
        h ^= (h >> 35) + len;
        h *= 0x9FB21C651E98DF25UL;
        return h ^ (h >> 28);
    }

    static ulong Mix16(ReadOnlySpan<byte> d, int o, int so) =>
        Fold64(R64(d, o) ^ R64(Secret, so), R64(d, o + 8) ^ R64(Secret, so + 8));

    public static ulong Hash64(ReadOnlySpan<byte> d)
    {
        int len = d.Length;
        ReadOnlySpan<byte> s = Secret;
        if (len == 0)
            return Xxh64Avalanche(R64(s, 56) ^ R64(s, 64));
        if (len <= 3)
        {
            uint combined = ((uint)d[0] << 16) | ((uint)d[len >> 1] << 24) | d[len - 1] | ((uint)len << 8);
            return Xxh64Avalanche(combined ^ (ulong)(R32(s, 0) ^ R32(s, 4)));
        }
        if (len <= 8)
        {
            ulong input64 = R32(d, len - 4) + ((ulong)R32(d, 0) << 32);
            return Rrmxmx(input64 ^ (R64(s, 8) ^ R64(s, 16)), (ulong)len);
        }
        if (len <= 16)
        {
            ulong lo = R64(d, 0) ^ (R64(s, 24) ^ R64(s, 32));
            ulong hi = R64(d, len - 8) ^ (R64(s, 40) ^ R64(s, 48));
            return Avalanche((ulong)len + BinaryPrimitives.ReverseEndianness(lo) + hi + Fold64(lo, hi));
        }
        if (len <= 128)
        {
            ulong acc = (ulong)len * P64_1;
            if (len > 32)
            {
                if (len > 64)
                {
                    if (len > 96)
                    {
                        acc += Mix16(d, 48, 96);
                        acc += Mix16(d, len - 64, 112);
                    }
                    acc += Mix16(d, 32, 64);
                    acc += Mix16(d, len - 48, 80);
                }
                acc += Mix16(d, 16, 32);
                acc += Mix16(d, len - 32, 48);
            }
            acc += Mix16(d, 0, 0);
            acc += Mix16(d, len - 16, 16);
            return Avalanche(acc);
        }
        if (len <= 240)
        {
            ulong acc = (ulong)len * P64_1;
            int rounds = len / 16;
            for (int i = 0; i < 8; i++)
                acc += Mix16(d, 16 * i, 16 * i);
            acc = Avalanche(acc);
            for (int i = 8; i < rounds; i++)
                acc += Mix16(d, 16 * i, 16 * (i - 8) + 3);
            acc += Mix16(d, len - 16, 136 - 17);
            return Avalanche(acc);
        }
        return HashLong(d);
    }

    static void Accumulate512(Span<ulong> acc, ReadOnlySpan<byte> d, int o, int so)
    {
        for (int i = 0; i < 8; i++)
        {
            ulong v = R64(d, o + 8 * i);
            ulong k = v ^ R64(Secret, so + 8 * i);
            acc[i ^ 1] += v;
            acc[i] += (k & 0xFFFFFFFFUL) * (k >> 32);
        }
    }

    static void Scramble(Span<ulong> acc, int so)
    {
        for (int i = 0; i < 8; i++)
        {
            ulong a = acc[i];
            a ^= a >> 47;
            a ^= R64(Secret, so + 8 * i);
            acc[i] = a * P32_1;
        }
    }

    static ulong HashLong(ReadOnlySpan<byte> d)
    {
        const int secretSize = 192, stripeLen = 64, stripesPerBlock = (secretSize - stripeLen) / 8;
        const int blockLen = stripeLen * stripesPerBlock;
        int len = d.Length;
        Span<ulong> acc = stackalloc ulong[] { P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1 };
        int blocks = (len - 1) / blockLen;
        for (int n = 0; n < blocks; n++)
        {
            for (int st = 0; st < stripesPerBlock; st++)
                Accumulate512(acc, d, n * blockLen + st * stripeLen, st * 8);
            Scramble(acc, secretSize - stripeLen);
        }
        int lastStripes = ((len - 1) - blockLen * blocks) / stripeLen;
        for (int st = 0; st < lastStripes; st++)
            Accumulate512(acc, d, blocks * blockLen + st * stripeLen, st * 8);
        Accumulate512(acc, d, len - stripeLen, secretSize - stripeLen - 7);
        ulong result = (ulong)len * P64_1;
        for (int i = 0; i < 4; i++)
            result += Fold64(acc[2 * i] ^ R64(Secret, 11 + 16 * i), acc[2 * i + 1] ^ R64(Secret, 11 + 16 * i + 8));
        return Avalanche(result);
    }
}
