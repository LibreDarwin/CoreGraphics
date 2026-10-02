/* CoreGraphics - CGColorSpace.c
   Copyright (C) 2026, LibreDarwin
   SPDX-License-Identifier: BSD-3-Clause

   Color spaces: the three device spaces, pattern spaces, the accessors over
   them, and the built-in name/ID table.

   See CGColorSpace.h for what is deliberately left out and why.

   Behaviour here is transcribed from the disassembly in
   local/disasm/CoreGraphics and checked against the running framework by
   tests/geometry-parity.c.  The three points worth stating up front:

   - The device spaces are process-lifetime singletons.  In the
     disassembly each Create function is a dispatch_once over a
     `_color_space_state_create_device_*' constructor whose result is
     retained into a file-scope slot, and the accessor is a plain load of
     that slot.  So the objects are never destroyed and every Create
     returns the same pointer.  A refcount that could reach zero would be
     observably wrong, so these use the immortal flag instead.

   - CGColorSpaceGetTypeID is a constant 73, not a registered CoreFoundation
     type identifier.  It names the CF class, so it is 73 for every space
     regardless of model.  Registering with CoreFoundation instead would
     hand back whatever identifier the CF runtime had free, which is not 73.

   - Several accessors crash in Apple rather than returning an error:
     CGColorSpaceGetIdentifier and CGColorSpaceGetMD5Digest fault on a
     device space, and CGColorSpaceGetNames and CGColorSpaceGetColorants
     abort the process on anything that is not a DeviceN space.  Those are
     not reproduced.  The first two return NULL here, and the last two
     return NULL, which is the answer Apple would give if it checked the
     space type before asserting.  The parity harness cannot compare against
     a crash, so these are exercised only for the fact that they do not
     fault. */

#include "CGColorSpace.h"
#include "CGSPI.h"
#include "CGInternal.h"

#include <stdlib.h>
#include <string.h>
/* fma() and isnan() for the Bradford computation in the calibrated RGB
   builder.  The colorants are sensitive to the order the fused multiply-adds
   are issued in, so fma is used deliberately rather than spelling out a
   multiply and an add and relying on the compiler to fuse them. */
#include <math.h>
#include <time.h>

/* The CF class identifier every color space reports.  Read out of Apple by
   calling CGColorSpaceGetTypeID on a device gray, a device RGB, a device
   CMYK, a pattern space and a pattern space with no base: all five report
   73. */
#define CG_COLORSPACE_TYPE_ID 73

/* CGColorSpaceGetType's values.  This tag is finer-grained than
   CGColorSpaceModel: a pattern space reports 9 whatever its base says,
   which the model could not express.  Probed on all five spaces we
   implement; the device spaces report 0, 1 and 2 in that order, a Lab
   space reports 5, and both pattern spaces report 9. */
enum {
    CGColorSpaceTypeMonochrome = 0,
    CGColorSpaceTypeRGB = 1,
    CGColorSpaceTypeCMYK = 2,
    CGColorSpaceTypeLab = 5,
    CGColorSpaceTypePattern = 9
};

struct CGColorSpace {
    /* Saturating reference count, or 0 for an immortal space.  Callers hold
       it via CGColorSpaceRef. */
    long refcount;
    /* Set for the three device spaces, which are never destroyed. */
    bool immortal;
    /* What CGColorSpaceGetModel and CGColorSpaceGetNumberOfComponents
       report. */
    CGColorSpaceModel model;
    /* What CGColorSpaceGetType reports. */
    int type;
    size_t ncomp;
    /* The space's name, as UTF-8.  NULL means the space has no name; the
       accessors turn that into a NULL CFStringRef rather than inventing
       one. */
    const char *name;
    /* The base of a pattern space, retained.  NULL for the device spaces
       and for a pattern space built with a NULL base. */
    struct CGColorSpace *base;
    /* The space's ICC profile, owned, or NULL.  Every space in this step
       except the calibrated ones has none, which is what CGColorSpaceCopyICCData
       reports for them. */
    unsigned char *profile;
    size_t profileLen;
    /* A Lab space's output range, and whether the caller supplied one.  Apple
       keeps this beside the profile rather than inside it: two spaces with
       byte-identical profiles are reported unequal when one was built with a
       range and the other was not.  A NULL range is not a range of zeros but
       the absence of one, which is why the flag is needed to tell them
       apart. */
    bool hasRange;
    CGFloat range[4];
};


/* The three device spaces.

   `name' is the ASCII form of the constant CFString Apple hands back.  It
   is spelled with the kCGColorSpace prefix that the name itself carries,
   i.e. the device RGB space is named "kCGColorSpaceDeviceRGB", not
   "Device RGB". */
static struct CGColorSpace CGColorSpaceDeviceGrayState = {
    0, true, kCGColorSpaceModelMonochrome, CGColorSpaceTypeMonochrome, 1,
    "kCGColorSpaceDeviceGray", NULL, NULL, 0, false, { 0, 0, 0, 0 }
};

static struct CGColorSpace CGColorSpaceDeviceRGBState = {
    0, true, kCGColorSpaceModelRGB, CGColorSpaceTypeRGB, 3,
    "kCGColorSpaceDeviceRGB", NULL, NULL, 0, false, { 0, 0, 0, 0 }
};

static struct CGColorSpace CGColorSpaceDeviceCMYKState = {
    0, true, kCGColorSpaceModelCMYK, CGColorSpaceTypeCMYK, 4,
    "kCGColorSpaceDeviceCMYK", NULL, NULL, 0, false, { 0, 0, 0, 0 }
};

/* The name a pattern space reports when it has no base.  A pattern space
   built on a base has no name of its own, so this is the only pattern name
   in this step. */
#define CG_PATTERN_NAME_WITHOUT_BASE "kCGColorSpaceColoredPattern"

/* The built-in name/ID table, in identifier order.  Identifier 0 is not in
   the table, which is why CGColorSpaceIDFromName answers 0 both for a known
   name and for an unknown one; a caller distinguishes the two by whether
   the name is one of the entries below.

   Extracted by sweeping CGColorSpaceNameFromID over 0..512 and reading back
   the identifier for each name it returned: ids 1 to 32 are present, 0 and
   33 and above are not, and all 32 round-trip exactly.  The comparison
   CGColorSpaceIDFromName does is an exact string match, not a prefix and
   not a case-insensitive one: "kCGColorSpaceSRGB" resolves, while "sRGB",
   "CGColorSpace sRGB", "P3" and the empty string do not, and neither do the
   three device space names. */
static const char *const CGColorSpaceBuiltInNames[] = {
    NULL,                               /* 0: not a valid identifier */
    "kCGColorSpaceGenericGrayGamma2_2",  /* 1 */
    "kCGColorSpaceExtendedGray",         /* 2 */
    "kCGColorSpaceLinearGray",           /* 3 */
    "kCGColorSpaceExtendedLinearGray",   /* 4 */
    "kCGColorSpaceGenericLab",           /* 5 */
    "kCGColorSpaceGenericXYZ",           /* 6 */
    "kCGColorSpaceDisplayP3",            /* 7 */
    "kCGColorSpaceExtendedDisplayP3",    /* 8 */
    "kCGColorSpaceLinearDisplayP3",      /* 9 */
    "kCGColorSpaceExtendedLinearDisplayP3", /* 10 */
    "kCGColorSpaceDisplayP3_PQ",         /* 11 */
    "kCGColorSpaceDisplayP3_HLG",        /* 12 */
    "kCGColorSpaceDisplayP3_709OETF",    /* 13 */
    "kCGColorSpaceAdobeRGB1998",         /* 14 */
    "kCGColorSpaceSRGB",                 /* 15 */
    "kCGColorSpaceExtendedSRGB",         /* 16 */
    "kCGColorSpaceLinearSRGB",           /* 17 */
    "kCGColorSpaceExtendedLinearSRGB",   /* 18 */
    "kCGColorSpaceACESCGLinear",         /* 19 */
    "kCGColorSpaceITUR_709",             /* 20 */
    "kCGColorSpaceITUR_709_PQ",          /* 21 */
    "kCGColorSpaceITUR_709_HLG",         /* 22 */
    "kCGColorSpaceITUR_2020",            /* 23 */
    "kCGColorSpaceLinearITUR_2020",      /* 24 */
    "kCGColorSpaceExtendedITUR_2020",    /* 25 */
    "kCGColorSpaceExtendedLinearITUR_2020", /* 26 */
    "kCGColorSpaceITUR_2020_sRGBGamma",  /* 27 */
    "kCGColorSpaceITUR_2100_PQ",         /* 28 */
    "kCGColorSpaceITUR_2100_HLG",        /* 29 */
    "kCGColorSpaceROMMRGB",              /* 30 */
    "kCGColorSpaceDCIP3",                /* 31 */
    "kCGColorSpaceCoreMedia709"          /* 32 */
};

