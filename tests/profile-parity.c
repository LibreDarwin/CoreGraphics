/* profile-parity.c -- full ICC profile dump differential.
 *
 * Compiled twice by profile-parity.sh, once against Apple's CoreGraphics and
 * once against the framework we built, this prints a deterministic structural
 * transcript of the named profile space: one line per header field, the tag
 * table, a parsed record for every block whose type is understood, and hex
 * of every block -- tags and header alike -- so the transcripts are provably
 * byte-for-byte equivalent even where a block type has no parser.  The two
 * transcripts are then compared, first token as the label.
 *
 * The creation-date window (bytes 24..35) is zeroed before anything is
 * printed, for the same reason geometry-parity.c masks it rather than
 * compares it: the generic Lab profile is built by CreateLab, which is the
 * one builder here that stamps the live clock.  Every other profile is
 * byte-identical run to run, and masking the same window on both sides
 * cannot hide a difference in any of those.
 */

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int CGColorSpaceGetType(CGColorSpaceRef); /* private SPI, not in headers */

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Print n bytes, ASCII as itself and other bytes as \xHH, so a transcript
   can carry arbitrary block text without ever injecting a line break. */
static void print_ascii(const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (p[i] >= 0x20 && p[i] <= 0x7e && p[i] != '\\')
            putchar(p[i]);
        else
            printf("\\x%02x", p[i]);
    }
}

/* Print a UTF-16BE string, honoring its BOM, as ASCII or \uXXXX/\UXXXXXXXX.
   The mluc strings shipped in these profiles are small, so the expense does
   not matter, and the output is a pure function of the bytes, which keeps the
   two transcripts equal exactly when the block is. */
static void print_utf16(const unsigned char *p, size_t n)
{
    size_t i = 0;
    int be = 1;
    unsigned cp = 0;

    if (n >= 2) {
        uint16_t bom = be16(p);

        if (bom == 0xfffe) {
            be = 0;
            i = 2;
        } else if (bom == 0xfeff) {
            i = 2;
        }
    }
    for (; i + 1 < n; i += 2) {
        uint16_t u = be ? be16(p + i) : (uint16_t)((p[i] << 8) | p[i + 1]);

        if (u >= 0xd800 && u <= 0xdbff && i + 3 < n) {
            uint16_t lo = be ? be16(p + i + 2)
                : (uint16_t)((p[i + 2] << 8) | p[i + 3]);

            if (lo >= 0xdc00 && lo <= 0xdfff) {
                cp = 0x10000 + (((unsigned)u - 0xd800) << 10)
                    + (lo - 0xdc00);
                printf("\\U%08x", cp);
                i += 2;
                continue;
            }
        }
        if (u >= 0x20 && u <= 0x7e && u != '\\')
            putchar(u);
        else
            printf("\\u%04x", u);
    }
}

/* The number of CLUT entries a lut geometry describes, or (size_t)-1 when the
   geometry is not believable.  A CLUT is grid^in * out; everything here is a
   few tens of thousands of entries at most, so the product cap below reads
   anything larger as garbage rather than believed.  The generic gamut table
   runs at a grid of 21, past the 2..17 the spec allows, so only the absurd
   is refused. */
static size_t lut_clut(unsigned in, unsigned out, unsigned grid)
{
    size_t n = 1;
    unsigned i;

    if (in > 16 || out > 16 || grid < 2)
        return (size_t)-1;
    for (i = 0; i < in; i++) {
        n *= grid;
        if (n > ((size_t)1 << 20) / 17)
            return (size_t)-1;
    }
    return n * out;
}

/* The min, max and sum of up to want entries, stepping stride bytes per entry
   and never past the bytes the block actually holds.  Both sides scan the
   same bytes, so the transcript stays a pure function of the block, and the
   count that is returned lets a truncated table print the rows it has. */
static size_t table_stats(const unsigned char *p, size_t avail, size_t want,
    unsigned stride, uint32_t *sum, uint32_t *mn, uint32_t *mx)
{
    size_t i, n = avail / (size_t)stride;
    uint32_t s = 0, lo = (uint32_t)-1, hi = 0;

    if (n > want)
        n = want;
    for (i = 0; i < n; i++) {
        uint32_t v = stride == 2 ? be16(p + 2 * i) : p[i];
        s += v;
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
    }
    *sum = s;
    *mn = n ? lo : 0;
    *mx = n ? hi : 0;
    return n;
}

