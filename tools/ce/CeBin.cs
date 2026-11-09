using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public class CeRecord { public uint Addr, Len, Sum; public long FileOff; }

public class CeImage
{
    public string Path; public uint Start, Length, Entry; public bool Ok;
    public List<CeRecord> Records = new List<CeRecord>();
    public byte[] Raw; // file bytes
    public byte[] Flat; // flattened image, base = Start
    public List<string> Log = new List<string>();

    public static CeImage Load(string path)
    {
        var img = new CeImage(); img.Path = path; img.Raw = File.ReadAllBytes(path);
        var b = img.Raw;
        if (Encoding.ASCII.GetString(b, 0, 7) != "B000FF\n") { img.Log.Add("bad sync"); return img; }
        img.Start = BitConverter.ToUInt32(b, 7); img.Length = BitConverter.ToUInt32(b, 11);
        long p = 15;
        while (p + 12 <= b.Length)
        {
            var r = new CeRecord { Addr = BitConverter.ToUInt32(b, (int)p), Len = BitConverter.ToUInt32(b, (int)p + 4), Sum = BitConverter.ToUInt32(b, (int)p + 8), FileOff = p + 12 };
            p += 12;
            if (r.Addr == 0 && r.Sum == 0) { img.Entry = r.Len; img.Ok = true; break; }
            if (p + r.Len > b.Length) { img.Log.Add(string.Format("record overruns file at 0x{0:X}", p - 12)); break; }
            uint s = 0; for (long i = 0; i < r.Len; i++) s += b[p + i];
            if (s != r.Sum) img.Log.Add(string.Format("checksum mismatch rec @0x{0:X8}", r.Addr));
            img.Records.Add(r); p += r.Len;
        }
        img.Flat = new byte[img.Length];
        foreach (var r in img.Records)
        {
            long o = (long)r.Addr - img.Start;
            if (o < 0 || o + r.Len > img.Length) { img.Log.Add(string.Format("record outside image 0x{0:X8}+0x{1:X}", r.Addr, r.Len)); continue; }
            Buffer.BlockCopy(b, (int)r.FileOff, img.Flat, (int)o, (int)r.Len);
        }
        return img;
    }

    public uint RomOff; // phys = va + RomOff
    uint P(uint va) { return unchecked(va + RomOff); }
    public bool InImg(uint va) { uint p = P(va); return p >= Start && p < Start + Length; }
    public uint U32(uint va) { return BitConverter.ToUInt32(Flat, (int)(P(va) - Start)); }
    public ushort U16(uint va) { return BitConverter.ToUInt16(Flat, (int)(P(va) - Start)); }
    public byte[] Bytes(uint va, int n) { var r = new byte[n]; Buffer.BlockCopy(Flat, (int)(P(va) - Start), r, 0, n); return r; }
    public string Str(uint va)
    {
        if (!InImg(va)) return string.Format("<0x{0:X8}>", va);
        int o = (int)(P(va) - Start), e = o; while (e < Flat.Length && Flat[e] != 0) e++;
        return Encoding.ASCII.GetString(Flat, o, e - o);
    }
}

public static class CeDump
{
    static string H(uint v) { return "0x" + v.ToString("X8"); }