#define CG_COLORSPACE_BUILT_IN_COUNT \
    ((int)(sizeof CGColorSpaceBuiltInNames / sizeof CGColorSpaceBuiltInNames[0]))

/* Compare a CFString against an ASCII table entry.  The table is ASCII, so
   this builds the CFString and does a byte compare rather than going
   through CFStringCompare, which would be a locale-sensitive Unicode
   collation and so would accept names Apple's exact match rejects. */
static bool CGColorSpaceNameEqualsASCII(CFStringRef name, const char *ascii)
{
    char buf[64];
    size_t n = strlen(ascii);

    if (n >= sizeof buf)
        return false;
    if (!name)
        return false;
    /* Early-out on length so a longer name is rejected before it is copied
       into the buffer. */
    if (CFStringGetLength(name) != (CFIndex)n)
        return false;
    if (!CFStringGetCString(name, buf, (CFIndex)sizeof buf,
            kCFStringEncodingUTF8))
        return false;
    return memcmp(buf, ascii, n) == 0;
}

/* The space's name, or NULL.

   These are CFSTR constants rather than strings built per call, because that
   is what Apple does: CGColorSpaceGetName and CGColorSpaceCopyName hand back
   the same pointer on every call, and CFGetRetainCount on it is CF's
   immortal marker (0x0fffffffffffffff).  Returning a constant is also what
   makes it safe for a caller to release the result of CopyName, since
   releasing an immortal string is a no-op rather than a free. */
static CFStringRef CGColorSpaceNameFor(struct CGColorSpace *s)
{
    if (!s)
        return NULL;
    if (s->type == CGColorSpaceTypePattern) {
        /* A pattern space is named for having no base, not for its base's
           name: a pattern space built on device RGB reports no name even
           though device RGB has one. */
        return s->base ? NULL : CFSTR(CG_PATTERN_NAME_WITHOUT_BASE);
    }
    if (!s->name)
        return NULL;
    /* The device spaces are the file-scope singletons, so they are named by
       identity rather than by building a string.  Returning the constant
       here is what keeps GetName and CopyName returning the same pointer, and
       keeps CFGetRetainCount on it at the immortal marker. */
    if (s == &CGColorSpaceDeviceRGBState)
        return CFSTR("kCGColorSpaceDeviceRGB");
    if (s == &CGColorSpaceDeviceGrayState)
        return CFSTR("kCGColorSpaceDeviceGray");
    if (s == &CGColorSpaceDeviceCMYKState)
        return CFSTR("kCGColorSpaceDeviceCMYK");
    return CFStringCreateWithCString(kCFAllocatorDefault, s->name,
        kCFStringEncodingUTF8);
}

/* Device spaces. */

CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void)
{
    /* The disassembly loads the file-scope slot and returns it, so every
       call hands back the same pointer. */
    return &CGColorSpaceDeviceRGBState;
}

CGColorSpaceRef CGColorSpaceCreateDeviceGray(void)
{
    return &CGColorSpaceDeviceGrayState;
}

CGColorSpaceRef CGColorSpaceCreateDeviceCMYK(void)
{
    return &CGColorSpaceDeviceCMYKState;
}

/* A pattern space is the only reference-counted space in this step, so this
   is where the allocator is used. */
CGColorSpaceRef CGColorSpaceCreatePattern(CGColorSpaceRef baseSpace)
{
    struct CGColorSpace *s = calloc(1, sizeof *s);
    struct CGColorSpace *base = baseSpace;

    if (!s)
        return NULL;
    s->immortal = false;
    s->refcount = 1;
    s->model = kCGColorSpaceModelPattern;
    s->type = CGColorSpaceTypePattern;
    /* The component count and the process model both come from the base, so
       a pattern space with no base has zero components and an unknown
       process model. */
    s->ncomp = base ? base->ncomp : 0;
    s->base = base;
    if (base)
        CGColorSpaceRetain(baseSpace);
    return s;
}

/* Offsets into the gray template, named so the patch sites read as what they
   are rather than as magic numbers. */
enum {
    CGICCProfileIDOffset = 84,          /* 16 bytes */
    CGICCProfileIDLength = 16,
    CGICCFlagsOffset = 44,              /* 4 bytes, zeroed for the digest */
    CGICCFlagsLength = 4,
    /* Each is a whole 20-byte XYZType -- 'XYZ ', four reserved bytes, then
       three s15Fixed16 -- so the offset is where the tag starts, not where
       its first coordinate does. */
    CGICCGrayWTptOffset = 324,
    CGICCGrayBKptOffset = 344,
    /* The gamma itself, four bytes into the 16-byte tone curve. */
    CGICCGrayGammaOffset = 376,
    CGICCGrayLength = 380
};

    /* Reference: white point D50, black point 0, gamma 2.2.
       Bytes 332-343 wtpt, 352-363 bkpt, 376-379 kTRC gamma and
       84-99 the profile ID are replaced per call. */
    static const unsigned char template[CGICCGrayLength] = {
        0x00,  0x00,  0x01,  0x7c,  0x61,  0x70,  0x70,  0x6c,  0x04,  0x00,  0x00,  0x00,
        0x6d,  0x6e,  0x74,  0x72,  0x47,  0x52,  0x41,  0x59,  0x58,  0x59,  0x5a,  0x20,
        0x07,  0xdf,  0x00,  0x01,  0x00,  0x01,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
        0x61,  0x63,  0x73,  0x70,  0x41,  0x50,  0x50,  0x4c,  0x00,  0x00,  0x00,  0x00,
        0x41,  0x50,  0x50,  0x4c,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
        0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0xf6,  0xd6,
        0x00,  0x01,  0x00,  0x00,  0x00,  0x00,  0xd3,  0x2d,  0x61,  0x70,  0x70,  0x6c,
        0x09,  0x88,  0x00,  0x82,  0x3c,  0x71,  0x20,  0xce,  0xbc,  0x9c,  0x19,  0x1d,
        0x65,  0xa2,  0xa3,  0x47,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
        0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
        0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x05,
        0x64,  0x65,  0x73,  0x63,  0x00,  0x00,  0x00,  0xc0,  0x00,  0x00,  0x00,  0x32,
        0x63,  0x70,  0x72,  0x74,  0x00,  0x00,  0x00,  0xf4,  0x00,  0x00,  0x00,  0x50,
        0x77,  0x74,  0x70,  0x74,  0x00,  0x00,  0x01,  0x44,  0x00,  0x00,  0x00,  0x14,
        0x62,  0x6b,  0x70,  0x74,  0x00,  0x00,  0x01,  0x58,  0x00,  0x00,  0x00,  0x14,
        0x6b,  0x54,  0x52,  0x43,  0x00,  0x00,  0x01,  0x6c,  0x00,  0x00,  0x00,  0x10,
        0x6d,  0x6c,  0x75,  0x63,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x01,
        0x00,  0x00,  0x00,  0x0c,  0x65,  0x6e,  0x55,  0x53,  0x00,  0x00,  0x00,  0x16,
        0x00,  0x00,  0x00,  0x1c,  0x00,  0x43,  0x00,  0x47,  0x00,  0x20,  0x00,  0x43,
        0x00,  0x61,  0x00,  0x6c,  0x00,  0x20,  0x00,  0x47,  0x00,  0x72,  0x00,  0x61,
        0x00,  0x79,  0x00,  0x00,  0x6d,  0x6c,  0x75,  0x63,  0x00,  0x00,  0x00,  0x00,
        0x00,  0x00,  0x00,  0x01,  0x00,  0x00,  0x00,  0x0c,  0x65,  0x6e,  0x55,  0x53,
        0x00,  0x00,  0x00,  0x34,  0x00,  0x00,  0x00,  0x1c,  0x00,  0x43,  0x00,  0x6f,
        0x00,  0x70,  0x00,  0x79,  0x00,  0x72,  0x00,  0x69,  0x00,  0x67,  0x00,  0x68,
        0x00,  0x74,  0x00,  0x20,  0x00,  0x41,  0x00,  0x70,  0x00,  0x70,  0x00,  0x6c,
        0x00,  0x65,  0x00,  0x20,  0x00,  0x49,  0x00,  0x6e,  0x00,  0x63,  0x00,  0x2e,
        0x00,  0x2c,  0x00,  0x20,  0x00,  0x32,  0x00,  0x30,  0x00,  0x31,  0x00,  0x35,
        0x58,  0x59,  0x5a,  0x20,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0xf3,  0x54,
        0x00,  0x01,  0x00,  0x00,  0x00,  0x01,  0x16,  0xc9,  0x58,  0x59,  0x5a,  0x20,
        0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
        0x00,  0x00,  0x00,  0x00,  0x70,  0x61,  0x72,  0x61,  0x00,  0x00,  0x00,  0x00,
        0x00,  0x00,  0x00,  0x00,  0x00,  0x02,  0x33,  0x33, 
    };

