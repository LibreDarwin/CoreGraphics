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
    CGColorSpaceTypePattern = 9,
    /* What CGColorSpaceGetType reports for any space built from an ICC
       profile, whatever the profile's colour model.  Verified by reading the
       private CGColorSpaceGetType off a space built by
       CGColorSpaceCreateWithICCData: gray, RGB, CMYK, XYZ, Lab and a generic
       RGB all report 6, and so does every built-in named space except Lab,
       which reports 5 because CGColorSpaceCreateLab builds it directly rather
       than from a profile. */
    CGColorSpaceTypeICC = 6
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
    /* Whether the space uses values outside 0..1, which is what
       CGColorSpaceUsesExtendedRange reports and what an extended space
       carries that its base does not.

       This is not derivable from the profile.  CGColorSpaceCreateExtended
       hands back a profile byte-for-byte identical to the base's, and yet
       Apple reports the two spaces unequal, so the difference cannot live
       in the profile and is kept beside it.  A linearized space is *not*
       flagged: it is extended in the everyday sense but reports false, which
       is why CGColorSpaceIsWideGamutRGB cannot be answered from this flag
       alone either. */
    bool extended;
    /* Whether the space was built by CGColorSpaceCreateLinearized.  This one
       is observable too: a linearized RGB space reports a wide gamut while
       reporting no extended range, and a linearized gray space reports
       neither.  Deriving it from the profile would mean looking for an
       identity tone curve, which is what the flag records. */
    bool linearized;
};


/* The three device spaces.

   `name' is the ASCII form of the constant CFString Apple hands back.  It
   is spelled with the kCGColorSpace prefix that the name itself carries,
   i.e. the device RGB space is named "kCGColorSpaceDeviceRGB", not
   "Device RGB". */
static struct CGColorSpace CGColorSpaceDeviceGrayState = {
    0, true, kCGColorSpaceModelMonochrome, CGColorSpaceTypeMonochrome, 1,
    "kCGColorSpaceDeviceGray", NULL, NULL, 0, false, { 0, 0, 0, 0 }, false, false
};

static struct CGColorSpace CGColorSpaceDeviceRGBState = {
    0, true, kCGColorSpaceModelRGB, CGColorSpaceTypeRGB, 3,
    "kCGColorSpaceDeviceRGB", NULL, NULL, 0, false, { 0, 0, 0, 0 }, false, false
};

static struct CGColorSpace CGColorSpaceDeviceCMYKState = {
    0, true, kCGColorSpaceModelCMYK, CGColorSpaceTypeCMYK, 4,
    "kCGColorSpaceDeviceCMYK", NULL, NULL, 0, false, { 0, 0, 0, 0 }, false, false
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

/* Declared ahead of its first use: the gray builder needs to repoint tag
   table entries, and the definition sits with the calibrated-RGB helpers. */
static void put_be32(unsigned char *p, int32_t v);

/* Offsets into the gray template, named so the patch sites read as what they
   are rather than as magic numbers. */
enum {
    CGICCHeaderLength = 128,
    /* The data colour space signature, four bytes in.  It is what decides
       whether a profile can be linearized: 'GRAY' and 'RGB ' can, and 'Lab '
       cannot even though it also has three components. */
    CGICCColorSpaceOffset = 16,
    /* A tag count, then 12-byte entries of signature, offset and length. */
    CGICCTagCountOffset = 128,
    CGICCTagTableOffset = 132,
    /* The rest of the header that CGColorSpaceCreateWithICCData reads. */
    CGICCProfileSizeOffset = 0,
    CGICCVersionOffset = 8,
    CGICCDeviceClassOffset = 12,
    CGICCSignatureOffset = 36,
    CGICCTagEntrySize = 12,
    /* An XYZType is 'XYZ ', four reserved bytes, then three s15Fixed16. */
    CGICCXYZLength = 20,
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
    CGICCGrayLength = 380,
    /* When the black point shares the white point's block the tone curve
       moves up into the black point's old slot and the profile is 20 bytes
       shorter. */
    CGICCGrayTRCOffset = CGICCGrayGammaOffset - 12,
    CGICCGrayCollapsedTRCOffset = CGICCGrayTRCOffset - CGICCXYZLength,
    CGICCGrayCollapsedLength = CGICCGrayLength - CGICCXYZLength
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
    size_t len;

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