/* One 'mft1'/'mft2' block.  The shapes the writers here produce are the 8-bit
   lut8, the 16-bit lut16 and the lut8to16 hybrid, and all three share this
   header: channels, grid and matrix, with the lut16 pair carrying the entry
   counts its 16-bit tables need.  A read only ever touches bytes the block
   holds, and the block's total length decides whether the wide or the narrow
   input tables are believed, so a recovered constant that runs short of the
   shape its own header promises -- the generic Lab and XYZ LUT does -- prints
   the rows it actually has and the tail, rather than being misread. */
static void dump_lut(const unsigned char *p, size_t len, const char *tag)
{
    unsigned in, out, grid, inE = 0, outE = 0;
    size_t clut, base;
    unsigned i;
    int iw, ow;

    if (len < 56) {
        printf("lut %s <short>\n", tag);
        return;
    }
    in = p[8];
    out = p[9];
    grid = p[10];
    printf("lut %s sig=%.4s in=%u out=%u grid=%u matrix", tag,
        (const char *)p, in, out, grid);
    for (i = 0; i < 9; i++)
        printf("%s0x%08x", i ? " " : "=", be32(p + 12 + 4 * i));
    putchar('\n');

    clut = lut_clut(in, out, grid);
    if (clut == (size_t)-1) {
        printf("lut %s <unparsed geometry>\n", tag);
        return;
    }
    if (memcmp(p, "mft1", 4) == 0) {
        iw = ow = 0;
        inE = outE = 256;
    } else if (memcmp(p, "mft2", 4) == 0) {
        inE = be16(p + 48);
        outE = be16(p + 50);
        if (inE > 4096 || outE > 4096) {
            printf("lut %s <entry count out of range>\n", tag);
            return;
        }
        /* Prefer the 16-bit input tables unless they would run past the end
           of the block, in which case the half-width ones of lut8to16 do. */
        iw = ow = 1;
        if (56 + (size_t)in * inE * 2 + clut * 2 > len)
            iw = 0;
    } else {
        printf("lut %s <unknown sig>\n", tag);
        return;
    }

    /* The input ramps, then the CLUT, then the output ramps, each row's min,
       max and sum of as many entries as the block still holds. */
    base = 56;
    for (i = 0; i < in && base < len; i++) {
        uint32_t sum, mn, mx;
        size_t n = table_stats(p + base, len - base, inE, iw ? 2 : 1,
            &sum, &mn, &mx);

        printf("lut_tab %s in i=%u n=%zu min=%u max=%u sum=%u\n", tag, i,
            n, mn, mx, (unsigned)sum);
        base += (size_t)inE * (iw ? 2 : 1);
    }
    if (base < len) {
        uint32_t sum, mn, mx;
        size_t n = table_stats(p + base, len - base, clut, ow ? 2 : 1,
            &sum, &mn, &mx);

        printf("lut_tab %s clut n=%zu min=%u max=%u sum=%u\n", tag, n,
            mn, mx, (unsigned)sum);
        base += clut * (size_t)(ow ? 2 : 1);
    }
    for (i = 0; i < out && base < len; i++) {
        uint32_t sum, mn, mx;
        size_t n = table_stats(p + base, len - base, outE, ow ? 2 : 1,
            &sum, &mn, &mx);

        printf("lut_tab %s out i=%u n=%zu min=%u max=%u sum=%u\n", tag, i,
            n, mn, mx, (unsigned)sum);
        base += (size_t)outE * (ow ? 2 : 1);
    }
    if (base < len)
        printf("lut %s tail=%zu\n", tag, len - base);
}

/* One 'mAB '/'mBA ' block.  The five section offsets at 12 say where B, the
   matrix, M, the CLUT and A begin, so their spans are read from the offsets
   themselves; B and A (and the M sections of these profiles) are runs of
   curve/para tags walked from each section's start and stopped by the first
   signature the walk does not know or the first tag that would run past the
   section's end.  The hex below is the ground truth for the parts a parse
   does not name. */
