/* api-parity.c -- accessor differential across the space matrix.
 *
 * Compiled twice by api-parity.sh, once against Apple's CoreGraphics and once
 * against the framework we built, this builds the widest practical set of
 * CGColorSpace objects -- the named spaces from argv plus a fixed matrix of
 * device, calibrated, Lab, ICC-from-data, pattern and indexed constructors,
 * each with its extended, linearized and extended-linearized derivatives --
 * and prints one deterministic line per space carrying the public accessor
 * surface: type, model, component count, name, base space, the full ICC
 * profile bytes, the extended range, HDR/HLG/PQ/wide-gamut flags, output
 * support, self-equality, the uncalibrated/ICC-compatible/PS-2 flags, the
 * ITU-R BT.2100 transfer-function flag, the rendering intent and the
 * type ID, with the indexed colour table when there is one.
 *
 * The transcript is compared by api-parity.sh, first token as the label.
 *
 * Two byte windows are deliberately not compared.  Header bytes 24..35 of
 * every profile are zeroed before printing, for the same reason
 * profile-parity.c masks them: CGColorSpaceCreateLab stamps the live clock
 * into that window, and the two sides create their profiles at different
 * instants.  Masking the same window on both sides cannot hide a difference
 * in any other byte.
 */

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int CGColorSpaceGetType(CGColorSpaceRef); /* private SPI, not in headers */
extern bool CGColorSpaceEqualToColorSpace(CGColorSpaceRef,
                                          CGColorSpaceRef); /* SPI */
extern bool CGColorSpaceIsUncalibrated(CGColorSpaceRef); /* SPI */
extern bool CGColorSpaceIsICCCompatible(CGColorSpaceRef); /* SPI */
extern bool CGColorSpaceIsPSLevel2Compatible(CGColorSpaceRef); /* SPI */
extern bool CGColorSpaceIgnoresIntent(CGColorSpaceRef); /* SPI */
extern int CGColorSpaceGetRenderingIntent(CGColorSpaceRef); /* SPI */
extern bool CGColorSpaceUsesITUR_2100TF(CGColorSpaceRef); /* SPI */

/* The creation-date window, bytes 24..35, stamped by CGColorSpaceCreateLab. */
#define ICC_DATE_OFF 24
#define ICC_DATE_LEN 12

/* Print n bytes as fixed-width hex with no separators, a single token. */
static void print_hex(const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
}

/* Print every byte of the space's profile, the creation-date window masked
   to zeros and the profile id intact.  No profile (device spaces, ...
   spaces with none) prints '-'. */
static void print_icc(CGColorSpaceRef cs)
{
    CFDataRef d = CGColorSpaceCopyICCData(cs);
    size_t len, i;
    const unsigned char *p;

    if (!d) {
        printf("-");
        return;
    }
    len = CFDataGetLength(d);
    p = CFDataGetBytePtr(d);
    print_hex(p, ICC_DATE_OFF);
    for (i = 0; i < ICC_DATE_LEN; i++)
        printf("00");
    if (len >= ICC_DATE_OFF + ICC_DATE_LEN)
        print_hex(p + ICC_DATE_OFF + ICC_DATE_LEN,
                  len - (ICC_DATE_OFF + ICC_DATE_LEN));
    else if (len > ICC_DATE_OFF)
        print_hex(p + len - (ICC_DATE_OFF), ICC_DATE_OFF - len);
    CFRelease(d);
}

/* The space's name, or '-' when it has none.  Public names are ASCII
   identifiers (kCGColorSpace...), so the C-string projection is exact. */
static void print_name(CGColorSpaceRef cs)
{
    CFStringRef nm = CGColorSpaceCopyName(cs);
    char buf[256];

    if (!nm) {
        printf("-");
        return;
    }
    if (CFStringGetCString(nm, buf, sizeof buf, kCFStringEncodingASCII))
        printf("%s", buf);
    else
        printf("?");
    CFRelease(nm);
}

/* The base space (non-owned) as type/model/components/name, or '-'. */
static void print_base(CGColorSpaceRef cs)
{
    CGColorSpaceRef b = CGColorSpaceGetBaseColorSpace(cs);

    if (!b) {
        printf("-");
        return;
    }
    printf("%d/%d/%zu/", CGColorSpaceGetType(b),
           (int)CGColorSpaceGetModel(b),
           CGColorSpaceGetNumberOfComponents(b));
    print_name(b);
}

