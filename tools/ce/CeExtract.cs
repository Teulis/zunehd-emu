using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

// Rebuilds XIP ROM modules (e32_rom/o32_rom) into loadable PE files and extracts ROM files.
public static class CeExtract
{
    const uint IMAGE_SCN_COMPRESSED = 0x00002000;
    static string H(uint v) { return "0x" + v.ToString("X8"); }

    public static uint FindRomhdr(CeImage img)
    {
        uint romhdr = BitConverter.ToUInt32(img.Flat, 0x44);
        foreach (var m in new uint[] { 0xF0000000, 0xFF000000, 0xFFF00000 })
            foreach (var cand in new uint[] { unchecked(img.Start - (romhdr & m)), unchecked(0u - (romhdr & m)) })
            {
                img.RomOff = cand;
                if (img.InImg(romhdr) && img.InImg(romhdr + 0x54) && unchecked(img.U32(romhdr + 8) + cand) == img.Start) return romhdr;
            }
        throw new Exception("ROMHDR delta not found");
    }

    public static string Run(string binPath, string outDir, string onlyModule)
    {
        var sb = new StringBuilder();
        var img = CeImage.Load(binPath);
        uint rh = FindRomhdr(img);
        uint nm = img.U32(rh + 16), nf = img.U32(rh + 48);
        string modDir = Path.Combine(outDir, "modules"), fileDir = Path.Combine(outDir, "files");
        Directory.CreateDirectory(modDir); Directory.CreateDirectory(fileDir);
        for (uint i = 0; i < nm; i++)
        {
            uint e = rh + 0x54 + i * 32;
            string name = img.Str(img.U32(e + 16));
            if (onlyModule != null && onlyModule.Length > 0 && !string.Equals(name, onlyModule, StringComparison.OrdinalIgnoreCase)) continue;
            uint e32 = img.U32(e + 20), o32 = img.U32(e + 24);
            try { sb.AppendLine(BuildPe(img, name, e32, o32, Path.Combine(modDir, name))); }
            catch (Exception ex) { sb.AppendLine(name + ": FAILED " + ex.Message); }
        }
        if (onlyModule == null || onlyModule.Length == 0)
        {
            uint ft = rh + 0x54 + nm * 32;
            for (uint i = 0; i < nf; i++)
            {
                uint e = ft + i * 28;
                uint attr = img.U32(e), real = img.U32(e + 12), comp = img.U32(e + 16), load = img.U32(e + 24);
                string name = img.Str(img.U32(e + 20));
                bool compressed = comp != real;
                File.WriteAllBytes(Path.Combine(fileDir, name + (compressed ? ".cecomp" : "")), img.Bytes(load, (int)comp));
                if (compressed) sb.AppendLine("file " + name + " is CE-compressed (comp 0x" + comp.ToString("X") + " real 0x" + real.ToString("X") + ")");
            }
        }
        return sb.ToString();
    }