/* Calibrated spaces: the ICC profile synthesis of 2b.

   Apple's calibrated spaces do not compute a profile, they emit a fixed
   template with a handful of fields filled in.  The gray template below was
   taken from Apple's own output for white point D50, black point 0 and gamma
   2.2 rather than transcribed, and a sweep over each parameter establishes
   that exactly four regions of the 380 bytes ever change:

     332-343  wtpt, the white point as three s15Fixed16
     352-363  bkpt, the black point likewise
     376-379  kTRC, the gamma as a u8Fixed8
       84-99  the profile ID, which is the MD5 of the finished profile

   Everything else -- the header, the tag table, the two mluc records, the
   creation date and the copyright -- is constant.  The last tag ends exactly
   at byte 380, so there is no trailing padding, and the tone curve occupies
   tag offset 12 rather than 10: the curve is padded so that its u8Fixed8
   lands on a 4-byte boundary.  Both of those are easy to get wrong and both
   are checked by the parity harness comparing whole profiles. */

/* The value of an s15Fixed16 field for `v'.  Apple quantises the caller's
   double to 16.16 with round-to-nearest, which is observable: 2.2 becomes
   0x00023333 rather than the 0x00023334 that truncation would give, and
   0.1 becomes 0x199a rather than 0x1999.

   The value is narrowed to float first, which is what Apple does on the way
   in.  That is not cosmetic here either: 0.5881576452780182 scaled in double
   lands on 38545.4994 and truncates to 0x9691, while the float32 of it lands
   on exactly 38545.5 and rounds to 0x9692.  Any white point or black point
   whose double scaled value falls just under a half-unit boundary lands one
   unit low without the narrowing. */
static void put_s15Fixed16(unsigned char *p, CGFloat v)
{
    double scaled = (double)(float)v * 65536.0;
    long q;

    if (!(scaled > -2147483648.0 && scaled < 2147483648.0)) {
        /* Out of range: clamp rather than invoke undefined behaviour. */
        q = scaled > 0.0 ? 2147483647L : -2147483648L;
    } else {
        /* Add a half and truncate toward zero, for negative inputs as well
           as positive.  That is round-half-up, not round-half-away-from-zero,
           and the difference is observable: -0.2 quantises to 0xFFFCCE here
           but to 0xFFFCCD under the symmetric rule.  Apple's negative
           components come out one larger than the symmetric rule would give,
           which is what pins this down. */
        q = (long)(scaled + 0.5);
    }
    p[0] = (unsigned char)(q >> 24);
    p[1] = (unsigned char)(q >> 16);
    p[2] = (unsigned char)(q >> 8);
    p[3] = (unsigned char)q;
}

/* Write three s15Fixed16 values after an 'XYZ ' tag. */
static void put_xyz(unsigned char *p, const CGFloat v[3])
{
    memcpy(p, "XYZ ", 4);
    memset(p + 4, 0, 4);
    put_s15Fixed16(p + 8, v[0]);
    put_s15Fixed16(p + 12, v[1]);
    put_s15Fixed16(p + 16, v[2]);
}

/* The 16-byte tone curve: 'para', six reserved bytes, then gamma as a
   u8Fixed8.  Apple declares the tag as 16 bytes although the parametric
   curve is 14, so the last two are padding.

   Gamma is narrowed to float before being scaled, for the same reason the
   s15Fixed16 fields are, and a gamma that is not positive becomes zero
   rather than saturating -- 0.0 and a negative value both give 0. */
static void put_tone_curve(unsigned char *p, CGFloat gamma)
{
    long g;

    memcpy(p, "para", 4);
    memset(p + 4, 0, 8);
    if (!(gamma > 0.0)) {
        g = 0;
    } else {
        double scaled = (double)(float)gamma * 65536.0;

        g = scaled < 4294967295.0 ? (long)(scaled + 0.5) : 4294967295L;
    }
    p[12] = (unsigned char)(g >> 24);
    p[13] = (unsigned char)(g >> 16);
    p[14] = (unsigned char)(g >> 8);
    p[15] = (unsigned char)g;
}

/* Stamp the profile ID, which is the MD5 of the profile with the flags and
   the ID field itself zeroed.  Lab is the exception in Apple's output -- it
   leaves the ID all zeros -- so this is not something a caller can assume
   for every space. */
static void put_profile_id(unsigned char *p, size_t len)
{
    unsigned char saved_flags[CGICCFlagsLength];
    unsigned char id[CGICCProfileIDLength];

    memcpy(saved_flags, p + CGICCFlagsOffset, CGICCFlagsLength);
    memset(p + CGICCFlagsOffset, 0, CGICCFlagsLength);
    memset(p + CGICCProfileIDOffset, 0, CGICCProfileIDLength);
    CGMD5(p, len, id);
    memcpy(p + CGICCFlagsOffset, saved_flags, CGICCFlagsLength);
    memcpy(p + CGICCProfileIDOffset, id, CGICCProfileIDLength);
}

CGColorSpaceRef CGColorSpaceCreateCalibratedGray(const CGFloat
    whitePoint[CG_NONNULL_ARRAY 3], const CGFloat blackPoint[__nullable 3],
    CGFloat gamma)
{
    static const CGFloat zero[3] = { 0.0, 0.0, 0.0 };
    struct CGColorSpace *s;
    unsigned char *p;

    /* Apple's signature marks the white point nonnull, so a NULL there is
       a caller error rather than a request for a default; checking it only
       keeps the dereference honest. */
    if (!whitePoint)
        return NULL;
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    p = malloc(CGICCGrayLength);
    if (!p) {
        free(s);
        return NULL;
    }
    memcpy(p, template, CGICCGrayLength);

    put_xyz(p + CGICCGrayWTptOffset, whitePoint);
    put_xyz(p + CGICCGrayBKptOffset, blackPoint ? blackPoint : zero);
    put_tone_curve(p + CGICCGrayGammaOffset - 12, gamma);
    put_profile_id(p, CGICCGrayLength);

    s->immortal = false;
    s->refcount = 1;
    s->model = kCGColorSpaceModelMonochrome;
    s->type = CGColorSpaceTypeMonochrome;
    s->ncomp = 1;
    /* A calibrated space is named for being calibrated, not for its white
       point, and the name does not vary with gamma. */
    s->name = NULL;
    s->base = NULL;
    s->profile = p;
    s->profileLen = CGICCGrayLength;
    return s;
}