    /* A black point that quantises to the same bytes as the white point
       shares its block rather than getting one of its own, which pulls the
       tone curve up by 20 bytes and takes 20 off the end.  The tag table
       keeps the same five entries but lists the tag that lost its block
       last.  A zero white point with no black point is the case that
       reaches this; the usual case has a white point and a black point that
       differ and never does. */
    if (memcmp(p + CGICCGrayWTptOffset, p + CGICCGrayBKptOffset,
            CGICCXYZLength) == 0) {
        unsigned char *q = malloc(CGICCGrayCollapsedLength);

        if (!q) {
            free(p);
            free(s);
            return NULL;
        }
        memcpy(q, p, CGICCGrayBKptOffset);
        memcpy(q + CGICCGrayCollapsedTRCOffset,
            p + CGICCGrayTRCOffset, 16);
        /* Slots three and four swap places, and their offsets change: the
           tone curve moves down to where the black point was, and the black
           point points at the white point.  Slot two still describes the
           white point. */
        memcpy(q + CGICCTagTableOffset + 3 * 12, p + CGICCTagTableOffset + 4 * 12,
            12);
        put_be32(q + CGICCTagTableOffset + 3 * 12 + 4,
            (int32_t)CGICCGrayCollapsedTRCOffset);
        memcpy(q + CGICCTagTableOffset + 4 * 12, p + CGICCTagTableOffset + 3 * 12,
            12);
        put_be32(q + CGICCTagTableOffset + 4 * 12 + 4,
            (int32_t)CGICCGrayWTptOffset);
        free(p);
        p = q;
        len = CGICCGrayCollapsedLength;
    } else {
        len = CGICCGrayLength;
    }
    put_be32(p, (int32_t)len);
    put_profile_id(p, len);

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
    s->profileLen = len;
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

/* Linearized and extended spaces.

   Only one of the three synthesises anything.  CGColorSpaceCreateExtended
   copies its base's profile byte for byte and sets a flag; and
   CGColorSpaceCreateExtendedLinearized produces a profile byte for byte
   identical to CGColorSpaceCreateLinearized's, differing only in that same
   flag.  Both are observable only because Apple reports an extended space
   unequal to the base whose profile it hands back unchanged, and because
   CGColorSpaceUsesExtendedRange tells them apart.

   CreateLinearized does the real work, and its profile keeps the base's white
   point and colorants verbatim -- read straight out of the base profile, not
   recomputed -- while dropping the black point and the copyright, replacing
   the tone curve with an identity one, and leaving the profile ID all zeros.
   The creation date is a constant here, as it is for the calibrated spaces,
   so unlike Lab no byte of it is live.

   The description is the one field that is not a copy: Apple appends
   " Linearized" to whatever description it is given, so linearizing an
   already-linearized space appends the word a second time and the profile
   grows by the 22 bytes the appended text occupies.  That is why
   linearizing is not idempotent, and why the result is not something that can
   be produced by a fixed template. */

/* A big-endian 32-bit read, for the tag table. */
static uint32_t get_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8) | p[3];
}

/* Find a tag in an ICC profile and return where its data starts.

   The table is a count at byte 128 followed by 12-byte entries of a
   four-character signature, a big-endian offset and a big-endian length.
   Several tags can point at one block -- that is how Apple shares a tone
   curve between the three channels -- so the length is a property of the tag
   and not of the block, which is why it is returned rather than inferred. */
static long icc_find_tag(const unsigned char *p, size_t len, const char *sig,
    size_t *tagLen)
{
    uint32_t count, i;

    if (len < CGICCTagTableOffset)
        return -1;
    count = get_be32(p + CGICCTagCountOffset);
    if (count > (len - CGICCTagTableOffset) / 12)
        return -1;
    for (i = 0; i < count; i++) {
        const unsigned char *e = p + CGICCTagTableOffset + (size_t)i * 12;

        if (memcmp(e, sig, 4) == 0) {
            size_t off = get_be32(e + 4);
            size_t l = get_be32(e + 8);

            if (off > len || l > len - off)
                return -1;
            if (tagLen)
                *tagLen = l;
            return (long)off;
        }
    }
    return -1;
}

/* The value of an s15Fixed16 field.  The inverse of put_s15Fixed16. */
static double icc_s15Fixed16(const unsigned char *p)
{
    return (double)(int32_t)get_be32(p) / 65536.0;
}

/* Whether an ICC profile describes a gamut wider than sRGB.

   For a space built by CGColorSpaceCreateWithICCData this is a property of
   the primaries rather than of a flag, and the measure is the area of the
   triangle the three colorants subtend in CIE 1931 xy.  Nothing else in the
   profile enters into it: changing the white point tag from D50 to D65 at a
   fixed set of primaries does not move the answer, and neither does replacing
   the tone curve with any gamma from 1.0 to 2.4.

   It is the whole triangle that counts, not any one vertex.  Substituting P3's
   red, green or blue into the sRGB primaries individually leaves the answer
   false; substituting all three makes it true.  That rules out comparing each
   colorant against sRGB's.

   The boundary was bisected by interpolating the primaries between the sRGB
   and Display P3 profiles: the last profile that reports false has an area of
   0.134466838 and the first that reports true has 0.134470543, so the
   threshold below sits inside a window of 4e-6. */