    public static string Summary(string path, string outDir)
    {
        var sb = new StringBuilder();
        var img = CeImage.Load(path);
        sb.AppendLine("=== " + System.IO.Path.GetFileName(path));
        sb.AppendLine(string.Format("ImageStart {0}  ImageLen 0x{1:X}  Entry {2}  Records {3}  Terminated {4}", H(img.Start), img.Length, H(img.Entry), img.Records.Count, img.Ok));
        foreach (var l in img.Log) sb.AppendLine("  ! " + l);
        int n = 0; foreach (var r in img.Records) { if (n++ < 6) sb.AppendLine(string.Format("  rec {0} len 0x{1:X}", H(r.Addr), r.Len)); }
        if (!string.IsNullOrEmpty(outDir)) { Directory.CreateDirectory(outDir); File.WriteAllBytes(System.IO.Path.Combine(outDir, System.IO.Path.GetFileNameWithoutExtension(path) + ".nb0"), img.Flat); }

        // ECEC signature at Start+0x40
        if (img.Length > 0x48 && Encoding.ASCII.GetString(img.Flat, 0x40, 4) == "ECEC")
        {
            uint romhdr = BitConverter.ToUInt32(img.Flat, 0x44);
            // Pick the VA->PA delta for which ROMHDR.physfirst maps back to ImageStart.
            uint[] masks = { 0xF0000000, 0xFF000000, 0xFFF00000 };
            bool found = false;
            foreach (var m in masks)
            {
                foreach (var baseVa in new uint[] { romhdr & m })
                {
                    uint off = unchecked(img.Start - baseVa);
                    // try both: image base aligned to the mask, and image start offset within it
                    foreach (var cand in new uint[] { off, unchecked(0u - baseVa) })
                    {
                        img.RomOff = cand;
                        if (!img.InImg(romhdr) || !img.InImg(romhdr + 0x54)) continue;
                        uint pf = img.U32(romhdr + 8);
                        if (unchecked(pf + cand) == img.Start) { found = true; break; }
                    }
                    if (found) break;
                }
                if (found) break;
            }
            sb.AppendLine("ECEC found, ROMHDR @ " + H(romhdr) + (found ? "  VA->PA delta " + H(img.RomOff) : "  (no consistent VA->PA delta)"));
            if (found) DumpRomhdr(img, romhdr, sb);
        }
        else sb.AppendLine("no ECEC at +0x40");
        return sb.ToString();
    }

    static void DumpRomhdr(CeImage img, uint rh, StringBuilder sb)
    {
        string[] f = { "dllfirst","dlllast","physfirst","physlast","nummods","ulRAMStart","ulRAMFree","ulRAMEnd","ulCopyEntries","ulCopyOffset","ulProfileLen","ulProfileOffset","numfiles","ulKernelFlags","ulFSRamPercent","ulDrivglobStart","ulDrivglobLen" };
        var v = new uint[f.Length];
        for (int i = 0; i < f.Length; i++) { v[i] = img.U32(rh + (uint)(i * 4)); sb.AppendLine(string.Format("  {0,-16} {1}", f[i], H(v[i]))); }
        sb.AppendLine(string.Format("  {0,-16} 0x{1:X4}", "usCPUType", img.U16(rh + 0x44)));
        sb.AppendLine(string.Format("  {0,-16} 0x{1:X4}", "usMiscFlags", img.U16(rh + 0x46)));
        sb.AppendLine(string.Format("  {0,-16} {1}", "pExtensions", H(img.U32(rh + 0x48))));
        uint nm = v[4], nf = v[12];
        uint toc = rh + 0x54;
        sb.AppendLine("  -- modules (" + nm + ")");
        for (uint i = 0; i < nm && i < 2000; i++)
        {
            uint e = toc + i * 32;
            sb.AppendLine(string.Format("    {0,-28} size 0x{1,-8:X} e32 {2} o32 {3} load {4}", img.Str(img.U32(e + 16)), img.U32(e + 12), H(img.U32(e + 20)), H(img.U32(e + 24)), H(img.U32(e + 28))));
        }
        uint ft = toc + nm * 32;
        sb.AppendLine("  -- files (" + nf + ")");
        for (uint i = 0; i < nf && i < 5000; i++)
        {
            uint e = ft + i * 28;
            sb.AppendLine(string.Format("    {0,-36} real 0x{1,-8:X} comp 0x{2,-8:X} load {3}", img.Str(img.U32(e + 20)), img.U32(e + 12), img.U32(e + 16), H(img.U32(e + 24))));
        }
    }
}