/* Calibrated RGB.

   This is where the gray template stops being usable.  The profile length
   varies, because Apple's builder allocates one block per *distinct* piece of
   content and lets the tag table point several tags at it, and the colorant
   tags are a real computation rather than a constant.

   The shape is fixed, though.  There is always a 128-byte header, a tag
   count, and a ten-entry tag table; the table ends at 252 and the tag data
   starts there.  The ten entries are always present.  A profile with three
   distinct tone curves and five distinct XYZ values runs to 528 bytes, one
   where all three curves coincide runs to 496, and one where the white point
   quantises to zero -- which makes every colorant zero too -- collapses all
   five XYZ tags onto a single block and runs to 416.

   Both mluc records carry a length field of 28 that does not describe the
   string stored after it: "CG Cal RGB" occupies the last 20 bytes of its
   48-byte block and "Copyright Apple Inc., 2015" the last 52 bytes of its
   80-byte block.  The record offsets are 20 and 52 respectively.  These are
   transcribed from Apple's output and are simply reproduced; the same
   mismatch is in the gray template, so it is Apple's convention and not a
   transcription error. */
enum {
    CGICCRGBTagCount = 10,
    CGICCRGBTagTableOffset = 132,
    CGICCRGBDataOffset = 252,
    CGICCRGBDescLength = 48,
    CGICCRGBCprtLength = 80,
    /* Both are whole tag types: an XYZType is 'XYZ ' plus four reserved bytes
       plus three s15Fixed16, and the tone curve is declared 16 bytes wide
       although the parametric curve in it is 14. */
    CGICCRGBXYZLength = 20,
    CGICCRGBTRCLength = 16
};

/*    Reference: white point (1,1,1), black point 0, gamma 2.2 on all three
   components and the identity matrix, which is the profile with all three
   tone curves and all five XYZ values distinct.  The size, the tag table and
   the profile ID are the only fields that change between calls. */
    static const unsigned char rgb_header[128] = {
        0x00,  0x00,  0x01,  0xf0,  0x61,  0x70,  0x70,  0x6c,  0x04,  0x00,  0x00,  0x00,
        0x6d,  0x6e,  0x74,  0x72,  0x52,  0x47,  0x42,  0x20,  0x58,  0x59,  0x5a,  0x20,
        0x07,  0xdf,  0x00,  0x01,  0x00,  0x01,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
        0x61,  0x63,  0x73,  0x70,  0x41,  0x50,  0x50,  0x4c,  0x00,  0x00,  0x00,  0x00,
        0x41,  0x50,  0x50,  0x4c,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
        0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0xf6,  0xd6,
        0x00,  0x01,  0x00,  0x00,  0x00,  0x00,  0xd3,  0x2d,  0x61,  0x70,  0x70,  0x6c,
        0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
        0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
        0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
        0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
    };
    static const unsigned char rgb_desc[CGICCRGBDescLength] = {
        0x6d,  0x6c,  0x75,  0x63,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x01,
        0x00,  0x00,  0x00,  0x0c,  0x65,  0x6e,  0x55,  0x53,  0x00,  0x00,  0x00,  0x14,
        0x00,  0x00,  0x00,  0x1c,  0x00,  0x43,  0x00,  0x47,  0x00,  0x20,  0x00,  0x43,
        0x00,  0x61,  0x00,  0x6c,  0x00,  0x20,  0x00,  0x52,  0x00,  0x47,  0x00,  0x42,
    };
    static const unsigned char rgb_cprt[CGICCRGBCprtLength] = {
        0x6d,  0x6c,  0x75,  0x63,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x01,
        0x00,  0x00,  0x00,  0x0c,  0x65,  0x6e,  0x55,  0x53,  0x00,  0x00,  0x00,  0x34,
        0x00,  0x00,  0x00,  0x1c,  0x00,  0x43,  0x00,  0x6f,  0x00,  0x70,  0x00,  0x79,
        0x00,  0x72,  0x00,  0x69,  0x00,  0x67,  0x00,  0x68,  0x00,  0x74,  0x00,  0x20,
        0x00,  0x41,  0x00,  0x70,  0x00,  0x70,  0x00,  0x6c,  0x00,  0x65,  0x00,  0x20,
        0x00,  0x49,  0x00,  0x6e,  0x00,  0x63,  0x00,  0x2e,  0x00,  0x2c,  0x00,  0x20,
        0x00,  0x32,  0x00,  0x30,  0x00,  0x31,  0x00,  0x35,
    };

/* The Bradford cone response and its inverse, as they appear in ColorSync.
   The inverse is not the matrix inverse computed in double precision: its
   coefficients differ in the seventh decimal place, and that difference is
   several quantisation units at the 16.16 resolution the colorants are
   stored at. */
static const double CGICCBradford[9] = {
    0.8951, 0.2664, -0.1614,
   -0.7502, 1.7135,  0.0367,
    0.0389, -0.0685, 1.0296
};
static const double CGICCBradfordInv[9] = {
    0.9869930, -0.1470540, 0.1599630,
    0.4323050,  0.5183600, 0.0492910,
   -0.0085290,  0.0400430, 0.9684870
};
static const double CGICCD50[3] = { 0.9642, 1.0, 0.8249 };

/* `o' = `v' times `m'.  Every one of these dot products, and the colorant dot
   products below, accumulates in the order 1, 0, 2 rather than 0, 1, 2, and
   the difference is observable in the last quantisation unit.  The order is
   Apple's, so it is reproduced rather than improved on. */
static void cg_vector_matrix(const double v[3], const double m[9], double o[3])
{
    int i;

    for (i = 0; i < 3; i++) {
        double acc = v[1] * m[3 * i + 1];
        acc = fma(v[0], m[3 * i], acc);
        acc = fma(v[2], m[3 * i + 2], acc);
        o[i] = acc;
    }
}

/* The same product for a vector the caller supplied, which has been through
   float first.  Accumulating in double from the float values is exactly what
   widening on entry to the double routine would have done. */
static void cg_vector_matrix_f(const float v[3], const double m[9], double o[3])
{
    double wide[3];
    int i;

    for (i = 0; i < 3; i++)
        wide[i] = v[i];
    cg_vector_matrix(wide, m, o);
}

/* `c' = `b' times `a'.  The argument order is the reverse of the mathematical
   one, which is how ColorSync spells it. */
static void cg_matrix_matrix(const double a[9], const double b[9], double c[9])
{
    int i, k;

    for (i = 0; i < 3; i++)
        for (k = 0; k < 3; k++) {
            double acc = b[3 * i + 1] * a[3 * 1 + k];
            acc = fma(b[3 * i], a[3 * 0 + k], acc);
            acc = fma(b[3 * i + 2], a[3 * 2 + k], acc);
            c[3 * i + k] = acc;
        }
}

/* Truncate toward zero into an int32, saturating rather than trapping, and
   mapping NaN to zero.  That is what the hardware conversion does, and it is
   load-bearing here: a white point of zeros divides by zero on the way to the
   colorants, the infinities that come back out subtract to NaN, and Apple
   stores zero for those colorants rather than a saturated bound. */
static int32_t cg_trunc_s32(double v)
{
    if (isnan(v))
        return 0;
    if (v >= 2147483648.0)
        return INT32_MAX;
    if (v <= -2147483648.0)
        return INT32_MIN;
    return (int32_t)v;
}

/* One colorant component: row `i' of the caller's matrix dotted with row `k'
   of the adapted matrix `a'.  Quantised to s15Fixed16. */
static int32_t cg_colorant(const float m[9], const double a[9], int i, int k)
{
    double acc = (double)m[3 * i + 1] * a[3 * k + 1];
    int32_t q;

    acc = fma((double)m[3 * i], a[3 * k], acc);
    acc = fma((double)m[3 * i + 2], a[3 * k + 2], acc);
    q = cg_trunc_s32(acc * 65536.0 + 0.5);
    /* Y is a luminance and is held to [0, 1]; X and Z get no such clamp and
       are whatever the saturated conversion above produced. */
    if (k == 1) {
        if (q < 0)
            q = 0;
        else if (q > 65536)
            q = 65536;
    }
    return q;
}