/* The indexed colour table as a hex token: count, a colon, then count times
   the base's component count bytes.  Non-indexed spaces have no table. */
static void print_table(CGColorSpaceRef cs)
{
    size_t count = CGColorSpaceGetColorTableCount(cs);
    size_t n, cap;
    unsigned char *buf;

    if (count == 0) {
        printf("ct -");
        return;
    }
    n = count * CGColorSpaceGetNumberOfComponents(cs);
    cap = n < 1024 ? 1024 : n;    /* indexed tables are at most 256x4 bytes */
    buf = calloc(1, cap);
    if (buf)
        CGColorSpaceGetColorTable(cs, buf);
    printf(" ct %zu:", count);
    if (buf) {
        print_hex(buf, n);
        free(buf);
    }
}

/* Print one transcript line for a space, NULL or not. */
static void probe(const char *label, CGColorSpaceRef cs)
{
    if (!cs) {
        printf("%s null\n", label);
        return;
    }
    printf("%s type %d model %d comp %zu name ",
           label, CGColorSpaceGetType(cs),
           (int)CGColorSpaceGetModel(cs),
           CGColorSpaceGetNumberOfComponents(cs));
    print_name(cs);
    printf(" base ");
    print_base(cs);
    printf(" icc ");
    print_icc(cs);
    printf(" ext %d hdr %d pq %d hlg %d wide %d sup %d eq %d"
           " uncal %d iccc %d ps2 %d itur %d intn %d ignt %d typeid %d",
           CGColorSpaceUsesExtendedRange(cs),
           CGColorSpaceIsHDR(cs),
           CGColorSpaceIsPQBased(cs),
           CGColorSpaceIsHLGBased(cs),
           CGColorSpaceIsWideGamutRGB(cs),
           CGColorSpaceSupportsOutput(cs),
           CGColorSpaceEqualToColorSpace(cs, cs),
           CGColorSpaceIsUncalibrated(cs),
           CGColorSpaceIsICCCompatible(cs),
           CGColorSpaceIsPSLevel2Compatible(cs),
           CGColorSpaceUsesITUR_2100TF(cs),
           CGColorSpaceGetRenderingIntent(cs),
           CGColorSpaceIgnoresIntent(cs),
           (int)CGColorSpaceGetTypeID());
    print_table(cs);
    printf("\n");
}

/* A named space from argv, with its extended, linearized and
   extended-linearized derivatives. */
static void probe_named(const char *name)
{
    CGColorSpaceRef base = CGColorSpaceCreateWithName(
        CFStringCreateWithCString(NULL, name, kCFStringEncodingASCII));
    CGColorSpaceRef ext, lin, exlin;
    char label[160];

    if (!base) {
        printf("%s null\n", name);
        return;
    }
    probe(name, base);
    ext = CGColorSpaceCreateExtended(base);
    snprintf(label, sizeof label, "%s_ext", name);
    probe(label, ext);
    CGColorSpaceRelease(ext);
    lin = CGColorSpaceCreateLinearized(base);
    snprintf(label, sizeof label, "%s_lin", name);
    probe(label, lin);
    CGColorSpaceRelease(lin);
    exlin = CGColorSpaceCreateExtendedLinearized(base);
    snprintf(label, sizeof label, "%s_exlin", name);
    probe(label, exlin);
    CGColorSpaceRelease(exlin);
    CGColorSpaceRelease(base);
}