static bool icc_rgb_is_wide(const unsigned char *p, size_t len)
{
    static const char *const prim[3] = { "rXYZ", "gXYZ", "bXYZ" };
    double xy[3][2];
    double area;
    size_t i;

    for (i = 0; i < 3; i++) {
        size_t tagLen;
        long at = icc_find_tag(p, len, prim[i], &tagLen);
        double sum;

        /* An XYZType is 'XYZ ', four reserved bytes, then three s15Fixed16 --
           the components start at offset 8, not 4. */
        if (at < 0 || tagLen < 20)
            return false;
        if (memcmp(p + at, "XYZ ", 4) != 0)
            return false;

        sum = icc_s15Fixed16(p + at + 8) + icc_s15Fixed16(p + at + 12)
            + icc_s15Fixed16(p + at + 16);
        if (!(sum > 0.0))
            return false;
        xy[i][0] = icc_s15Fixed16(p + at + 8) / sum;
        xy[i][1] = icc_s15Fixed16(p + at + 12) / sum;
    }

    area = 0.5 * fabs(xy[0][0] * (xy[1][1] - xy[2][1])
        + xy[1][0] * (xy[2][1] - xy[0][1])
        + xy[2][0] * (xy[0][1] - xy[1][1]));
    return area > 0.13446869;
}

/* The description tag is an mluc: 'mluc', four reserved bytes, a record
   count, a record size, then that many records of language, country, length
   and offset.  Apple writes exactly one enUS record and puts the text at
   offset 28, which is the size of the header and the record together, so the
   text begins where the record describing it ends. */
enum {
    CGICCMLUCRecordLength = 28
};

static int mluc_text(const unsigned char *tag, size_t len,
    const unsigned char **text, size_t *textLen)
{
    uint32_t count, recSize, recLen, off;

    if (len < CGICCMLUCRecordLength || memcmp(tag, "mluc", 4) != 0)
        return 0;
    count = get_be32(tag + 8);
    recSize = get_be32(tag + 12);
    if (count == 0 || recSize < 12)
        return 0;
    recLen = get_be32(tag + 20);
    off = get_be32(tag + 24);
    if (off > len || recLen > len - off)
        return 0;
    *text = tag + off;
    *textLen = recLen;
    return 1;
}

/* " Linearized", in the UTF-16BE an mluc stores its text in. */
static const unsigned char CGICCLinearizedWord[] = {
    0x00,  0x20,  0x00,  0x4c,  0x00,  0x69,  0x00,  0x6e,  0x00,  0x65,
    0x00,  0x61,  0x00,  0x72,  0x00,  0x69,  0x00,  0x7a,  0x00,  0x65,
    0x00,  0x64
};

/* The tone curve of a linearized space: a 'curv' with a single entry, which
   is the identity curve by definition.  Apple declares it 14 bytes here
   rather than padding to 16 the way the parametric curve in the calibrated
   templates is, so the last tag of a linearized gray profile ends at 274 and
   the profile is 276 after padding. */
static const unsigned char CGICCIdentityCurve[14] = {
    0x63,  0x75,  0x72,  0x76,  0x00,  0x00,  0x00,  0x00,
    0x00,  0x00,  0x00,  0x01,  0x01,  0x00
};

/* The tag table of a linearized profile: three tags for a gray space and
   eight for RGB.  The three tone curves are listed as r, b, g and all point
   at one block, which is the order the calibrated RGB template uses for the
   same three tags. */
static const char *const CGICCLinearizedGrayTags[] = { "desc", "wtpt", "kTRC" };
static const char *const CGICCLinearizedRGBTags[] = {
    "desc", "wtpt", "rXYZ", "gXYZ", "bXYZ", "rTRC", "bTRC", "gTRC"
};

/* Round an offset up to the 4-byte boundary the tag data starts on. */
static size_t align4(size_t off)
{
    return (off + 3) & ~(size_t)3;
}