static void dump_alt(const unsigned char *p, size_t len, const char *tag)
{
    static const char *const part[5] = { "B", "Mtx", "M", "CLUT", "A" };
    uint32_t off[5];
    int i;

    if (len < 32) {
        printf("lut_alt %s <short>\n", tag);
        return;
    }
    for (i = 0; i < 5; i++)
        off[i] = be32(p + 12 + 4 * i);
    printf("lut_alt %s sig=%.4s in=%u out=%u b=0x%x mtx=0x%x m=0x%x "
        "clut=0x%x a=0x%x\n", tag, (const char *)p, p[8], p[9], off[0],
        off[1], off[2], off[3], off[4]);

    for (i = 0; i < 5; i++) {
        size_t j, end;
        uint32_t o = off[i];

        if (o == 0) {
            printf("lut_sec %s part=%s off=0 <absent>\n", tag, part[i]);
            continue;
        }
        if (o >= len) {
            printf("lut_sec %s part=%s off=0x%x <out of bounds>\n", tag,
                part[i], o);
            continue;
        }
        end = len;
        for (j = 0; j < 5; j++)
            if (off[j] > o && off[j] <= len && off[j] < end)
                end = off[j];
        printf("lut_sec %s part=%s off=0x%x span=0x%zx\n", tag, part[i], o,
            end - o);

        if (i == 1) {
            /* The matrix is 12 fixed-point words; the pad between the matrix
               and M is inside the section, so it prints as zero words. */
            size_t k;

            printf("lut_sec %s part=Mtx matrix", tag);
            for (k = 0; k < 12 && o + 4 * (k + 1) <= end; k++)
                printf("%s0x%08x", k ? " " : "=", be32(p + o + 4 * k));
            putchar('\n');
        } else if (i == 3) {
            /* A CLUT begins with its grid points, one byte per input. */
            size_t g = p[8] < 3 ? p[8] : 3, k;

            printf("lut_sec %s part=CLUT grid=", tag);
            for (k = 0; k < g && o + k + 1 <= end; k++)
                printf("%s%02x", k ? " " : "", p[o + k]);
            putchar('\n');
        } else {
            /* B, M and A are runs of curve/para tags. */
            int k = 0;

            while (o + 12 <= end) {
                const char *s = (const char *)p + o;
                size_t clen;

                if (memcmp(s, "curv", 4) == 0) {
                    unsigned count = be32(p + o + 8);

                    clen = 12 + 2 * (size_t)count;
                    if (clen > end - o)
                        break;
                    if (count == 0) {
                        printf("lut_curv %s part=%s k=%d identity\n", tag,
                            part[i], k);
                    } else if (count == 1) {
                        printf("lut_curv %s part=%s k=%d gamma=0x%04x\n",
                            tag, part[i], k, be16(p + o + 12));
                    } else {
                        printf("lut_curv %s part=%s k=%d count=%u "
                            "first=0x%04x last=0x%04x\n", tag, part[i], k,
                            count, be16(p + o + 12),
                            be16(p + o + 12 + 2 * (count - 1)));
                    }
                } else if (memcmp(s, "para", 4) == 0) {
                    static const unsigned char np[5] = { 1, 3, 4, 5, 7 };
                    unsigned fn = be16(p + o + 8);
                    unsigned n = fn < 5 ? np[fn] : 0, q;

                    clen = 12 + 4 * (size_t)n;
                    if (clen > end - o)
                        break;
                    printf("lut_para %s part=%s k=%d function=%u", tag,
                        part[i], k, fn);
                    for (q = 0; q < n; q++)
                        printf(" p%u=0x%08x", q, be32(p + o + 12 + 4 * q));
                    putchar('\n');
                } else {
                    break;
                }
                o += clen;
                k++;
                if (k >= 64)
                    break;
            }
        }
    }
}

/* One tag's block, parsed where its type is understood and always hexed.  The
   'tag' argument names the tag, so a mismatch report says which one it was. */