int main(int argc, char **argv)
{
    static const CGFloat d50[3] = { 0.96422, 1.00000, 0.82521 };
    static const CGFloat d65[3] = { 0.95047, 1.00000, 1.08883 };
    static const CGFloat kept[3] = { 0.5, 1.0, 1.0 };
    static const CGFloat black[3] = { 0.05, 0.05, 0.05 };
    static const CGFloat range[4] = { -100, 100, -100, 100 };
    static const CGFloat gamma[3] = { 2.2, 2.2, 2.2 };
    static const CGFloat identity9[9] = { 1,0,0, 0,1,0, 0,0,1 };
    static const CGFloat displayp3[9] = {
        0.51512, 0.24120, 0.16116,
        0.29198, 0.62417, 0.08386,
        0.15710, 0.07059, 0.77217
    };
    unsigned char t3[12], t6[18], tgray[256];
    CGColorSpaceRef dev_gray, dev_rgb, dev_cmyk;
    CGColorSpaceRef cal_gray, cal_p3;
    CGColorSpaceRef lab_d50, lab_d65;
    CGColorSpaceRef srgb;
    CGColorSpaceRef cs;
    int i;

    for (i = 1; i < argc; i++)
        probe_named(argv[i]);

    /* Device spaces, and their extended derivatives. */
    dev_gray = CGColorSpaceCreateDeviceGray();
    dev_rgb = CGColorSpaceCreateDeviceRGB();
    dev_cmyk = CGColorSpaceCreateDeviceCMYK();
    probe("dev_gray", dev_gray);
    probe("dev_rgb", dev_rgb);
    probe("dev_cmyk", dev_cmyk);
    cs = CGColorSpaceCreateExtended(dev_gray);
    probe("ext_dev_gray", cs);
    CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateExtended(dev_rgb);
    probe("ext_dev_rgb", cs);
    CGColorSpaceRelease(cs);

    /* Calibrated gray and RGB, a spread of white points, black points,
       gammas and matrices. */
    cs = CGColorSpaceCreateCalibratedGray(d50, NULL, 2.2);
    probe("cal_gray_d50", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cal_gray = CGColorSpaceCreateCalibratedGray(d65, black, 1.8);
    probe("cal_gray_bp", cal_gray);
    cs = CGColorSpaceCreateCalibratedRGB(displayp3, NULL, NULL, NULL);
    probe("cal_rgb_p3", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cal_p3 = CGColorSpaceCreateCalibratedRGB(d65, NULL, NULL, displayp3);
    probe("cal_rgb_d65", cal_p3);
    cs = CGColorSpaceCreateCalibratedRGB(d50, NULL, NULL, identity9);
    probe("cal_rgb_d50", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateCalibratedRGB(d65, NULL, gamma, identity9);
    probe("cal_rgb_gamma", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateCalibratedRGB(d65, black, NULL, identity9);
    probe("cal_rgb_black", cs);
    if (cs)
        CGColorSpaceRelease(cs);

    /* Linearized derivatives of the calibrated family, and Lab. */
    cs = CGColorSpaceCreateLinearized(cal_gray);
    probe("lin_cal_gray", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateLinearized(cal_p3);
    probe("lin_cal_rgb", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateCalibratedRGB(d65, NULL, NULL, NULL);
    probe("cal_rgb_identity", cs);
    if (cs)
        CGColorSpaceRelease(cs);

    /* Lab: collapsed white points (D50, D65 share the generic 496-byte
       profile), a surviving white point, an explicit black point, an
       explicit range. */
    lab_d50 = CGColorSpaceCreateLab(d50, NULL, NULL);
    probe("lab_d50", lab_d50);
    lab_d65 = CGColorSpaceCreateLab(d65, NULL, NULL);
    probe("lab_d65", lab_d65);
    cs = CGColorSpaceCreateLab(kept, NULL, NULL);
    probe("lab_wp_kept", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateLab(d50, black, NULL);
    probe("lab_bp", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateLab(d50, NULL, range);
    probe("lab_range", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateLab(kept, NULL, range);
    probe("lab_wp_range", cs);
    if (cs)
        CGColorSpaceRelease(cs);

    /* ICC-from-data round trips: reparse the profiles the named SRGB, the
       generic Lab and the generic XYZ spaces carry. */
    srgb = CGColorSpaceCreateWithName(
        CFStringCreateWithCString(NULL, "kCGColorSpaceSRGB",
                                  kCFStringEncodingASCII));
    if (srgb) {
        CFDataRef d = CGColorSpaceCopyICCData(srgb);
        cs = d ? CGColorSpaceCreateWithICCData(d) : NULL;
        probe("icc_srgb", cs);
        if (cs)
            CGColorSpaceRelease(cs);
        if (d)
            CFRelease(d);
        CGColorSpaceRelease(srgb);
    }
    cs = lab_d65 ? (CGColorSpaceRef)NULL : NULL;
    (void)cs;
    cs = CGColorSpaceCreateWithICCData(CGColorSpaceCopyICCData(lab_d50));
    probe("icc_lab", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateWithName(CFStringCreateWithCString(
        NULL, "kCGColorSpaceGenericXYZ", kCFStringEncodingASCII));
    if (cs) {
        CFDataRef d = CGColorSpaceCopyICCData(cs);
        CGColorSpaceRef r = d ? CGColorSpaceCreateWithICCData(d) : NULL;
        probe("icc_xyz", r);
        if (r)
            CGColorSpaceRelease(r);
        if (d)
            CFRelease(d);
        CGColorSpaceRelease(cs);
    }

    /* The same round trip on rebuilt profiles: linearized named and
       calibrated spaces carry a profile that is not a built-in, so reopening
       it must come back with no name. */
    {
        CGColorSpaceRef s2, lin, r;
        CFDataRef d;

        s2 = CGColorSpaceCreateWithName(CFStringCreateWithCString(
            NULL, "kCGColorSpaceSRGB", kCFStringEncodingASCII));
        if (s2) {
            lin = CGColorSpaceCreateLinearized(s2);
            d = lin ? CGColorSpaceCopyICCData(lin) : NULL;
            r = d ? CGColorSpaceCreateWithICCData(d) : NULL;
            probe("icc_srgb_lin", r);
            if (r)
                CGColorSpaceRelease(r);
            if (d)
                CFRelease(d);
            if (lin)
                CGColorSpaceRelease(lin);
            CGColorSpaceRelease(s2);
        }
    }
    {
        CGColorSpaceRef lin, r;
        CFDataRef d;

        lin = CGColorSpaceCreateLinearized(cal_gray);
        d = lin ? CGColorSpaceCopyICCData(lin) : NULL;
        r = d ? CGColorSpaceCreateWithICCData(d) : NULL;
        probe("icc_cal_lin", r);
        if (r)
            CGColorSpaceRelease(r);
        if (d)
            CFRelease(d);
        if (lin)
            CGColorSpaceRelease(lin);
    }

    /* Broken inputs: a stream too short to carry the 128-byte profile header
       and an empty one both leave nothing to probe. */
    if (srgb) {
        CFDataRef bd = CGColorSpaceCopyICCData(srgb);
        if (bd) {
            size_t n = CFDataGetLength(bd);
            CFDataRef t = CFDataCreate(kCFAllocatorDefault,
                                       CFDataGetBytePtr(bd), n < 12 ? n : 12);
            CGColorSpaceRef r = t ? CGColorSpaceCreateWithICCData(t) : NULL;

            probe("icc_trunc", r);
            if (r)
                CGColorSpaceRelease(r);
            if (t)
                CFRelease(t);
            CFRelease(bd);
        }
    }
    {
        CFDataRef ed = CFDataCreate(kCFAllocatorDefault, (const uint8_t *)"", 0);
        cs = ed ? CGColorSpaceCreateWithICCData(ed) : NULL;
        probe("icc_empty", cs);
        if (cs)
            CGColorSpaceRelease(cs);
        if (ed)
            CFRelease(ed);
    }

    /* Uncolored and colored pattern spaces. */
    cs = CGColorSpaceCreatePattern(NULL);
    probe("pat_null", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreatePattern(dev_rgb);
    probe("pat_rgb", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreatePattern(cal_gray);
    probe("pat_gray", cs);
    if (cs)
        CGColorSpaceRelease(cs);

    /* Indexed spaces: RGB base, gray base with a full 256-entry table, and a
       calibratable RGB base. */
    for (i = 0; i < 12; i++)
        t3[i] = (unsigned char)(i * 17);
    for (i = 0; i < 18; i++)
        t6[i] = (unsigned char)(i * 13);
    for (i = 0; i < 256; i++)
        tgray[i] = (unsigned char)i;
    cs = CGColorSpaceCreateIndexed(dev_rgb, 3, t3);
    probe("idx_rgb", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateIndexed(cal_gray, 255, tgray);
    probe("idx_gray", cs);
    if (cs)
        CGColorSpaceRelease(cs);
    cs = CGColorSpaceCreateIndexed(cal_p3, 5, t6);
    probe("idx_p3", cs);
    if (cs)
        CGColorSpaceRelease(cs);

    CGColorSpaceRelease(dev_gray);
    CGColorSpaceRelease(dev_rgb);
    CGColorSpaceRelease(dev_cmyk);
    if (cal_gray)
        CGColorSpaceRelease(cal_gray);
    if (cal_p3)
        CGColorSpaceRelease(cal_p3);
    if (lab_d50)
        CGColorSpaceRelease(lab_d50);
    if (lab_d65)
        CGColorSpaceRelease(lab_d65);
    return 0;
}