/* Build the space, sharing one code path between the three entry points. */
static CGColorSpaceRef CGColorSpaceCreateLinearizedInternal(
    CGColorSpaceRef baseSpace, bool extended)
{
    struct CGColorSpace *base = baseSpace;
    struct CGColorSpace *s;
    const char *const *tags;
    const unsigned char *baseText;
    unsigned char *p, *baseDesc;
    size_t baseTextLen, descLen, tagLen, len, off, descOff, wtptOff, trcOff;
    size_t colorOff[3] = { 0, 0, 0 };
    bool colorOwn[3] = { false, false, false };
    long wtptAt, colorAt[3], descAt;
    size_t ntags;
    bool rgb;
    int i;

    if (!base || !base->profile)
        return NULL;
    /* Only a gray or RGB profile can be linearized.  The device and pattern
       spaces have no profile, and a Lab profile is a device class of its own
       whose tags are not tone curves -- so the data colour space in the
       header, not the component count, is what decides. */
    if (memcmp(base->profile + CGICCColorSpaceOffset, "GRAY", 4) == 0) {
        rgb = false;
    } else if (memcmp(base->profile + CGICCColorSpaceOffset, "RGB ", 4) == 0) {
        rgb = true;
    } else {
        return NULL;
    }

    /* Everything the new profile keeps is read out of the base's, so a base
       that is itself linearized or extended -- which is why chaining works
       -- needs no special case. */
    descAt = icc_find_tag(base->profile, base->profileLen, "desc", &tagLen);
    if (descAt < 0 || !mluc_text(base->profile + descAt, tagLen, &baseText,
        &baseTextLen))
        return NULL;
    wtptAt = icc_find_tag(base->profile, base->profileLen, "wtpt", &tagLen);
    if (wtptAt < 0 || tagLen != CGICCXYZLength)
        return NULL;
    if (rgb) {
        static const char *const xyz[3] = { "rXYZ", "gXYZ", "bXYZ" };

        for (i = 0; i < 3; i++) {
            colorAt[i] = icc_find_tag(base->profile, base->profileLen,
                xyz[i], &tagLen);
            if (colorAt[i] < 0 || tagLen != CGICCXYZLength)
                return NULL;
        }
    }

    tags = rgb ? CGICCLinearizedRGBTags : CGICCLinearizedGrayTags;
    ntags = rgb ? sizeof CGICCLinearizedRGBTags / sizeof *CGICCLinearizedRGBTags
        : sizeof CGICCLinearizedGrayTags / sizeof *CGICCLinearizedGrayTags;
    descLen = CGICCMLUCRecordLength + baseTextLen
        + sizeof CGICCLinearizedWord;

    /* The tag data follows the table, each block aligned to 4.  The three
       tone curves share the last block, so a profile ends just after it.

       A colorant that quantises to the same bytes as the white point, or as
       an earlier colorant, does not get a block of its own -- it points at
       the block already holding those bytes.  A zero white point makes every
       colorant zero as well, so all three collapse onto the white point and
       a profile ends up 60 bytes shorter. */
    off = align4(CGICCTagTableOffset + ntags * 12);
    descOff = off;
    off = align4(off + descLen);
    wtptOff = off;
    off += CGICCXYZLength;
    if (rgb) {
        for (i = 0; i < 3; i++) {
            const unsigned char *at = base->profile + colorAt[i];

            if (memcmp(at, base->profile + wtptAt, CGICCXYZLength) == 0) {
                colorOff[i] = wtptOff;
                continue;
            }
            if (i && memcmp(at, base->profile + colorAt[i - 1],
                    CGICCXYZLength) == 0) {
                colorOff[i] = colorOff[i - 1];
                continue;
            }
            colorOwn[i] = true;
            colorOff[i] = off;
            off += CGICCXYZLength;
        }
    }
    trcOff = off;
    len = align4(off + sizeof CGICCIdentityCurve);

    p = calloc(1, len);
    if (!p)
        return NULL;
    s = calloc(1, sizeof *s);
    if (!s) {
        free(p);
        return NULL;
    }

    /* The header is the base's, so the version, the device class, the
       platform and manufacturer, the rendering intent and the constant
     creation date all carry over.  Only the size changes -- and the profile
       ID, which is left zeroed as it is for a Lab space. */
    memcpy(p, base->profile, CGICCHeaderLength);
    p[0] = (unsigned char)(len >> 24);
    p[1] = (unsigned char)(len >> 16);
    p[2] = (unsigned char)(len >> 8);
    p[3] = (unsigned char)len;
    memset(p + CGICCProfileIDOffset, 0, CGICCProfileIDLength);
    put_be32(p + CGICCTagCountOffset, (int32_t)ntags);

    baseDesc = p + descOff;
    memcpy(baseDesc, base->profile + descAt, CGICCMLUCRecordLength);
    put_be32(baseDesc + 20, (int32_t)(baseTextLen + sizeof CGICCLinearizedWord));
    memcpy(baseDesc + CGICCMLUCRecordLength, baseText, baseTextLen);
    memcpy(baseDesc + CGICCMLUCRecordLength + baseTextLen,
        CGICCLinearizedWord, sizeof CGICCLinearizedWord);

    memcpy(p + wtptOff, base->profile + wtptAt, CGICCXYZLength);
    if (rgb) {
        for (i = 0; i < 3; i++)
            if (colorOwn[i])
                memcpy(p + colorOff[i], base->profile + colorAt[i],
                    CGICCXYZLength);
    }
    memcpy(p + trcOff, CGICCIdentityCurve, sizeof CGICCIdentityCurve);

    /* The tag table lists the tags that own a block first, in allocation
       order, and the tags that point at an existing block after them.  With
       nothing shared the two groups run together and the table is just the
       natural order, which is why this is not visible for any base whose
       colorants differ.

       Each tag's block is settled once here and the two passes below only
       decide which group it goes in, so the offset and length of a tag cannot
       come out different in the two places that write it. */
    {
        size_t off1[8], len1[8];
        bool owns1[8];
        size_t slot = 0;
        int trcFirst, pass;

        /* The tone curves come after the colorants, so the first tag that is
           not a colorant is the one that owns the shared curve block.  That is
           rTRC for an RGB profile and kTRC for a gray one. */
        for (trcFirst = 2; trcFirst < (int)ntags; trcFirst++)
            if (tags[trcFirst][1] != 'X')
                break;

        for (i = 0; i < (int)ntags; i++) {
            if (i == 0) {
                off1[i] = descOff;
                len1[i] = descLen;
                owns1[i] = true;
            } else if (i == 1) {
                off1[i] = wtptOff;
                len1[i] = CGICCXYZLength;
                owns1[i] = true;
            } else if (tags[i][1] == 'X') {
                off1[i] = colorOff[i - 2];
                len1[i] = CGICCXYZLength;
                owns1[i] = colorOwn[i - 2];
            } else {
                /* rTRC, bTRC and gTRC all name one shared block, so only the
                   entry that names it first owns the bytes. */
                off1[i] = trcOff;
                len1[i] = sizeof CGICCIdentityCurve;
                owns1[i] = (i == trcFirst);
            }
        }
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < (int)ntags; i++) {
                unsigned char *e;

                if (owns1[i] != (pass == 0))
                    continue;
                e = p + CGICCTagTableOffset + slot * 12;
                memcpy(e, tags[i], 4);
                put_be32(e + 4, (int32_t)off1[i]);
                put_be32(e + 8, (int32_t)len1[i]);
                slot++;
            }
    }

    s->immortal = false;
    s->refcount = 1;
    s->model = base->model;
    s->type = base->type;
    s->ncomp = base->ncomp;
    s->name = base->name;
    s->base = NULL;
    s->profile = p;
    s->profileLen = len;
    s->linearized = true;
    s->extended = extended;
    return s;
}