    static string BuildPe(CeImage img, string name, uint e32, uint o32, string outPath)
    {
        ushort objcnt = img.U16(e32), flags = img.U16(e32 + 2);
        uint entry = img.U32(e32 + 4), vbase = img.U32(e32 + 8);
        ushort ssmaj = img.U16(e32 + 12), ssmin = img.U16(e32 + 14);
        uint stackmax = img.U32(e32 + 16), vsize = img.U32(e32 + 20), ts = img.U32(e32 + 32);
        var unitRva = new uint[9]; var unitSize = new uint[9];
        for (int u = 0; u < 9; u++) { unitRva[u] = img.U32(e32 + 36 + (uint)u * 8); unitSize[u] = img.U32(e32 + 40 + (uint)u * 8); }
        ushort subsys = img.U16(e32 + 108);
        if (objcnt == 0 || objcnt > 64) throw new Exception("bad objcnt " + objcnt);

        var info = new StringBuilder();
        info.AppendFormat("{0}: vbase {1} entry {2} vsize 0x{3:X} objs {4} flags 0x{5:X4} subsys {6}", name, H(vbase), H(vbase + entry), vsize, objcnt, flags, subsys);

        const uint fileAlign = 0x200;
        uint hdrSize = 0x400;
        var secs = new List<uint[]>(); // vsize, rva, psize, dataptr, realaddr, flags
        for (uint s = 0; s < objcnt; s++)
        {
            uint o = o32 + s * 24;
            secs.Add(new uint[] { img.U32(o), img.U32(o + 4), img.U32(o + 8), img.U32(o + 12), img.U32(o + 16), img.U32(o + 20) });
        }
        var raw = new List<byte[]>();
        uint fptr = hdrSize;
        var ptrs = new List<uint>();
        foreach (var s in secs)
        {
            byte[] data;
            uint vs = s[0], ps = s[2], dp = s[3], fl = s[5];
            if (ps == 0) data = new byte[0];
            else if ((fl & IMAGE_SCN_COMPRESSED) != 0)
            {
                info.AppendFormat("\n    section rva 0x{0:X} is compressed (psize 0x{1:X}); left zero-filled", s[1], ps);
                data = new byte[Align(vs, fileAlign)];
            }
            else
            {
                data = img.Bytes(dp, (int)Math.Min(ps, vs == 0 ? ps : Math.Max(ps, vs)));
                if (data.Length < ps) Array.Resize(ref data, (int)ps);
            }
            info.AppendFormat("\n    sec rva 0x{0:X5} vsz 0x{1:X6} psz 0x{2:X6} data {3} real {4} fl 0x{5:X8}", s[1], vs, ps, H(dp), H(s[4]), fl);
            byte[] padded = new byte[Align((uint)data.Length, fileAlign)];
            Buffer.BlockCopy(data, 0, padded, 0, data.Length);
            raw.Add(padded); ptrs.Add(padded.Length == 0 ? 0 : fptr); fptr += (uint)padded.Length;
        }

        var ms = new MemoryStream(); var w = new BinaryWriter(ms);
        // DOS header
        w.Write((ushort)0x5A4D); w.Write(new byte[0x3A]); w.Write((uint)0x80);
        w.Write(new byte[0x80 - 0x40]);
        // PE header
        w.Write((uint)0x00004550);
        w.Write((ushort)0x01C2); // ARM Thumb-interworking (THUMB machine); CE ARMV4I
        w.Write((ushort)objcnt); w.Write(ts); w.Write((uint)0); w.Write((uint)0);
        w.Write((ushort)0xE0);
        w.Write((ushort)(0x0102 | ((flags & 0x2000) != 0 ? 0x2000 : 0) | 0x0001)); // exec, 32bit, relocs stripped (XIP image is fixed)
        // Optional header
        w.Write((ushort)0x10B); w.Write((byte)8); w.Write((byte)0);
        uint code = 0, idata = 0; foreach (var s in secs) { if ((s[5] & 0x20) != 0) code += s[0]; else idata += s[0]; }
        w.Write(code); w.Write(idata); w.Write((uint)0);
        w.Write(entry); w.Write((uint)0x1000); w.Write((uint)0);
        w.Write(vbase); w.Write((uint)0x1000); w.Write(fileAlign);
        w.Write((ushort)4); w.Write((ushort)0); w.Write((ushort)0); w.Write((ushort)0); w.Write(ssmaj); w.Write(ssmin);
        w.Write((uint)0); w.Write(Align(vsize, 0x1000)); w.Write(hdrSize); w.Write((uint)0);
        w.Write(subsys == 0 ? (ushort)9 : subsys); w.Write((ushort)0);
        w.Write(stackmax); w.Write((uint)0x1000); w.Write((uint)0x100000); w.Write((uint)0x1000);
        w.Write((uint)0); w.Write((uint)16);
        // Data directories: ROM units 0..6 map to PE dirs 0..6 (exp,imp,res,exc,sec,fix,dbg).
        for (int d = 0; d < 16; d++)
        {
            if (d < 7 && d != 4) { w.Write(unitRva[d]); w.Write(unitSize[d]); }
            else { w.Write((uint)0); w.Write((uint)0); }
        }
        // Section headers
        string[] names = { ".text", ".data", ".pdata", ".rsrc", ".reloc", ".sec5", ".sec6", ".sec7" };
        for (int s = 0; s < secs.Count; s++)
        {
            uint fl = secs[s][5];
            string sn = (fl & 0x20) != 0 ? ".text" : ((fl & 0x80) != 0 ? ".bss" : ((fl & 0x80000000) != 0 ? ".data" : ".rdata"));
            if (secs[s][1] == unitRva[2] && unitRva[2] != 0) sn = ".rsrc";
            if (secs[s][1] == unitRva[3] && unitRva[3] != 0) sn = ".pdata";
            var nb = new byte[8]; var asc = Encoding.ASCII.GetBytes(sn); Buffer.BlockCopy(asc, 0, nb, 0, Math.Min(8, asc.Length)); w.Write(nb);
            w.Write(Math.Max(secs[s][0], (uint)raw[s].Length == 0 ? secs[s][0] : secs[s][0]));
            w.Write(secs[s][1]); w.Write((uint)raw[s].Length); w.Write(ptrs[s]);
            w.Write((uint)0); w.Write((uint)0); w.Write((ushort)0); w.Write((ushort)0);
            w.Write(fl & ~IMAGE_SCN_COMPRESSED & 0xFFFFF7FF);
        }
        w.Flush();
        if (ms.Length > hdrSize) throw new Exception("header overflow");
        ms.SetLength(hdrSize); ms.Position = hdrSize;
        foreach (var r in raw) ms.Write(r, 0, r.Length);
        File.WriteAllBytes(outPath, ms.ToArray());
        return info.ToString();
    }

    static uint Align(uint v, uint a) { return (v + a - 1) & ~(a - 1); }
}