static void dump_block(const unsigned char *p, size_t len, const char *tag)
{
    size_t i, j;

    printf("blk %s sig=", tag);
    if (len >= 4)
        printf("%.4s", (const char *)p);
    else
        printf("----");
    printf(" len=%zu\n", len);

    if (len >= 4) {
        const char *sig = (const char *)p;

        if (!memcmp(sig, "XYZ ", 4)) {
            if (len >= 20)
                printf("xyz %s X=0x%08x Y=0x%08x Z=0x%08x\n", tag,
                    be32(p + 8), be32(p + 12), be32(p + 16));
        } else if (!memcmp(sig, "curv", 4)) {
            unsigned count;

            if (len < 12) {
                printf("curv %s <short>\n", tag);
            } else {
                count = be32(p + 8);
                if (count == 1) {
                    printf("curv %s gamma=0x%04x\n", tag, be16(p + 12));
                } else {
                    printf("curv %s count=%u\n", tag, count);
                    for (i = 0; i < count && 12 + 2 * i + 1 < len; i++)
                        printf("curv_table %s %zu 0x%04x\n", tag, i,
                            be16(p + 12 + 2 * i));
                    if (12 + 2 * count > len)
                        printf("curv_table %s <truncated>\n", tag);
                }
            }
        } else if (!memcmp(sig, "para", 4)) {
            /* Parametric curve: the parameter count belongs to the function
               type rather than to the block length, so the block length
               cannot be trusted to say how many parameters there are.  The
               over-read that would otherwise print is padding, and it is the
               same on both sides, so it is noise rather than a difference --
               but it would mislead. */
            static const unsigned char nparam[5] = { 1, 3, 4, 5, 7 };
            unsigned fn = be16(p + 8);
            unsigned k, n = fn < 5 ? nparam[fn] : 0;

            printf("para %s function=%u", tag, fn);
            for (k = 0; k < n && 12 + 4 * k + 3 < len; k++)
                printf(" p%u=0x%08x", k, be32(p + 12 + 4 * k));
            putchar('\n');
        } else if (!memcmp(sig, "text", 4)) {
            printf("text %s <", tag);
            print_ascii(p + 8, len - 8);
            printf(">\n");
        } else if (!memcmp(sig, "desc", 4)) {
            unsigned count = be32(p + 8);

            if (len >= 12 && count <= len - 12) {
                printf("desc %s <", tag);
                print_ascii(p + 12, count);
                printf(">\ndesc_tail %s ", tag);
                for (i = 12 + count; i < len; i++)
                    printf("%02x", p[i]);
                putchar('\n');
            }
        } else if (!memcmp(sig, "mluc", 4)) {
            unsigned count, recsize;

            if (len < 16) {
                printf("mluc %s <short>\n", tag);
            } else {
                count = be32(p + 8);
                recsize = be32(p + 12);
                printf("mluc %s count=%u recsize=%u\n", tag, count, recsize);
                for (i = 0; i < count && 16 + recsize * i + 12 <= len; i++) {
                    const unsigned char *r = p + 16 + recsize * i;
                    unsigned bytes = be32(r + 4);
                    unsigned off = be32(r + 8);

                    printf("mluc_rec %s i=%zu lang=%.4s len=%u off=%u <", tag,
                        i, (const char *)r, bytes, off);
                    if (off + bytes <= len)
                        print_utf16(p + off, bytes);
                    else
                        printf("<out of bounds>");
                    printf(">\n");
                }
            }
        } else if (!memcmp(sig, "sf32", 4)) {
            if (len >= 44) {
                printf("sf32 %s", tag);
                for (i = 0; i < 9; i++)
                    printf(" v%zu=0x%08x", i, be32(p + 8 + 4 * i));
                putchar('\n');
            }
        } else if (!memcmp(sig, "cicp", 4)) {
            if (len >= 12)
                printf("cicp %s %u %u %u %u\n", tag, p[8], p[9], p[10], p[11]);
        } else if (!memcmp(sig, "view", 4)) {
            if (len >= 32) {
                printf("view %s illum=0x%08x 0x%08x 0x%08x surround=0x%08x "
                    "0x%08x 0x%08x lux=%u\n", tag, be32(p + 8), be32(p + 12),
                    be32(p + 16), be32(p + 20), be32(p + 24), be32(p + 28),
                    be32(p + 32));
            }
        } else if (!memcmp(sig, "meas", 4)) {
            if (len >= 32) {
                printf("meas %s observer=%u backing=0x%08x 0x%08x 0x%08x "
                    "geometry=%u flare=%u illum=%u\n", tag, be32(p + 8),
                    be32(p + 12), be32(p + 16), be32(p + 20), be32(p + 24),
                    be16(p + 28), be16(p + 30));
            }
        } else if (!memcmp(sig, "sig ", 4)) {
            if (len >= 12)
                printf("sig %s %.4s\n", tag, (const char *)p + 8);
        } else if (!memcmp(sig, "mft1", 4) || !memcmp(sig, "mft2", 4)) {
            dump_lut(p, len, tag);
        } else if (!memcmp(sig, "mAB ", 4) || !memcmp(sig, "mBA ", 4)) {
            dump_alt(p, len, tag);
        }
        /* The stored extras (vcgt, ndin, ...) are recovered constants and stay
           opaque: their hex below still makes the transcript provably
           byte-equivalent, which is what the differential needs. */
    }

    printf("hex %s ", tag);
    for (j = 0; j < len; j += 16) {
        printf("%s%04zx", j ? " " : "", j);
        for (i = 0; i < 16 && j + i < len; i++)
            printf(" %02x", p[j + i]);
        putchar('\n');
        if (j + 16 < len)
            printf("hex %s ", tag);
    }
}