/* The Bradford adaptation of `m' onto `whitePoint', as the colorant matrix.

   The construction is: scale the white point into Bradford space, derive the
   per-axis gains that carry it to D50, apply those gains to the matrix, and
   invert.  Note that the gains are applied to the matrix *before* the
   inversion, not to the result.

   Both caller-supplied arrays go through float first, which is what Apple
   does with its conversion instructions and what the parameters would have
   been anyway had CGFloat been float.  The rounding is not cosmetic: carrying
   the white point or the matrix at full double precision instead gets the
   last quantisation unit of many colorants wrong.  D50 and the two Bradford
   matrices stay double throughout -- it is only the caller's values that are
   narrowed. */
static void cg_colorants(const CGFloat whitePoint[3], const CGFloat m[9],
    int32_t out[9])
{
    float wp[3], mat[9];
    double cone_d50[3], cone_white[3], gains[9], scaled[9], result[9];
    int i, k;

    for (i = 0; i < 3; i++)
        wp[i] = (float)whitePoint[i];
    for (i = 0; i < 9; i++)
        mat[i] = (float)m[i];
    cg_vector_matrix(CGICCD50, CGICCBradford, cone_d50);
    cg_vector_matrix_f(wp, CGICCBradford, cone_white);
    memset(gains, 0, sizeof gains);
    for (i = 0; i < 3; i++)
        gains[4 * i] = cone_d50[i] / cone_white[i];
    /* result = BIh * (BR * diag(gains)) */
    cg_matrix_matrix(CGICCBradford, gains, scaled);
    cg_matrix_matrix(scaled, CGICCBradfordInv, result);
    /* The colorant row is the caller's matrix row dotted with a row of the
       adapted matrix -- that is the transpose Apple's matrix performs. */
    for (i = 0; i < 3; i++)
        for (k = 0; k < 3; k++)
            out[3 * i + k] = cg_colorant(mat, result, i, k);
}

static void put_be32(unsigned char *p, int32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* Write three already-quantised s15Fixed16 values after an 'XYZ ' tag. */
static void put_xyz_i32(unsigned char *p, const int32_t v[3])
{
    memcpy(p, "XYZ ", 4);
    memset(p + 4, 0, 4);
    put_be32(p + 8, v[0]);
    put_be32(p + 12, v[1]);
    put_be32(p + 16, v[2]);
}

CGColorSpaceRef CGColorSpaceCreateCalibratedRGB(const CGFloat
    whitePoint[CG_NONNULL_ARRAY 3], const CGFloat blackPoint[__nullable 3],
    const CGFloat gamma[__nullable 3], const CGFloat matrix[__nullable 9])
{
    /* The tags, in the order they are first emitted.  The two mluc records
       are always first, then the five XYZ tags in the order named here, then
       the three tone curves -- and note that the curves are red, blue, green
       rather than red, green, blue, which is both the order their blocks are
       allocated in and the order `gamma' indexes them by. */
    static const char *const tagNames[CGICCRGBTagCount] = {
        "desc", "cprt", "wtpt", "bkpt", "rXYZ", "gXYZ", "bXYZ",
        "rTRC", "bTRC", "gTRC"
    };
    /* Which caller's gamma each tone curve takes, in table order. */
    static const int gammaIndex[3] = { 0, 2, 1 };
    static const CGFloat zero[3] = { 0.0, 0.0, 0.0 };
    static const CGFloat unit[3] = { 1.0, 1.0, 1.0 };
    static const CGFloat identity[9] = {
        1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0
    };
    unsigned char xyz[5][CGICCRGBXYZLength];
    unsigned char trc[3][CGICCRGBTRCLength];
    int32_t tagOff[CGICCRGBTagCount], tagLen[CGICCRGBTagCount];
    int owns[CGICCRGBTagCount];
    size_t len, off;
    struct CGColorSpace *s;
    unsigned char *p;
    int32_t colorants[9];
    int i, k;

    if (!whitePoint)
        return NULL;
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    if (!blackPoint)
        blackPoint = zero;
    if (!gamma)
        gamma = unit;
    if (!matrix)
        matrix = identity;

    cg_colorants(whitePoint, matrix, colorants);

    /* The five XYZ candidates and the three curves, each already in its
       stored form -- it is the stored bytes that get compared for sharing
       below, not the caller's input. */
    put_xyz(xyz[0], whitePoint);
    put_xyz(xyz[1], blackPoint);
    for (i = 0; i < 3; i++)
        put_xyz_i32(xyz[2 + i], &colorants[3 * i]);
    for (i = 0; i < 3; i++)
        put_tone_curve(trc[i], gamma[gammaIndex[i]]);

    tagOff[0] = CGICCRGBDataOffset;
    tagLen[0] = CGICCRGBDescLength;
    tagOff[1] = CGICCRGBDataOffset + CGICCRGBDescLength;
    tagLen[1] = CGICCRGBCprtLength;
    owns[0] = owns[1] = 1;

    /* Walk the remaining eight in build order, giving each block that is not
       already present the next free offset.  A later tag whose stored bytes
       match an earlier one reuses that earlier one's offset and is recorded
       as not owning its block. */
    off = CGICCRGBDataOffset + CGICCRGBDescLength + CGICCRGBCprtLength;
    for (i = 2; i < CGICCRGBTagCount; i++) {
        const unsigned char *bytes = i < 7 ? xyz[i - 2] : trc[i - 7];
        int32_t width = (int32_t)(i < 7 ? CGICCRGBXYZLength
            : CGICCRGBTRCLength);
        int shared = 0;

        for (k = 2; k < i; k++) {
            if (tagLen[k] != width)
                continue;
            if (memcmp(k < 7 ? xyz[k - 2] : trc[k - 7], bytes, (size_t)width))
                continue;
            tagOff[i] = tagOff[k];
            tagLen[i] = width;
            owns[i] = 0;
            shared = 1;
            break;
        }
        if (shared)
            continue;
        tagOff[i] = (int32_t)off;
        tagLen[i] = width;
        owns[i] = 1;
        off += (size_t)width;
    }
    len = off;

    p = malloc(len);
    if (!p) {
        free(s);
        return NULL;
    }
    memset(p, 0, len);
    memcpy(p, rgb_header, sizeof rgb_header);
    put_be32(p, (int32_t)len);
    put_be32(p + 128, CGICCRGBTagCount);
    memcpy(p + CGICCRGBDataOffset, rgb_desc, CGICCRGBDescLength);
    memcpy(p + CGICCRGBDataOffset + CGICCRGBDescLength, rgb_cprt,
        CGICCRGBCprtLength);
    for (i = 2; i < CGICCRGBTagCount; i++)
        if (owns[i])
            memcpy(p + tagOff[i], i < 7 ? xyz[i - 2] : trc[i - 7],
                (size_t)tagLen[i]);

    /* The tag table does not simply follow the order the tags were built in.
       Apple emits the tags that own a block first, in build order, and then
       the tags that share one, also in build order.  With nothing shared the
       two groups are the same list, so the table looks like the obvious
       desc/cprt/wtpt/bkpt/rXYZ/gXYZ/bXYZ/rTRC/bTRC/gTRC -- but a white point
       of zero shares all five XYZ tags, and a repeated gamma shares the
       curves, and then the order visibly changes.  With a zero white point
       and a single shared curve the table comes out desc, cprt, wtpt, rTRC,
       bkpt, rXYZ, gXYZ, bXYZ, bTRC, gTRC. */
    {
        int slot = 0, pass;

        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < CGICCRGBTagCount; i++) {
                unsigned char *entry;

                if (owns[i] != !pass)
                    continue;
                entry = p + CGICCRGBTagTableOffset + slot * 12;
                memcpy(entry, tagNames[i], 4);
                put_be32(entry + 4, tagOff[i]);
                put_be32(entry + 8, tagLen[i]);
                slot++;
            }
        }
    }
    put_profile_id(p, len);

    s->immortal = false;
    s->refcount = 1;
    s->model = kCGColorSpaceModelRGB;
    s->type = CGColorSpaceTypeRGB;
    s->ncomp = 3;
    s->name = NULL;
    s->base = NULL;
    s->profile = p;
    s->profileLen = len;
    return s;
}