CGColorSpaceRef CGColorSpaceCreateLinearized(CGColorSpaceRef baseSpace)
{
    return CGColorSpaceCreateLinearizedInternal(baseSpace, false);
}

CGColorSpaceRef CGColorSpaceCreateExtendedLinearized(
    CGColorSpaceRef baseSpace)
{
    return CGColorSpaceCreateLinearizedInternal(baseSpace, true);
}

/* The extended variant of a calibrated space.  This one synthesises nothing:
   the profile is the base's, unchanged down to the profile ID, and the only
   difference between the two spaces is the flag. */
CGColorSpaceRef CGColorSpaceCreateExtended(CGColorSpaceRef baseSpace)
{
    struct CGColorSpace *base = baseSpace;
    struct CGColorSpace *s;
    unsigned char *p;

    if (!base || !base->profile)
        return NULL;
    if (memcmp(base->profile + CGICCColorSpaceOffset, "GRAY", 4) != 0
        && memcmp(base->profile + CGICCColorSpaceOffset, "RGB ", 4) != 0)
        return NULL;
    p = malloc(base->profileLen);
    if (!p)
        return NULL;
    memcpy(p, base->profile, base->profileLen);
    s = calloc(1, sizeof *s);
    if (!s) {
        free(p);
        return NULL;
    }
    s->immortal = false;
    s->refcount = 1;
    s->model = base->model;
    s->type = base->type;
    s->ncomp = base->ncomp;
    s->name = base->name;
    s->base = NULL;
    s->profile = p;
    s->profileLen = base->profileLen;
    s->extended = true;
    return s;
}

/* An ICC profile's data colour space signature, read as the model and
   component count Apple reports.  Only the five signatures real profiles use
   are mapped, plus the DeviceN forms: 'nCLR', where the digit is the channel
   count, and the handful of three-channel names CoreGraphics knows.

   `required' receives the tags Apple insists on for the mapped model, and
   `nclass' how many device classes it accepts.  Both vary by signature and
   were measured by dropping tags one at a time and rewriting the class byte;
   see the comment on the caller for the tables.  For the DeviceN signatures
   both are placeholders the caller replaces from the body, since a DeviceN
   space takes its requirements and its class set from what the tags describe
   rather than from its own name. */
static int icc_model_for(const unsigned char *sig, CGColorSpaceModel *model,
    size_t *ncomp, const char *const **required, int *nclass)
{
    static const char *const rgb[] = {
        "rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC", NULL
    };
    static const char *const gray[] = { "kTRC", NULL };
    static const char *const cmyk[] = { "A2B0", "B2A0", NULL };
    static const char *const lab[] = { "A2B0", "B2A0", NULL };
    static const char *const xyz[] = { "A2B0", "B2A0", NULL };
    /* A DeviceN space is named for a colour space Apple does not model.  The
       mandatory tags still come from the *body*'s own model, which the caller
       derives from the tags themselves, so no list is attached here; the
       caller resolves it after reading the tag table. */

    if (memcmp(sig, "GRAY", 4) == 0) {
        *model = kCGColorSpaceModelMonochrome; *ncomp = 1;
        *required = gray; *nclass = 3; return 1;
    }
    if (memcmp(sig, "RGB ", 4) == 0) {
        *model = kCGColorSpaceModelRGB; *ncomp = 3;
        *required = rgb; *nclass = 2; return 1;
    }
    if (memcmp(sig, "CMYK", 4) == 0) {
        *model = kCGColorSpaceModelCMYK; *ncomp = 4;
        *required = cmyk; *nclass = 5; return 1;
    }
    if (memcmp(sig, "Lab ", 4) == 0) {
        *model = kCGColorSpaceModelLab; *ncomp = 3;
        *required = lab; *nclass = 5; return 1;
    }
    if (memcmp(sig, "XYZ ", 4) == 0) {
        *model = kCGColorSpaceModelXYZ; *ncomp = 3;
        *required = xyz; *nclass = 5; return 1;
    }
    /* 'HSV ', 'CMY ', 'Yxy ', 'Luv ' and 'HLS ' are all three channels, and
       CoreGraphics maps each of them to DeviceN.  'YCbr' is not one of them
       and is refused, like every other unrecognised name. */
    if (memcmp(sig, "HSV ", 4) == 0 || memcmp(sig, "CMY ", 4) == 0
        || memcmp(sig, "Yxy ", 4) == 0 || memcmp(sig, "Luv ", 4) == 0
        || memcmp(sig, "HLS ", 4) == 0) {
        *model = kCGColorSpaceModelDeviceN; *ncomp = 3;
        *required = NULL; *nclass = 5; return 2;
    }
    /* 'nCLR' carries its channel count in the digit. */
    if (sig[1] == 'C' && sig[2] == 'L' && sig[3] == 'R') {
        unsigned d;

        if (sig[0] >= '1' && sig[0] <= '9')
            d = (unsigned)(sig[0] - '0');
        else if (sig[0] >= 'A' && sig[0] <= 'F')
            d = (unsigned)(sig[0] - 'A') + 10u;
        else
            return 0;
        *model = kCGColorSpaceModelDeviceN; *ncomp = d;
        *required = NULL; *nclass = 5; return 2;
    }
    return 0;
}

