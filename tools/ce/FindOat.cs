using System;
using System.IO;
using System.Text;

// Scans a flat CE image for OEMAddressTable candidates: runs of {VA>=0x80000000, PA, sizeMB} ending in {0,0,0}.
public static class FindOat
{
    public static string Scan(string nb0, uint baseVa)
    {
        var b = File.ReadAllBytes(nb0);
        var sb = new StringBuilder();
        for (int o = 0; o + 12 <= b.Length; o += 4)
        {
            int n = 0; int p = o;
            while (p + 12 <= b.Length)
            {
                uint va = BitConverter.ToUInt32(b, p), pa = BitConverter.ToUInt32(b, p + 4), mb = BitConverter.ToUInt32(b, p + 8);
                if (va == 0 && pa == 0 && mb == 0) break;
                if (va < 0x80000000 || va >= 0xA0000000 || (va & 0xFFFFF) != 0 || (pa & 0xFFFFF) != 0 || mb == 0 || mb > 512) { n = -1; break; }
                n++; p += 12;
            }
            if (n >= 3 && BitConverter.ToUInt32(b, o) == 0x80000000)
            {
                sb.AppendFormat("candidate @ VA 0x{0:X8} ({1} entries)\n", baseVa + (uint)o, n);
                for (int i = 0; i < n; i++)
                {
                    int q = o + i * 12;
                    uint va = BitConverter.ToUInt32(b, q), pa = BitConverter.ToUInt32(b, q + 4), mb = BitConverter.ToUInt32(b, q + 8);
                    sb.AppendFormat("   VA 0x{0:X8} (uncached 0x{3:X8})  ->  PA 0x{1:X8}  size {2} MB (PA end 0x{4:X8})\n", va, pa, mb, va + 0x20000000, pa + mb * 0x100000);
                }
            }
        }
        return sb.ToString();
    }
}