/* Lab.

   Apple builds a Lab profile from a fixed template the way it builds the
   calibrated ones, but two of its fields differ in kind from anything in gray
   or RGB.

   The first is the white and black points.  Apple keeps a point only when the
   caller's double is one a float can hold exactly, and discards the whole tag
   otherwise, leaving all three coordinates zero.  That single fact accounts
   for most of what looks strange about this function: D65 and D50 both fail
   the test, since neither 0.95047 nor 1.08883 is a float, so asking for
   either yields a profile byte-for-byte identical to the generic Lab space --
   it is the same synthesised profile, not a bundled resource, which is why
   CGColorSpaceCreateWithName(kCGColorSpaceGenericLab) produces those bytes
   too.  A point like (0.5, 1, 1) survives, because all three coordinates are
   exactly representable, and the profile grows by the 20 bytes its black
   point then needs.  NaN fails the comparison as well and is discarded with
   everything else; an infinity passes and saturates below.

   The stored value is 16.16 fixed point in a full 32 bits, so 1.0 is
   0x00010000 where the s15Fixed16 the gray and RGB spaces use would have
   clamped it.  A coordinate well outside the unit range therefore survives as
   a large number rather than a saturated one.

   The second field is the creation date, which is the local wall clock at the
   moment of the call, to the second.  The gray and RGB templates carry a
   fixed 2015 date; Lab stamps the time, so two calls three seconds apart
   differ in one byte, and a byte-exact comparison has to mask 24-35.

   The rest is constant.  The description is the same "Custom Lab Profile"
   whatever the white point, the two LUTs are a single 124-byte 'mft2' block
   that A2B0 and B2A0 share, and the profile ID is left all zeros -- the
   exception put_profile_id notes above.  `range' is accepted and ignored: a
   sweep over NULL, the all-zero default Apple documents, the extremes of both
   signs and three arbitrary arrays all produced the same bytes.

   The layout is the RGB one.  Blocks are allocated in build order and a later
   tag whose stored bytes match an earlier one reuses that block, so a zero
   white point shares the black point's block and the profile runs to 496
   bytes with the tag table reading desc, cprt, wtpt, A2B0, bkpt, B2A0.  A
   white point that survives gives the black point a block of its own, the
   profile runs to 516, and the table reads in build order. */

/* The creation date is twelve bytes of the shared ICC header at the same
   offset whatever the space, so it is named once here rather than per
   template.  It is also the one field that is live rather than a constant:
   Lab stamps the local time of the call into it. */
enum {
    CGICCCreateDateOffset = 24,
    CGICCCreateDateLength = 12
};