/* Is this tag in the table? */
static int icc_has(const unsigned char *p, uint32_t count, const char *sig)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        const unsigned char *e = p + CGICCTagTableOffset + (size_t)i * CGICCTagEntrySize;

        if (memcmp(e, sig, 4) == 0)
            return 1;
    }
    return 0;
}

/* A DeviceN profile's tags still form one of the models above, and that is
   what its mandatory tags, channel count and device-class set are.  Only the
   colorants and the 'A2B0'/'B2A0' pair are read: dropping 'gamt', 'A2B1',
   'A2B2', 'B2A1', 'B2A2' or 'bkpt' leaves acceptance alone, and dropping
   either LUT ends it.  CMYK, Lab and XYZ all carry the same 'A2B0'/'B2A0' pair
   and are not separable from the tags, but they differ in channel count and
   only one of the three-channel models exists -- so the one four-channel case
   is settled from the signature, which is still the original 'CMYK' on a
   profile whose model is DeviceN.

   That last part is known to be incomplete: Apple accepts '4CLR' on a CMYK
   body and reports four channels, and no mutation tried here reproduces the
   channel count without also breaking the profile -- setting A2B0's
   inputChannels to 3 on the same body still refuses '3CLR'.  So the count is
   read from somewhere inside the LUT that has not been isolated, and 'CMYK'
   is a stand-in that covers only the signature-as-shipped case. */
/* A body's channel count, when it is described by an 'A2B0' LUT, is that LUT's
   own inputChannels byte: the shipped CMYK profile says 4 and reports four
   channels, the shipped Lab profile says 3 and reports three.  The signature
   cannot be the source, because overwriting that byte on either profile makes
   Apple refuse the profile outright -- 'A2B0' has to stay consistent with the
   table it describes -- instead of answering a different channel count.  That
   is also why no mutation of this byte can ever be observed to change the
   answer, and why the reported length is not checked either: growing 'A2B0'
   by a byte is still accepted.

   Returns the count, or -1 when there is no usable 'A2B0'. */
static int icc_lut_channels(const unsigned char *p, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        const unsigned char *e = p + CGICCTagTableOffset
            + (size_t)i * CGICCTagEntrySize;
        uint64_t off, size;

        if (memcmp(e, "A2B0", 4) != 0)
            continue;
        off = get_be32(e + 4);
        size = get_be32(e + 8);
        if (size < 9)
            return -1;
        return p[off + 8];
    }
    return -1;
}

static int icc_body_model(const unsigned char *p, uint32_t count,
    CGColorSpaceModel *model, size_t *ncomp, const char *const **required,
    int *nclass)
{
    static const char *const rgb[] = {
        "rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC", NULL
    };
    static const char *const cmyk[] = { "A2B0", "B2A0", NULL };
    static const char *const xyz[] = { "A2B0", "B2A0", NULL };

    if (icc_has(p, count, "rXYZ") || icc_has(p, count, "gXYZ")
        || icc_has(p, count, "bXYZ")) {
        *model = kCGColorSpaceModelRGB; *ncomp = 3;
        *required = rgb; *nclass = 2; return 1;
    }
    if (icc_has(p, count, "A2B0") && icc_has(p, count, "B2A0")) {
        int n = icc_lut_channels(p, count);

        /* Three and four are the only widths with a body to measure against,
           and the 'nCLR' signature still has to name a space of that width:
           the CMYK body takes '4CLR' and refuses '3CLR' and '5CLR', the Lab
           body takes '3CLR' and refuses '4CLR'.  A five- or six-channel body
           would need a consistent LUT that no shipped profile provides, so
           those are refused rather than guessed at. */
        if (n == 4) {
            *model = kCGColorSpaceModelCMYK; *ncomp = 4;
            *required = cmyk;
        } else if (n == 3) {
            *model = kCGColorSpaceModelXYZ; *ncomp = 3;
            *required = xyz;
        } else {
            return 0;
        }
        *nclass = 5; return 1;
    }
    /* A gray body is deliberately not matched here.  Every 'nCLR' and every
       three-channel DeviceN name is refused on a gray profile, '1CLR'
       included -- and a gray profile does have exactly one channel, so this is
       not the channel-count agreement doing the rejecting.  A gray body has no
       'A2B0'/'B2A0' pair either, and dropping tags one at a time shows those
       two are the only ones the DeviceN path reads: removing 'gamt', 'A2B1',
       'A2B2' or 'bkpt' changes nothing, while removing either LUT does. */
    return 0;
}