static void dump_profile(const unsigned char *data, size_t len)
{
    unsigned char *p;
    unsigned count, i;
    size_t j;

    /* The one field that is not a constant masked to zero, both sides, for
       the reason given at the top of this file. */
    p = malloc(len ? len : 1);
    if (!p)
        exit(2);
    memcpy(p, data, len);
    memset(p + 24, 0, 12);

    printf("hdr_size 0x%08x\n", be32(p + 0));
    printf("hdr_cmm %.4s\n", (const char *)p + 4);
    printf("hdr_version 0x%08x\n", be32(p + 8));
    printf("hdr_class %.4s\n", (const char *)p + 12);
    printf("hdr_space %.4s\n", (const char *)p + 16);
    printf("hdr_pcs %.4s\n", (const char *)p + 20);
    printf("hdr_date_masked %u %u %u %u %u %u\n", be16(p + 24),
        be16(p + 26), be16(p + 28), be16(p + 30), be16(p + 32), be16(p + 34));
    printf("hdr_flags 0x%08x\n", be32(p + 40));
    printf("hdr_platform %.4s\n", (const char *)p + 36);
    printf("hdr_manufacturer %.4s\n", (const char *)p + 48);
    printf("hdr_model %.4s\n", (const char *)p + 52);
    printf("hdr_attributes 0x%08x 0x%08x\n", be32(p + 56), be32(p + 60));
    printf("hdr_intent %u\n", be32(p + 64));
    printf("hdr_illum 0x%08x 0x%08x 0x%08x\n", be32(p + 68), be32(p + 72),
        be32(p + 76));
    printf("hdr_creator %.4s\n", (const char *)p + 80);
    printf("hdr_profileid ");
    for (j = 84; j < 100 && j < len; j++)
        printf("%02x", p[j]);
    putchar('\n');

    printf("hex _hdr ");
    for (j = 0; j < 128 && j < len; j += 16) {
        printf("%s%04zx", j ? " " : "", j);
        for (i = 0; i < 16 && j + i < len; i++)
            printf(" %02x", p[j + i]);
        putchar('\n');
        if (j + 16 < 128 && j + 16 < len)
            printf("hex _hdr ");
    }

    count = be32(p + 128);
    printf("tag_count %u\n", count);
    for (i = 0; i < count; i++) {
        const unsigned char *e;
        unsigned off, tlen;

        if (132 + 12 * (size_t)i + 12 > len)
            break;
        e = p + 132 + 12 * (size_t)i;
        printf("tag %.4s off=%u len=%u\n", (const char *)e, be32(e + 4),
            be32(e + 8));
        off = be32(e + 4);
        tlen = be32(e + 8);
        if (off > len || tlen > len - off) {
            printf("block_oor %.4s off=%u len=%u\n", (const char *)e, off,
                tlen);
            continue;
        }
        if (p + off + tlen <= p + len)
            dump_block(p + off, tlen, (const char *)e);
    }

    free(p);
}

static void dump_name(const char *name)
{
    CFStringRef key = CFStringCreateWithCString(NULL, name,
        kCFStringEncodingUTF8);
    CGColorSpaceRef cs;

    printf("name %s\n", name);
    cs = CGColorSpaceCreateWithName(key);
    printf("null %d\n", cs == NULL);
    if (cs) {
        CFStringRef reported = CGColorSpaceCopyName(cs);
        char buf[256];

        printf("model %ld\n", (long)CGColorSpaceGetModel(cs));
        printf("ncomp %zu\n", CGColorSpaceGetNumberOfComponents(cs));
        printf("type %d\n", CGColorSpaceGetType(cs));
        printf("out %d\n", CGColorSpaceSupportsOutput(cs) ? 1 : 0);
        printf("extended %d\n", CGColorSpaceUsesExtendedRange(cs) ? 1 : 0);
        printf("wide %d\n", CGColorSpaceIsWideGamutRGB(cs) ? 1 : 0);
        printf("stable %d\n", CGColorSpaceCreateWithName(key) == cs);
        printf("base_null %d\n", CGColorSpaceGetBaseColorSpace(cs) == NULL);
        if (reported && CFStringGetCString(reported, buf, sizeof buf,
                kCFStringEncodingUTF8))
            printf("report %s\n", buf);
        else
            printf("report <null>\n");
        if (reported)
            CFRelease(reported);

        {
            CFDataRef d = CGColorSpaceCopyICCData(cs);

            printf("icc_null %d\n", d == NULL);
            if (d) {
                printf("icc_len %zu\n", CFDataGetLength(d));
                dump_profile(CFDataGetBytePtr(d), CFDataGetLength(d));
                CFRelease(d);
            }
        }
        CGColorSpaceRelease(cs);
    }
    CFRelease(key);
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
        dump_name(argv[i]);
    return 0;
}