enum {
    CGICCLabTagCount = 6,
    CGICCLabTagTableOffset = 132,
    /* The description runs 204-312 and the copyright 316-350, each starting
       on a 4-byte boundary, which leaves 3 and 1 bytes of padding before
       them and the content starting at 352. */
    CGICCLabDescOffset = 204,
    CGICCLabDescLength = 109,
    CGICCLabCprtOffset = 316,
    CGICCLabCprtLength = 35,
    CGICCLabDataOffset = 352,
    CGICCLabXYZLength = 20,
    CGICCLabLutLength = 124,
    CGICCLabCreateDateOffset = CGICCCreateDateOffset,
    CGICCLabCreateDateLength = CGICCCreateDateLength
};

    /* Reference: white point (1, 1, 1) and no black point, which is the only
       shape that gives every tag a block of its own.
       Bytes 0-3 the profile size and 24-35 the creation date are replaced per
       call; 84-99 the profile ID stays zero. */
    static const unsigned char lab_header[128] = {
        0x00, 0x00, 0x01, 0xf0, 0x61, 0x70, 0x70, 0x6c, 0x02, 0x10, 0x00, 0x00,
        0x73, 0x70, 0x61, 0x63, 0x4c, 0x61, 0x62, 0x20, 0x4c, 0x61, 0x62, 0x20,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x61, 0x63, 0x73, 0x70, 0x41, 0x50, 0x50, 0x4c, 0x00, 0x00, 0x00, 0x00,
        0x41, 0x50, 0x50, 0x4c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf6, 0xd6,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xd3, 0x2d, 0x61, 0x70, 0x70, 0x6c,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    /* A legacy 'desc' record: no version word, the ASCII length at 8, the
       text at 12, then zero fill out to 109. */
    static const unsigned char lab_desc[109] = {
        0x64, 0x65, 0x73, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13,
        0x43, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x20, 0x4c, 0x61, 0x62, 0x20, 0x50,
        0x72, 0x6f, 0x66, 0x69, 0x6c, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00,
    };

    /* A 'text' copyright, the same shape as the gray and RGB records but
       without the trailing bytes those two carry. */
    static const unsigned char lab_cprt[35] = {
        0x74, 0x65, 0x78, 0x74, 0x00, 0x00, 0x00, 0x00, 0x43, 0x6f, 0x70, 0x79,
        0x72, 0x69, 0x67, 0x68, 0x74, 0x20, 0x41, 0x70, 0x70, 0x6c, 0x65, 0x20,
        0x49, 0x6e, 0x63, 0x2e, 0x2c, 0x20, 0x32, 0x30, 0x32, 0x36, 0x00,
    };

    /* One 'mft2' lookup, shared by A2B0 and B2A0: a 3x3x2-entry table, so 9
       lines of 6 channels.  It is the same in every Lab profile regardless of
       the white point, which is what lets a 496-byte profile carry it at all. */
    static const unsigned char lab_lut[124] = {
        0x6d, 0x66, 0x74, 0x32, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03, 0x02, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff,
        0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
        0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff,
        0x00, 0x00, 0xff, 0xff,
    };

static void put_be16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

/* The creation date: six 16-bit big-endian fields, year through second, in
   local time.  Apple stamps the moment of the call, which is the only part of
   a Lab profile that is not a function of its arguments. */
static void put_create_date(unsigned char *p)
{
    time_t now = time(NULL);
    struct tm tmv;

    if (!localtime_r(&now, &tmv))
        return;
    put_be16(p, (unsigned)(tmv.tm_year + 1900));
    put_be16(p + 2, (unsigned)(tmv.tm_mon + 1));
    put_be16(p + 4, (unsigned)tmv.tm_mday);
    put_be16(p + 6, (unsigned)tmv.tm_hour);
    put_be16(p + 8, (unsigned)tmv.tm_min);
    put_be16(p + 10, (unsigned)tmv.tm_sec);
}

/* Whether Apple keeps this point at all.  Every coordinate is narrowed to
   float, and unless all three survive the narrowing unchanged the entire tag
   becomes zero -- one inexpressible coordinate takes the two that are fine
   with it.  NaN fails here for the same reason and is discarded with the
   rest; an infinity passes and then saturates in lab_fixed16.

   This is why (0.5, 1, 1) produces a real white point while D65 produces
   none, and why both 0.5 and 1.0 work while 0.999 and 1.0005 do not: the
   first two are representable as floats and the last two are not. */
static int lab_point_kept(const CGFloat v[3])
{
    int i;

    for (i = 0; i < 3; i++)
        if ((CGFloat)(float)v[i] != v[i])
            return 0;
    return 1;
}

/* One stored XYZ coordinate.  Apple adds a half and truncates toward zero,
   the same idiom as put_s15Fixed16, so halves round up and negative values
   come out one larger than symmetric rounding would give: -0.5 stores
   -32767, not -32768.  Out-of-range results saturate rather than wrap, so an
   infinite coordinate lands on the matching limit.  NaN never arrives: it is
   rejected by lab_point_kept, and comparing it here would be false on both
   sides and fall through to the conversion. */
static int32_t lab_fixed16(CGFloat v)
{
    double scaled = (double)v * 65536.0 + 0.5;

    if (scaled >= 2147483647.0)
        return 2147483647;
    if (scaled <= -2147483648.0)
        return (-2147483647 - 1);
    return (int32_t)scaled;
}

/* A whole white or black point tag, in the bytes sharing is decided on. */
static void lab_xyz(unsigned char *p, const CGFloat v[3])
{
    int32_t q[3];
    int i;

    if (lab_point_kept(v)) {
        for (i = 0; i < 3; i++)
            q[i] = lab_fixed16(v[i]);
    } else {
        q[0] = q[1] = q[2] = 0;
    }
    put_xyz_i32(p, q);
}

CGColorSpaceRef CGColorSpaceCreateLab(const CGFloat
    whitePoint[CG_NONNULL_ARRAY 3], const CGFloat blackPoint[__nullable 3],
    const CGFloat range[__nullable 4])
{
    static const char *const tagNames[CGICCLabTagCount] = {
        "desc", "cprt", "wtpt", "bkpt", "A2B0", "B2A0"
    };
    static const CGFloat zero[3] = { 0.0, 0.0, 0.0 };
    unsigned char xyz[2][CGICCLabXYZLength];
    int32_t tagOff[CGICCLabTagCount], tagLen[CGICCLabTagCount];
    int owns[CGICCLabTagCount];
    size_t len, off;
    struct CGColorSpace *s;
    unsigned char *p;
    int i, k;

    /* Taken and ignored; see the note above. */
    (void)range;

    /* Apple faults on a null white point instead of returning null, so there
       is no behaviour to copy.  Refusing keeps the dereference honest. */
    if (!whitePoint)
        return NULL;
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    if (!blackPoint)
        blackPoint = zero;

    lab_xyz(xyz[0], whitePoint);
    lab_xyz(xyz[1], blackPoint);

    tagOff[0] = CGICCLabDescOffset;
    tagLen[0] = CGICCLabDescLength;
    tagOff[1] = CGICCLabCprtOffset;
    tagLen[1] = CGICCLabCprtLength;
    owns[0] = owns[1] = 1;

    /* The four content tags in build order.  A2B0 and B2A0 are the same bytes,
       so the second of them always ends up sharing. */
    off = CGICCLabDataOffset;
    for (i = 2; i < CGICCLabTagCount; i++) {
        const unsigned char *bytes = i < 4 ? xyz[i - 2] : lab_lut;
        int32_t width = (int32_t)(i < 4 ? CGICCLabXYZLength
            : CGICCLabLutLength);
        int shared = 0;

        for (k = 2; k < i; k++) {
            if (tagLen[k] != width)
                continue;
            if (memcmp(k < 4 ? xyz[k - 2] : lab_lut, bytes, (size_t)width))
                continue;
            tagOff[i] = tagOff[k];
            tagLen[i] = width;
            owns[i] = 0;
            shared = 1;
            break;
        }
        if (shared)
            continue;
        tagOff[i] = (int32_t)off;
        tagLen[i] = width;
        owns[i] = 1;
        off += (size_t)width;
    }
    len = off;

    p = malloc(len);
    if (!p) {
        free(s);
        return NULL;
    }
    memset(p, 0, len);
    memcpy(p, lab_header, sizeof lab_header);
    put_be32(p, (int32_t)len);
    put_be32(p + 128, CGICCLabTagCount);
    memcpy(p + CGICCLabDescOffset, lab_desc, CGICCLabDescLength);
    memcpy(p + CGICCLabCprtOffset, lab_cprt, CGICCLabCprtLength);
    put_create_date(p + CGICCLabCreateDateOffset);
    for (i = 2; i < CGICCLabTagCount; i++)
        if (owns[i])
            memcpy(p + tagOff[i], i < 4 ? xyz[i - 2] : lab_lut,
                (size_t)tagLen[i]);

    /* Owners before sharers, each in build order -- the RGB builder's rule. */
    {
        int slot = 0, pass;

        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < CGICCLabTagCount; i++) {
                unsigned char *entry;

                if (owns[i] != !pass)
                    continue;
                entry = p + CGICCLabTagTableOffset + slot * 12;
                memcpy(entry, tagNames[i], 4);
                put_be32(entry + 4, tagOff[i]);
                put_be32(entry + 8, tagLen[i]);
                slot++;
            }
        }
    }

    /* Deliberately no put_profile_id: Lab leaves bytes 84-99 zero, as
       lab_header already has them. */

    s->immortal = false;
    s->refcount = 1;
    s->model = kCGColorSpaceModelLab;
    s->type = CGColorSpaceTypeLab;
    s->ncomp = 3;
    /* A calibrated space is named for being calibrated, not for its white
       point, and this one has no name at all. */
    s->name = NULL;
    s->base = NULL;
    s->profile = p;
    s->profileLen = len;
    /* The range is kept beside the profile rather than inside it, which is
       observable: Apple reports two spaces with byte-identical profiles as
       unequal when only one of them was given a range.  A NULL range is the
       absence of one, not a range of zeros, so the flag carries that
       distinction -- no explicit range ever equals NULL. */
    s->hasRange = range != NULL;
    if (range)
        memcpy(s->range, range, sizeof s->range);
    return s;
}

/* Reference counting. */


CGColorSpaceRef CGColorSpaceRetain(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    /* An immortal space leaves its count at 0, so there is nothing to
       increment. */
    if (s && !s->immortal)
        s->refcount++;
    return space;
}

void CGColorSpaceRelease(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    if (!s || s->immortal)
        return;
    if (--s->refcount > 0)
        return;
    if (s->base) {
        CGColorSpaceRelease(s->base);
        s->base = NULL;
    }
    free(s->profile);
    free(s);
}

/* Shape. */

CFTypeID CGColorSpaceGetTypeID(void)
{
    return CG_COLORSPACE_TYPE_ID;
}

CGColorSpaceModel CGColorSpaceGetModel(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    return s ? s->model : kCGColorSpaceModelUnknown;
}

int CGColorSpaceGetType(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    return s ? s->type : 0;
}

size_t CGColorSpaceGetNumberOfComponents(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    return s ? s->ncomp : 0;
}

CGColorSpaceModel CGColorSpaceGetProcessColorModel(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    if (!s)
        return kCGColorSpaceModelUnknown;
    /* A pattern space paints in its base's model, and a pattern space with
       no base paints in nothing, which is Unknown rather than Pattern. */
    if (s->type == CGColorSpaceTypePattern)
        return s->base ? CGColorSpaceGetProcessColorModel(s->base)
                       : kCGColorSpaceModelUnknown;
    return s->model;
}

/* Naming. */

/* CopyName and GetName hand back the same pointer, which is what Apple does:
   the string is a constant, so there is no +1 reference to hand over and a
   caller releasing the result is releasing something immortal. */
CFStringRef CGColorSpaceCopyName(CGColorSpaceRef space)
{
    return CGColorSpaceNameFor(space);
}

CFStringRef CGColorSpaceGetName(CGColorSpaceRef space)
{
    return CGColorSpaceNameFor(space);
}

int CGColorSpaceGetID(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;
    CFStringRef name;
    int i, id = 0;

    /* The device spaces are not in the built-in table, so they report 0
       even though they are named. */
    if (!s || s->type == CGColorSpaceTypePattern)
        return 0;
    name = CGColorSpaceNameFor(s);
    if (!name)
        return 0;
    for (i = 1; i < CG_COLORSPACE_BUILT_IN_COUNT; i++) {
        if (CGColorSpaceNameEqualsASCII(name,
                CGColorSpaceBuiltInNames[i])) {
            id = i;
            break;
        }
    }
    CFRelease(name);
    return id;
}

int CGColorSpaceIDFromName(CFStringRef name)
{
    int i;

    if (!name)
        return 0;
    for (i = 1; i < CG_COLORSPACE_BUILT_IN_COUNT; i++) {
        if (CGColorSpaceNameEqualsASCII(name, CGColorSpaceBuiltInNames[i]))
            return i;
    }
    return 0;
}