/* The device classes Apple accepts for a given colour space signature.  This
   is not a flat allowlist of the five real ICC classes: an RGB profile takes
   only 'mntr' and 'scnr', a gray one also takes 'prtr', and CMYK, Lab and XYZ
   take all five.  Measured by rewriting the class byte of a working profile
   and reading the result back; the sets are what CoreGraphics does, not what
   the ICC specification suggests. */
static int icc_class_ok(const unsigned char *cls, int nclass)
{
    static const char *const all[] = { "mntr", "scnr", "prtr", "spac", "abst" };
    int i;

    for (i = 0; i < nclass; i++)
        if (memcmp(cls, all[i], 4) == 0)
            return 1;
    return 0;
}

/* CGColorSpaceCreateWithICCData.  Accepts an ICC profile the caller already
   has and answers the model and component count its header declares, keeping
   the profile bytes verbatim so CGColorSpaceCopyICCData hands them back
   unchanged.

   What Apple validates, established by mutating one field at a time and
   reading the result back: the 'acsp' signature, a nonzero profile version, a
   device class the colour space accepts (see icc_class_ok), and a tag table
   that fits with every tag's offset+length inside the data.  Apple also
   insists on the tags the mapped model cannot be described without -- six
   colorant/Tone tags for RGB, 'kTRC' for gray, 'A2B0' and 'B2A0' for CMYK, Lab
   and XYZ -- so lowering the tag count until one of those falls off the end is
   refused, while dropping 'desc', 'cprt', 'wtpt' or 'bkpt' is not.  The
   profile-size field is not one of the things it checks: a profile claiming 0
   or 0xFFFFFFFF bytes is still accepted, and so is one claiming a size smaller
   than its own tag table.

   The length actually kept is min(data length, max(needed, declared)), where
   needed is the end of the tag table or the furthest tag, whichever is
   later.  Data beyond that is dropped, which is why a buffer with 64 trailing
   zero bytes comes back 64 bytes shorter than it went in. */
