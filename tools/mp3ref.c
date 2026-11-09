/*
 * mp3ref: reference Layer III Huffman decode of an MP3 file with minimp3 (CC0),
 * for validating the Zune HD VDE bitstream engine model (qemu hw/arm/zunehd-bse.c).
 *
 * Output (text, one record per granule/channel):
 *   G frame gr ch part23 part3 big_values t0 t1 t2 r0 r1 count1tab block_type mixed fileoff bit
 *   B <hex of 700 main-data bytes starting at the byte holding the first part3 bit>
 *   V <576 integer spectral values (sign + linbits applied)>
 * part3 = part23 minus scalefactor bits; "bit" = first part3 bit within the first B byte (0 = MSB).
 * fileoff = file offset of the first B byte if it lies in the current frame, else -1.
 *
 * Build (MSYS2 UCRT64; an -O2 build segfaults, use -O0):
 *   gcc -O0 -I ../ext/minimp3 -o mp3ref.exe mp3ref.c -lm
 * Usage: mp3ref in.mp3 out.txt [maxframes]
 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int decode_frame(mp3dec_t *dec, const uint8_t *mp3, int mp3_bytes, FILE *out,
                        int frame_no, long base_off, int *consumed)
{
    int i = 0, igr, frame_size = 0;
    const uint8_t *hdr;
    bs_t bs_frame[1];
    static mp3dec_scratch_t scratch;

    i = mp3d_find_frame(mp3, mp3_bytes, &dec->free_format_bytes, &frame_size);
    if (!frame_size || i + frame_size > mp3_bytes) {
        *consumed = i ? i : mp3_bytes;
        return 0;
    }
    hdr = mp3 + i;
    memcpy(dec->header, hdr, HDR_SIZE);
    *consumed = i + frame_size;
    if (4 - HDR_GET_LAYER(hdr) != 3) {
        return 1;
    }
    int nch = HDR_IS_MONO(hdr) ? 1 : 2;
    bs_init(bs_frame, hdr + HDR_SIZE, frame_size - HDR_SIZE);
    if (HDR_IS_CRC(hdr)) {
        get_bits(bs_frame, 16);
    }
    int main_data_begin = L3_read_side_info(bs_frame, scratch.gr_info, hdr);
    if (main_data_begin < 0 || bs_frame->pos > bs_frame->limit) {
        mp3dec_init(dec);
        return 1;
    }
    int bytes_have = MINIMP3_MIN(dec->reserv, main_data_begin);
    int ok = L3_restore_reservoir(dec, bs_frame, &scratch, main_data_begin);
    /* main data byte k (k >= bytes_have) is at file offset frame_main + k - bytes_have */
    long frame_main = base_off + i + bs_frame->pos / 8;
    if (ok) {
        for (igr = 0; igr < (HDR_TEST_MPEG1(hdr) ? 2 : 1); igr++) {
            L3_gr_info_t *gi = scratch.gr_info + igr * nch;
            for (int ch = 0; ch < nch; ch++) {
                L3_gr_info_t *g = gi + ch;
                int limit = scratch.bs.pos + g->part_23_length;
                L3_decode_scalefactors(dec->header, scratch.ist_pos[ch], &scratch.bs, g,
                                       scratch.scf, ch);
                int p3 = scratch.bs.pos;
                for (int k = 0; k < 40; k++) {
                    scratch.scf[k] = 1.0f;
                }
                float buf[576];
                memset(buf, 0, sizeof(buf));
                L3_huffman(buf, &scratch.bs, g, scratch.scf, limit);
                int r0 = 0, r1 = 0;
                /* region counts as stored by minimp3 (r0 = region0_count+1 etc. for long) */
                r0 = g->region_count[0];
                r1 = g->region_count[1];
                long fo = (p3 / 8 >= bytes_have) ? frame_main + p3 / 8 - bytes_have : -1;
                fprintf(out, "G %d %d %d %d %d %d %d %d %d %d %d %d %d %d %ld %d\n", frame_no, igr, ch,
                        g->part_23_length, limit - p3, g->big_values, g->table_select[0],
                        g->table_select[1], g->table_select[2], r0, r1, g->count1_table,
                        g->block_type, g->mixed_block_flag, fo, p3 & 7);
                fprintf(out, "B ");
                for (int k = 0; k < 700; k++) {
                    int idx = p3 / 8 + k;
                    fprintf(out, "%02x", idx < (int)sizeof(scratch.maindata) ? scratch.maindata[idx] : 0);
                }
                fprintf(out, "\nV");
                for (int k = 0; k < 576; k++) {
                    float x = buf[k];
                    int v = (int)lrintf(powf(fabsf(x), 0.75f));
                    fprintf(out, " %d", x < 0 ? -v : v);
                }
                fprintf(out, "\n");
            }
        }
    }
    L3_save_reservoir(dec, &scratch);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mp3ref in.mp3 out.txt [maxframes]\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) {
        return 1;
    }
    fclose(f);
    FILE *out = fopen(argv[2], "w");
    int maxf = argc > 3 ? atoi(argv[3]) : 1 << 30;
    long off = 0;
    /* skip ID3v2 */
    if (n > 10 && !memcmp(d, "ID3", 3)) {
        off = 10 + (((d[6] & 0x7f) << 21) | ((d[7] & 0x7f) << 14) | ((d[8] & 0x7f) << 7) | (d[9] & 0x7f));
    }
    mp3dec_t dec;
    mp3dec_init(&dec);
    int fr = 0;
    while (off < n && fr < maxf) {
        int used = 0;
        if (decode_frame(&dec, d + off, (int)(n - off), out, fr, off, &used)) {
            fr++;
        }
        if (used <= 0) {
            break;
        }
        off += used;
    }
    fclose(out);
    fprintf(stderr, "%d frames\n", fr);
    return 0;
}