CFStringRef CGColorSpaceNameFromID(int id)
{
    /* Out of range in either direction, including the 0 that means "no
       identifier". */
    if (id < 1 || id >= CG_COLORSPACE_BUILT_IN_COUNT)
        return NULL;
    return CFStringCreateWithCString(kCFAllocatorDefault,
        CGColorSpaceBuiltInNames[id], kCFStringEncodingUTF8);
}

/* Base. */

CGColorSpaceRef CGColorSpaceGetBaseColorSpace(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    return s ? s->base : NULL;
}

CGColorSpaceRef CGColorSpaceCopyBaseColorSpace(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    return s ? CGColorSpaceRetain(s->base) : NULL;
}

/* Identity. */

bool CGColorSpaceEqualToColorSpace(CGColorSpaceRef space1,
    CGColorSpaceRef space2)
{
    struct CGColorSpace *a = space1;
    struct CGColorSpace *b = space2;

    if (a == b)
        return true;
    if (!a || !b)
        return false;
    /* Two pattern spaces are equal when their bases are equal, so two
       patterns painted in the same space compare equal, and two colored
       patterns -- both of which have no base -- do too. */
    if (a->type == CGColorSpaceTypePattern && b->type == CGColorSpaceTypePattern)
        return CGColorSpaceEqualToColorSpace(a->base, b->base);
    /* A pattern space is never equal to a space that is not a pattern, and in
       particular is not equal to the space it wraps: Apple answers false for
       CGColorSpaceEqualToColorSpace(Pattern(rgb), rgb). */
    if (a->type == CGColorSpaceTypePattern || b->type == CGColorSpaceTypePattern)
        return false;
    /* A space with a profile is compared by that profile, so two calibrated
       spaces built from the same white point, black point and gamma are
       equal however they were built, and two built from different ones are
       not -- a calibrated space is not "some gray space", it is this gray
       space.  Comparing the model alone would call every pair equal.

       The comparison skips the creation date.  Apple stamps the local time
       into a Lab profile, and yet reports two Lab spaces built from the same
       arguments a second apart as equal, so the date is part of the profile
       that gets handed out and not part of the identity.  For the other
       calibrated spaces the field is constant, so skipping it changes
       nothing.

       A Lab space also carries a range that the profile does not describe,
       so it has to be compared too, or two spaces Apple calls different would
       look identical here. */
    if (a->profile || b->profile) {
        size_t k;

        if (!a->profile || !b->profile)
            return false;
        if (a->profileLen != b->profileLen)
            return false;
        if (a->hasRange != b->hasRange)
            return false;
        if (a->hasRange && memcmp(a->range, b->range, sizeof a->range) != 0)
            return false;
        for (k = 0; k < a->profileLen; k++) {
            if (k >= CGICCCreateDateOffset
                && k < CGICCCreateDateOffset + CGICCCreateDateLength)
                continue;
            if (a->profile[k] != b->profile[k])
                return false;
        }
        return true;
    }
    /* Two device spaces are equal when they are the same one; distinct
       shapes are not equal. */
    return a->model == b->model && a->ncomp == b->ncomp;
}

bool CGColorSpaceEqualToColorSpaceIgnoringRange(CGColorSpaceRef space1,
    CGColorSpaceRef space2)
{
    /* No space in this step has an output range, so ignoring the range
       cannot change the answer. */
    return CGColorSpaceEqualToColorSpace(space1, space2);
}

/* Capabilities. */

bool CGColorSpaceSupportsOutput(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    /* A pattern space describes a paint, not somewhere to paint, so it
       cannot be a drawing destination. */
    return s != NULL && s->type != CGColorSpaceTypePattern;
}

bool CGColorSpaceIsHDR(CGColorSpaceRef space)
{
    (void)space;
    return false;
}

bool CGColorSpaceIsHLGBased(CGColorSpaceRef space)
{
    (void)space;
    return false;
}

bool CGColorSpaceIsPQBased(CGColorSpaceRef space)
{
    (void)space;
    return false;
}

bool CGColorSpaceIsWideGamutRGB(CGColorSpaceRef space)
{
    (void)space;
    return false;
}

bool CGColorSpaceUsesExtendedRange(CGColorSpaceRef space)
{
    (void)space;
    return false;
}

bool CGColorSpaceIsUncalibrated(CGColorSpaceRef space)
{
    (void)space;
    /* False even for the device spaces: Apple treats them as calibrated,
       which is the opposite of what the name suggests. */
    return false;
}

bool CGColorSpaceIsICCCompatible(CGColorSpaceRef space)
{
    /* Being ICC-compatible is precisely having an ICC profile, so the
       calibrated spaces answer true and the device spaces false. */
    return space != NULL && space->profile != NULL;
}

bool CGColorSpaceIsPSLevel2Compatible(CGColorSpaceRef space)
{
    /* A synthesised profile is by construction a PS-Level-2 one, and Apple
       reports it as such for every calibrated space. */
    return space != NULL && space->profile != NULL;
}

bool CGColorSpaceIgnoresIntent(CGColorSpaceRef space)
{
    (void)space;
    /* True: with no ICC profile there is no intent to apply. */
    return true;
}

bool CGColorSpaceUsesITUR_2100TF(CGColorSpaceRef space)
{
    (void)space;
    return false;
}

int CGColorSpaceGetRenderingIntent(CGColorSpaceRef space)
{
    (void)space;
    return 0;
}

/* ICC profile.  None of the spaces in this step has one, so these all
   report "no profile" rather than manufacturing an empty one. */

CFDataRef CGColorSpaceCopyICCData(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    /* The device and pattern spaces carry no profile, and neither does
       anything we are handed that we do not recognise. */
    if (!s || !s->profile)
        return NULL;
    return CFDataCreate(kCFAllocatorDefault, s->profile, (CFIndex)s->profileLen);
}

CFDataRef CGColorSpaceCopyICCProfile(CGColorSpaceRef space)
{
    return CGColorSpaceCopyICCData(space);
}

CFDataRef CGColorSpaceGetMD5Digest(CGColorSpaceRef space)
{
    (void)space;
    /* Apple faults here rather than returning NULL; NULL is the answer the
       surrounding code would use, so it is what we give. */
    return NULL;
}

const char *CGColorSpaceGetIdentifier(CGColorSpaceRef space)
{
    (void)space;
    /* As above: Apple faults, we answer NULL. */
    return NULL;
}

/* Conversions.  None applies to a space with no profile. */

/* Color tables.  Only an indexed space has one, and no indexed space is
   built by this step, so the count is always zero and the copy is always a
   no-op that leaves the caller's buffer alone. */

size_t CGColorSpaceGetColorTableCount(CGColorSpaceRef space)
{
    (void)space;
    return 0;
}

void CGColorSpaceGetColorTable(CGColorSpaceRef space, uint8_t *table)
{
    (void)space;
    (void)table;
}

/* Descriptors.  A pattern space has one; the device spaces do not. */

CGColorSpaceRef CGColorSpaceGetAlternateColorSpace(CGColorSpaceRef space)
{
    (void)space;
    return NULL;
}

/* DeviceN-only queries.  Apple asserts that the space is a DeviceN space and
   aborts otherwise; nothing built by this step is one, so the answer is
   always "none" instead of a trap.

   GetHeadroomInfo, GetCICPInfo, GetConversionMatrix, GetDescriptor and
   GetTintTransform are not exported at all.  Each one either traps in Apple
   for every space reachable in this step -- GetDescriptor faults on a
   pattern space, GetConversionMatrix faults on all of them -- or reports
   nothing for the device spaces while its argument list could not be
   confirmed from the disassembly.  An export that cannot be checked is not
   worth having; the names stay reserved for a later step that can answer for
   an indexed, DeviceN or ICC-backed space. */

CFArrayRef CGColorSpaceGetNames(CGColorSpaceRef space)
{
    (void)space;
    return NULL;
}