CGColorSpaceRef CGColorSpaceCreateWithICCData(CFDataRef data)
{
    const unsigned char *p;
    size_t len, need, stored;
    uint32_t count, i;
    uint64_t far;
    CGColorSpaceModel model;
    size_t ncomp;
    const char *const *required;
    int nclass, ri;
    struct CGColorSpace *s;
    unsigned char *copy;

    if (!data)
        return NULL;
    p = CFDataGetBytePtr(data);
    len = (size_t)CFDataGetLength(data);
    /* Enough for a header and one tag table entry. */
    if (len < CGICCTagTableOffset + CGICCTagEntrySize)
        return NULL;
    if (memcmp(p + CGICCSignatureOffset, "acsp", 4) != 0)
        return NULL;
    if (get_be32(p + CGICCVersionOffset) == 0)
        return NULL;

    /* The end of the tag table, and the furthest byte any tag reaches.  Both
       are computed in 64 bits so an out-of-range offset or length cannot wrap
       a 32-bit sum and slip through.

       The declared count is not trusted to fit: a profile claiming 0xFFFFFFFF
       tags is refused, but the entries are read only while the table still
       lies inside the data, and a table that overruns simply leaves `far' past
       the end. */
    count = get_be32(p + CGICCTagCountOffset);
    far = (uint64_t)CGICCTagTableOffset + (uint64_t)count * CGICCTagEntrySize;
    if (far > (uint64_t)len)
        return NULL;
    for (i = 0; i < count; i++) {
        const unsigned char *e = p + CGICCTagTableOffset + (size_t)i * CGICCTagEntrySize;
        uint64_t end = (uint64_t)get_be32(e + 4) + get_be32(e + 8);

        if (end > far)
            far = end;
    }
    /* A tag pointing past the end of the data fails here, because needed
       then exceeds what the caller supplied. */
    if (far > len)
        return NULL;
    need = (size_t)far;
    if (need < CGICCTagTableOffset + CGICCTagEntrySize)
        return NULL;
    /* Does the tag table carry this tag? */
    {
        int rc = icc_model_for(p + CGICCColorSpaceOffset, &model, &ncomp,
            &required, &nclass);

        if (rc == 0)
            return NULL;
        if (rc == 2) {
            /* A DeviceN signature does not say what the profile describes, so
               the tags decide: the mandatory set belongs to whichever body
               model they form, and the channel count in the signature has to
               agree with that body or the profile is refused.  This is what
               makes '3CLR' work on an RGB, Lab or XYZ body and fail on a gray
               or CMYK one.  The model stays DeviceN; only the requirements
               and the channel count come from the body. */
            CGColorSpaceModel bm;
            size_t bn;
            const char *const *breq;
            int bcl;

            if (!icc_body_model(p, count, &bm, &bn, &breq, &bcl))
                return NULL;
            if (bn != ncomp)
                return NULL;
            required = breq;
            /* The class set is the body's too, not DeviceN's own.  An RGB body
               relabelled 'HSV ' accepts only 'mntr' and 'scnr', the same two an
               RGB profile accepts, while a CMYK or Lab body accepts all five.
               So the check follows the body even though the model reported is
               DeviceN. */
            nclass = bcl;
        }
    }
    if (!icc_class_ok(p + CGICCDeviceClassOffset, nclass))
        return NULL;
    /* The tags the mapped model cannot be described without have to be in the
       table.  This is what makes a lowered tag count fail: the entries are
       dropped from the tail, so whichever mandatory tag sat last is the one
       that disappears. */
    for (ri = 0; required && required[ri]; ri++) {
        int found = 0;

        for (i = 0; i < count && !found; i++) {
            const unsigned char *e = p + CGICCTagTableOffset
                + (size_t)i * CGICCTagEntrySize;

            if (memcmp(e, required[ri], 4) == 0)
                found = 1;
        }
        if (!found)
            return NULL;
    }

    {
        uint64_t declared = get_be32(p + CGICCProfileSizeOffset);
        uint64_t cap = declared > (uint64_t)need ? declared : (uint64_t)need;

        stored = len < (size_t)cap ? len : (size_t)cap;
    }

    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    copy = malloc(stored);
    if (!copy) {
        free(s);
        return NULL;
    }
    memcpy(copy, p, stored);
    s->immortal = false;
    s->refcount = 1;
    s->model = model;
    s->type = CGColorSpaceTypeICC;
    s->ncomp = ncomp;
    /* A profile handed to us carries no built-in name: Apple recovers one by
       matching the bytes against its profile table, which this step does not
       have, so the name stays absent until that table exists. */
    s->name = NULL;
    s->base = NULL;
    s->profile = copy;
    s->profileLen = stored;
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
       look identical here.

       The same is true of an extended space, and it is the sharper case: an
       extended space's profile is its base's byte for byte, so comparing
       profiles alone would report every space equal to its own extended
       form.  The linearized flag is compared for the same reason -- a
       linearized space and an extended linearized space have identical
       profiles and are still unequal. */
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
        if (a->extended != b->extended || a->linearized != b->linearized)
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
    struct CGColorSpace *s = space;

    if (s == NULL)
        return false;

    /* A space built from a profile answers from its colorants, not from a
       flag, and only an RGB one has the primaries the measure needs.  The
       same profile handed to CGColorSpaceCreateCalibratedRGB's caller instead
       reaches the flag path below, which is why the type has to be tested
       before the flags. */
    if (s->type == CGColorSpaceTypeICC)
        return s->model == kCGColorSpaceModelRGB && s->profile != NULL
            && icc_rgb_is_wide(s->profile, s->profileLen);

    /* A gamut wider than sRGB is not a property of the profile: a linearized
       RGB space answers true while answering false for an extended range,
       and both an extended gray and a linearized gray answer false.  So it
       is a three-component space that was linearized or extended, and
       nothing else.

       One caveat, and it is visible in the profile rather than the flags.  A
       linearized RGB space whose colorants all collapsed onto the white
       point's block is degenerate -- a zero white point makes every colorant
       zero as well -- and Apple does not call those wide gamut.  The collapse
       is legible in the tag table, so it can be asked rather than recorded. */
    if (s->ncomp != 3 || (!s->extended && !s->linearized))
        return false;
    if (s->linearized && !s->extended && s->profile) {
        size_t len;
        long wtpt = icc_find_tag(s->profile, s->profileLen, "wtpt", &len);
        long rXYZ = icc_find_tag(s->profile, s->profileLen, "rXYZ", &len);

        if (wtpt < 0 || rXYZ < 0 || wtpt == rXYZ)
            return false;
    }
    return true;
}

bool CGColorSpaceUsesExtendedRange(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    /* True only for the spaces built by CreateExtended and
       CreateExtendedLinearized.  A linearized space is not flagged, so this
       cannot be derived from the profile: an extended space hands back its
       base's profile unchanged. */
    return s != NULL && s->extended;
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

/* ICC profile.  A space built by CGColorSpaceCreateWithICCData carries the
   bytes it was given; the device, pattern and calibrated spaces have none, so
   those still report "no profile" rather than manufacturing an empty one. */

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
