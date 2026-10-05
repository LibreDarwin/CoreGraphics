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
    /* What CGColorSpaceGetType reports for an indexed space, and the only
       value here that does not come from the base: an indexed space reports 7
       whether its base is gray, RGB or CMYK, so this tag cannot be derived
       from the model the way the device spaces' can. */
    CGColorSpaceTypeIndexed = 7,
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
    /* A Lab space's white point as the caller gave it, and whether they gave
       one at all.  This is kept beside the profile for the same reason the
       range is, and it is the sharper case: Apple collapses the white point
       when it cannot be written as s15Fixed16, so CGColorSpaceCreateLab built
       from D65 and from D50 both produce the very same 496 bytes and are yet
       reported unequal.  A white point that was collapsed away is still what
       the caller asked for, and it is what tells the two apart.

       The generic Lab space recorded by name records none, which is what
       keeps it distinct from every space a caller builds -- those 496 bytes
       are exactly what CGColorSpaceCreateLab hands out for D65. */
    bool hasWhitePoint;
    CGFloat whitePoint[3];
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
    /* The space's colorants as nine s15Fixed16, when the profile carries none.

       CGColorSpaceIsWideGamutRGB otherwise reads them out of the profile's
       'rXYZ', 'gXYZ' and 'bXYZ' tags, and that works for every calibrated
       space above.  It cannot work for the HDR profiles, which describe
       themselves as Display P3, Rec. ITU-R BT.709 or BT.2100 primaries in
       their description and then store no colorants at all: what they hold
       instead is a lut16Type whose matrix maps through the transfer function
       rather than being the primaries, and whose numbers are not the
       colorants even up to scale.  Apple calls those spaces wide or not wide
       exactly as it calls the gamma spaces of the same primaries -- the 2020
       and P3 HDR spaces are wide and the 709 ones are not -- so the primaries
       are kept beside the profile rather than inside it, as the range and the
       white point beside a Lab profile are.

       Not owned, and NULL for every space whose profile carries colorants. */
    const int32_t *primaries;
    /* An indexed space's lookup table, owned, and the largest index in it.
       NULL for every other space, which is what makes an indexed space
       distinguishable from a base-less one without consulting the model.
       The table is (last + 1) * (base's components) bytes and is copied out
       of the caller's buffer, so the caller's array need not outlive the
       call. */
    unsigned char *indexed;
    size_t indexedLen;
    size_t lastIndex;
};


/* The three device spaces.

   `name' is the ASCII form of the constant CFString Apple hands back.  It
   is spelled with the kCGColorSpace prefix that the name itself carries,
   i.e. the device RGB space is named "kCGColorSpaceDeviceRGB", not
   "Device RGB". */
/* The initializers are designated rather than positional: the struct carries
   several adjacent fields that are easy to mistype in a bare list, and a
   field added in the middle should not silently reinterpret one. */

static struct CGColorSpace CGColorSpaceDeviceGrayState = {
    .immortal = true,
    .model = kCGColorSpaceModelMonochrome,
    .type = CGColorSpaceTypeMonochrome,
    .ncomp = 1,
    .name = "kCGColorSpaceDeviceGray"
};

static struct CGColorSpace CGColorSpaceDeviceRGBState = {
    .immortal = true,
    .model = kCGColorSpaceModelRGB,
    .type = CGColorSpaceTypeRGB,
    .ncomp = 3,
    .name = "kCGColorSpaceDeviceRGB"
};

static struct CGColorSpace CGColorSpaceDeviceCMYKState = {
    .immortal = true,
    .model = kCGColorSpaceModelCMYK,
    .type = CGColorSpaceTypeCMYK,
    .ncomp = 4,
    .name = "kCGColorSpaceDeviceCMYK"
};

/* The generic Lab space, built on first use and then kept.

   Apple answers CGColorSpaceCreateWithName(kCGColorSpaceGenericLab) with the
   same pointer every time, and that pointer is an immortal one -- its retain
   count reads as the immortal marker and releasing it does nothing.  So this
   is a cached singleton like the device spaces, and not a fresh Lab space per
   call, even though the profile has to be assembled at runtime.  Until the
   first call this is null. */
static struct CGColorSpace *CGColorSpaceGenericLabState;

/* Declared ahead of its first use by CGColorSpaceCreateWithName: the sixteen
   names that resolve to the eleven ten-tag v4 profiles are built here, and the
   definition sits with the profile emitters.  It answers NULL for any other
   name, which is how CGColorSpaceCreateWithName falls through. */
static CGColorSpaceRef CGColorSpaceCreateNamedRGBV4(CFStringRef name);

/* The same arrangement for the two gray names, which resolve to a single v2.1
   profile of a rather different shape. */
static CGColorSpaceRef CGColorSpaceCreateNamedGrayV2(CFStringRef name);

/* And for the ten HDR names, which resolve to six profiles of a third shape:
   the PQ and HLG variants of the Display P3, Rec. ITU-R BT.709 and Rec. ITU-R
   BT.2100 primaries. */
static CGColorSpaceRef CGColorSpaceCreateNamedHDR(CFStringRef name);

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
   three device space names.

   The four second spellings below are not in this table, because
   CGColorSpaceNameFromID never hands one back; they are matched by
   CGColorSpaceIDFromName alone. */
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

/* Second spellings that CGColorSpaceIDFromName answers with an identifier even
   though the name itself is not one of the entries above.  Each of the four is
   another name for a space the table already carries under a sibling spelling,
   and each answers with that sibling's identifier rather than with one of its
   own: the 2020 PQ and HLG names reach the identifiers the table files under
   the 2100 spellings, and the two EOTF names reach the identifiers of the PQ
   spaces they are EOTF spellings of.

   Recovered by reading CGColorSpaceIDFromName over every constant reachable
   through the API and keeping the names that answered with an identifier they
   are not filed under.  The four are the only such names. */
static const struct {
    const char *name;
    int id;
} CGColorSpaceBuiltInAliases[] = {
    { "kCGColorSpaceITUR_2020_PQ", 28 },
    { "kCGColorSpaceITUR_2020_HLG", 29 },
    { "kCGColorSpaceDisplayP3_PQ_EOTF", 11 },
    { "kCGColorSpaceITUR_2020_PQ_EOTF", 28 }
};

#define CG_COLORSPACE_BUILT_IN_ALIAS_COUNT \
    ((int)(sizeof CGColorSpaceBuiltInAliases / sizeof CGColorSpaceBuiltInAliases[0]))

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
    /* The generic Lab space is a singleton too, so it is named by identity
       for the same reason: its name is a constant and not a copy. */
    if (s == CGColorSpaceGenericLabState)
        return CFSTR("kCGColorSpaceGenericLab");
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

/* Create a color space from one of its name constants.

   The lookup is an exact byte compare of the UTF-8 contents -- the same
   comparison CGColorSpaceIDFromName does, and for the same reason: going
   through CFStringCompare would be a locale-sensitive Unicode collation and
   so would accept spellings Apple's exact match refuses.  What is read is the
   contents and not the CFString's identity, which is observable: a mutable
   CFString holding "kCGColorSpaceDeviceRGB" resolves to device RGB just as an
   immutable constant does.

   A NULL name and the empty string both return NULL rather than faulting.

   Thirty-six of the fifty name constants resolve here, where Apple resolves
   forty-four.  The identifier table is not the set of names this accepts and is
   not even most of it: sweeping every constant reachable through the API gives
   50 distinct names, of which Apple's 44 resolve and six return NULL.  Of those
   44, the table's 32 all resolve under their own names, eight more resolve with
   no identifier at all, and four are second spellings that reach an identifier
   through a sibling -- the 2020 PQ and HLG names, and the two EOTF names.  The
   six that return NULL are Pattern, Unnamed, Invalid, and the three legacy
   GenericGamma2_2 / GenericCMYKLinear spellings.  The eight with no identifier
   are the three device names, the four generic profiles the table omits, and
   ColoredPattern -- so a lookup built from the identifier table alone would
   refuse eight names Apple accepts, among them kCGColorSpaceGenericGray, which
   reads as identifier 0 and so is indistinguishable from a name Apple has never
   heard of.  The eight names this step still refuses are the ones carrying a
   profile it cannot emit yet.

   The three device names resolve to the file-scope singletons, so the named
   space and the CreateDeviceX() space are one object: both return the same
   address, and two named calls do too. */

CGColorSpaceRef CGColorSpaceCreateWithName(CFStringRef name)
{
    /* Ordering is not observable here -- no two of these names are prefixes of
       each other, and the compare is exact -- so the device names come first
       only because they are the cheapest to answer. */
    if (CGColorSpaceNameEqualsASCII(name, "kCGColorSpaceDeviceGray"))
        return &CGColorSpaceDeviceGrayState;
    if (CGColorSpaceNameEqualsASCII(name, "kCGColorSpaceDeviceRGB"))
        return &CGColorSpaceDeviceRGBState;
    if (CGColorSpaceNameEqualsASCII(name, "kCGColorSpaceDeviceCMYK"))
        return &CGColorSpaceDeviceCMYKState;

    /* A pattern space is the one named space that is built rather than
       resolved, and every call builds a fresh one: two calls return different
       addresses at reference count 1, so the caller owns the result.  It is
       also not the object CGColorSpaceCreatePattern(NULL) returns -- different
       address again -- though the two agree on every observable: model 6, zero
       components, type 9, no base, no ICC data, no output support, and the
       name kCGColorSpaceColoredPattern.  Building it the same way is right
       because there is nothing left to distinguish. */
    if (CGColorSpaceNameEqualsASCII(name, CG_PATTERN_NAME_WITHOUT_BASE))
        return CGColorSpaceCreatePattern(NULL);

    /* GenericLab is the one named space whose profile this step can already
       produce, and it does so through CreateLab rather than through any new
       code: any white point a float cannot hold exactly produces the same
       496-byte profile, with the whole wtpt tag dropped rather than rounded,
       so D65 and D50 land on identical bytes.  Both were compared against the
       space the name resolves to and neither differs in a byte.

       Apple hands back a singleton, so the space is built once and kept.  Two
       further details come with that.  It is immortal, like the device spaces:
       its retain count reads as the immortal marker and releasing it is a
       no-op.  And it records no white point, which is what makes it unequal to
       every Lab space a caller builds -- the profile it carries is exactly the
       one CreateLab produces for D65, and Apple still calls the two different,
       so the white point the caller supplied has to be distinguished from the
       one this space does not have. */
    if (CGColorSpaceNameEqualsASCII(name, "kCGColorSpaceGenericLab")) {
        static const CGFloat d65[3] = { 0.9505, 1.0, 1.089 };
        struct CGColorSpace *s;

        if (CGColorSpaceGenericLabState)
            return CGColorSpaceGenericLabState;
        s = CGColorSpaceCreateLab(d65, NULL, NULL);
        if (!s)
            return NULL;
        s->hasWhitePoint = false;
        s->immortal = true;
        /* CreateLab leaves the space unnamed, since a space named for its
           white point has no business carrying a name, so the name is attached
           here.  It is a literal like the device spaces' rather than a copy,
           and the name field is not owned. */
        s->name = "kCGColorSpaceGenericLab";
        CGColorSpaceGenericLabState = s;
        return s;
    }

    /* Nineteen of the remaining names resolve to the fourteen profiles that
       share the RGB template, and CGColorSpaceCreateNamedRGBV4 answers for
       exactly those nineteen -- including the three extended-range aliases,
       which take the same profile bytes as their base and differ only in the
       name they report and the extended flag they carry. */
    {
        CGColorSpaceRef gray = CGColorSpaceCreateNamedGrayV2(name);
        CGColorSpaceRef hdr;

        /* Two more are gray rather than RGB, so they get their own v2.1
           template, and the extended one is an alias of the other in the same
           way.  Asked first because it is the shortest list of the three. */
        if (gray)
            return gray;
        /* Six more are HDR, in the PQ and HLG variants of the Display P3, 709
           and 2020 primaries, which share a third template between them. */
        hdr = CGColorSpaceCreateNamedHDR(name);
        if (hdr)
            return hdr;
    }

    /* The names no template owns fall through to the same NULL the other 14
       produce, of which five are names Apple refuses too. */
    return CGColorSpaceCreateNamedRGBV4(name);
}

/* Create an indexed color space.  `lastIndex' names the largest valid index,
   so the table holds lastIndex + 1 entries of base->ncomp bytes each -- the
   size Apple's header documents, and the one confirmed here by placing the
   table flush against a PROT_NONE page: a three-byte-per-entry table survives
   on an RGB base and faults on the fourth byte, a one-byte one survives on a
   gray base, and a four-byte one survives on CMYK.  There is no pad or alpha
   byte in front of an entry, which is the first thing worth getting wrong.

   The 256-entry ceiling is Apple's, and it is a property of the table rather
   than of the base: lastIndex 255 is accepted on a gray base and on a CMYK
   base alike, and 256 is refused on both. */
CGColorSpaceRef CGColorSpaceCreateIndexed(CGColorSpaceRef baseSpace,
    size_t lastIndex, const unsigned char *colorTable)
{
    struct CGColorSpace *s;
    struct CGColorSpace *base = baseSpace;
    size_t bytes;

    if (!base || !colorTable || lastIndex > 255)
        return NULL;
    /* An indexed space expands an index into the base's components, so the
       base has to be a space that has components to expand into -- and the
       component count has to mean what it says.  Two models report a count
       that would otherwise pass and cannot be expanded: a pattern space,
       which is a paint rather than a place a color can live (and which
       reports 3 on an RGB base, so its count looks ordinary), and an indexed
       space, which is already an index.  Both are refused. */
    if (base->model == kCGColorSpaceModelPattern
        || base->model == kCGColorSpaceModelIndexed)
        return NULL;
    bytes = (lastIndex + 1) * base->ncomp;
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->indexed = malloc(bytes ? bytes : 1);
    if (!s->indexed) {
        free(s);
        return NULL;
    }
    s->refcount = 1;
    s->model = kCGColorSpaceModelIndexed;
    s->type = CGColorSpaceTypeIndexed;
    /* One component, whatever the base has: the component of an indexed space
       is the index itself, which is why a gray-based one reports 1 as well as
       a CMYK-based one. */
    s->ncomp = 1;
    s->name = NULL;
    s->base = base;
    CGColorSpaceRetain(baseSpace);
    memcpy(s->indexed, colorTable, bytes);
    s->indexedLen = bytes;
    s->lastIndex = lastIndex;
    return s;
}

/* Declared ahead of its first use: the gray builder needs to repoint tag
   table entries, and the definition sits with the calibrated-RGB helpers. */
static void put_be32(unsigned char *p, int32_t v);
static void put_be16(unsigned char *p, unsigned v);

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
    /* The versions the profiles here declare: v2.1 and v2.2 for the three that
       predate the multi-localized string types, v4 for the rest. */
    CGICCVersionV21 = 0x02100000,
    CGICCVersionV22 = 0x02200000,
    CGICCVersionV4 = 0x04000000,
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

/* The RGB matrix/TRC template.

   Eight of the named RGB spaces resolve to profiles of one shape: a v4 header,
   ten tags and nothing else.  They are Display P3, the two ITU-R spaces, the
   BT.2020 space with an sRGB gamma, the Display P3 space with a 709 OETF,
   ROMM RGB, DCI P3 and ACES CG Linear, which between them the ten names
   because two of them have an extended-range alias.

   The template differs from the calibrated one above in three ways that all
   show in the tag table.  There is no black point, so the white point follows
   the colorants directly.  A 'chad' tag sits between the first tone curve and
   the last two, carrying the Bradford inverse that adapts the D50-stored
   colorants to the profile's own white point.  And the three tone curve tags
   all point at one shared block rather than at three separate curves, so the
   space's name costs one curve instead of three.

   The block that the curves share belongs to whichever component's tag the
   table lists first.  That is red in every space here except Display P3's 709
   OETF variant, which lists blue first -- a difference in the tag table
   alone, invisible in the bytes the two curves would have had. */
enum {
    /* The tag table's order is built one entry at a time below rather than
       counted from the loop index, because three separate things move it: the
       component that owns the shared curve block, whether the coding tag is
       present, and how many constant tags sit in the middle.  So the count
       recorded in the header is whatever that build produced -- ten without a
       coding tag, eleven with one, twelve for the space carrying two tags of
       its own.  What the table needs is an upper bound, and the widest case is
       every kind at once: six fixed tags, three tone curves, 'chad', 'cicp',
       and both constant tags. */
    CGICCRGBV4FixedTags = 6,    /* desc, cprt, wtpt, rXYZ, gXYZ, bXYZ */
    CGICCRGBV4CurveTags = 3,    /* rTRC, gTRC, bTRC, all sharing one block */
    /* Two constant tags is what the nineteen profiles below need between them
       -- CoreMedia709 carries both, every other space neither -- so two is the
       most the template allows rather than an arbitrary bound. */
    CGICCRGBV4MaxExtraTags = 2,
    CGICCRGBV4MaxTags = CGICCRGBV4FixedTags + CGICCRGBV4CurveTags + 2 /* chad, cicp */
        + CGICCRGBV4MaxExtraTags,
    /* 'sf32', four reserved bytes, then nine s15Fixed16. */
    CGICCRGBV4ChadLength = 44,
    /* 'cicp', four reserved bytes, then the four coding bytes. */
    CGICCRGBV4CicpLength = 12
};

/* Which component's tone curve tag owns the shared block. */
enum {
    CGICCRGBTRCRedFirst = 0,
    CGICCRGBTRCBlueFirst = 1
};

/* Where a space's constant tags sit in its tag table.  Nothing in the format
   decides this -- CoreMedia709 lists its two between the first tone curve and
   'chad', while GenericRGB's one is second, straight after the description --
   so the row says which of the two it is. */
enum {
    CGICCRGBV4ExtrasAfterCurve = 0,
    CGICCRGBV4ExtrasAfterDesc = 1
};

/* A tone curve as these profiles store one.

   A parametric curve is a 'para' block: four signature bytes, four reserved,
   the function type, two more reserved, then the parameters as s15Fixed16.
   The function type occupies two bytes and not the four the specification
   gives it, which is worth saying because a reader who assumes the wider
   field lands on the parameters and finds a plausible-looking number there.

   A simple curve is a 'curv' block instead: the signature, four reserved, a
   count of one as a four-byte field, and a u8Fixed8 gamma.  The count is a
   four-byte field here, unlike the two-byte function type above, so the two
   are not laid out alike even though both are curves. */
struct cgs_icc_curve {
    int parametric;
    int function;
    int gamma;
    int words;
    int32_t word[5];
};

/* One space's parameters.  The colorants, the Bradford inverse in 'chad' and
   the curve are all constants recovered from the profile the name resolves
   to, so nothing here is computed at run time. */
struct cgs_rgb_v4 {
    const char *desc;
    const char *cprt;
    /* The profile version, as the four-byte field the header carries.  Sixteen
       of the nineteen rows are v4 and leave this zero; CoreMedia709 and
       GenericRGBLinear are v2.1 and GenericRGB is v2.2, and each says so here.
       A version below 2.4 is the reason those three carry the legacy string
       types below, so the facts belong together. */
    int32_t version;
    /* Whether the strings are the v2 forms -- a legacy 'desc' and 'text' -- or
       the 'mluc' records the rest of the template uses. */
    int legacyStrings;
    /* Whether the 'desc' also carries a ScriptCode copy of its ASCII, which one
       profile does and the rest do not; see put_desc. */
    int descScriptCode;
    /* Whether the header's manufacturer field is the lower-case spelling, which
       three profiles use and the rest do not; see put_rgb_v4_header. */
    int lowercaseManufacturer;
    int created[6];
    int32_t wtpt[3];
    int32_t colorants[9];
    struct cgs_icc_curve trc;
    int32_t chad[9];
    /* Whether to leave 'chad' out entirely.  Seventeen of the spaces carry it
       and one does not, so this is spelled as the exception and defaults to
       emitting it: a row that forgets says so in the bytes rather than losing
       a tag quietly, which is the same reason the version above is a row's to
       give with zero meaning the common v4. */
    int omitChad;
    int trcOwner;
    /* Whether the profile carries a 'cicp' tag and a profile ID, which the
       two groups differ on together: a linearized space has both, and the
       other eight have neither. */
    int cicp;
    int32_t cicpValue[4];
    int profileId;
    /* Constant tags carried in the tag table, either stored whole or given as
       the translations to build an 'mluc' from.

       Stored whole, CoreMedia709's 'vcgt' and 'ndin': both blocks carry their
       own signature and their own length, and what they encode is not written
       down anywhere this file could check it against, so copying the block
       copies the signature the table points at and the length a reader inside
       the block agrees with.  Built instead, GenericRGB's 'dscm', whose thirty-
       one translations are the description itself rather than something whose
       meaning needs recovering.

       The two forms are told apart by which array the row filled, never both,
       and put_extra_tag is what reads either one. */
    const char *extraName[CGICCRGBV4MaxExtraTags];
    const unsigned char *extraData[CGICCRGBV4MaxExtraTags];
    const struct cgs_mluc_record *extraRecords[CGICCRGBV4MaxExtraTags];
    int32_t extraLen[CGICCRGBV4MaxExtraTags];
    int extraRecordCount[CGICCRGBV4MaxExtraTags];
    int extraCount;
    /* Whether those tags go straight after the description or after the first
       tone curve; see CGICCRGBV4ExtrasAfterDesc. */
    int extrasWhere;
};

/* Round up to the next four-byte boundary.  Tag data is stored aligned, and
   the pad counts toward the profile size while belonging to no tag. */
static size_t icc_pad(size_t off)
{
    return (off + 3) & ~(size_t)3;
}

/* One translation: the four-byte language and country code the record names,
   and the string as UTF-8. */
struct cgs_mluc_record {
    const char *code;
    const char *text;
};

/* The most records any 'mluc' here holds, which is the description of the
   generic RGB space: thirty-one languages, no two of the other profiles
   carrying anything like as many. */
enum { CGICCMlucMaxRecords = 31 };

/* One UTF-8 string as big-endian UTF-16 at p, returning its length in bytes.

   Every character in the translations below is inside the basic plane, so a
   writer that emitted only the BMP would produce the right answer for all of
   them -- but a code point outside it is written as the surrogate pair it needs
   rather than truncated to something in range, so the conversion is right for
   the input it is actually given. */
static size_t put_utf16(unsigned char *p, const char *utf8)
{
    const unsigned char *s = (const unsigned char *)utf8;
    size_t n = 0;

    while (*s) {
        unsigned c = *s++;
        uint32_t cp;
        size_t extra;

        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1fu;
            extra = 1;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0fu;
            extra = 2;
        } else {
            cp = c & 0x07u;
            extra = 3;
        }
        while (extra--)
            cp = (cp << 6) | (uint32_t)(*s++ & 0x3f);
        if (cp < 0x10000) {
            put_be16(p + n, (unsigned)cp);
            n += 2;
        } else {
            cp -= 0x10000;
            put_be16(p + n, (unsigned)(0xd800 + (cp >> 10)));
            put_be16(p + n + 2, (unsigned)(0xdc00 + (cp & 0x3ff)));
            n += 4;
        }
    }
    return n;
}

/* The length the string above would occupy, without writing it.  Two passes
   over the same string rather than one that counts and writes together,
   because the caller has to know how long every block is before any of them is
   allocated. */
static size_t utf16_length(const char *utf8)
{
    const unsigned char *s = (const unsigned char *)utf8;
    size_t n = 0;

    while (*s) {
        unsigned c = *s++;

        if (c < 0x80) {
            n += 2;
        } else if ((c & 0xe0) == 0xc0) {
            s += 1;
            n += 2;
        } else if ((c & 0xf0) == 0xe0) {
            s += 2;
            n += 2;
        } else {
            /* Four bytes can carry a code point outside the basic plane, which
               is two UTF-16 units rather than one. */
            s += 3;
            n += 4;
        }
    }
    return n;
}

/* An 'mluc' block holding one record per entry, in the order given.

   The header is the signature, four reserved bytes, the record count and the
   record size; the records follow it, twelve bytes each, and each carries a
   language code, a length in bytes of UTF-16, and an offset counted from the
   start of the tag.  The strings follow the records.

   A string that repeats an earlier one takes that string's offset instead of a
   second copy of it, which is why the offsets need not rise with the record
   order: two of GenericRGB's translations are the same text as another
   language's, Swedish as Norwegian and Spanish as European Portuguese.  That
   also means the offsets cannot be written as they are worked out, so the
   records are walked twice -- once to place every string and remember where,
   once to write the block out.

   A null p measures the block instead of writing it, which is what the profile
   layout needs: the tag offsets are assigned before anything is allocated, so
   every length has to be known first. */
static size_t put_mluc_records(unsigned char *p, const struct cgs_mluc_record *r,
    int n)
{
    int32_t off[CGICCMlucMaxRecords], bytes[CGICCMlucMaxRecords];
    size_t len = 16 + 12 * (size_t)n;
    int i, k;

    for (i = 0; i < n; i++) {
        int shared = 0;

        bytes[i] = (int32_t)utf16_length(r[i].text);
        off[i] = (int32_t)len;
        for (k = 0; k < i; k++) {
            if (strcmp(r[i].text, r[k].text))
                continue;
            off[i] = off[k];
            shared = 1;
            break;
        }
        if (!shared)
            len += (size_t)bytes[i];
    }

    if (p) {
        memcpy(p, "mluc", 4);
        put_be32(p + 4, 0);
        put_be32(p + 8, (uint32_t)n);
        put_be32(p + 12, 12);
        for (i = 0; i < n; i++) {
            unsigned char *e = p + 16 + 12 * i;

            memcpy(e, r[i].code, 4);
            put_be32(e + 4, bytes[i]);
            put_be32(e + 8, off[i]);
            /* Written for every record, shared or not: the two are the same
               bytes at the same place, and testing for the repeat again here
               would only risk disagreeing with the pass above. */
            put_utf16(p + off[i], r[i].text);
        }
    }
    return len;
}

/* A 'mluc' block holding one en-US string, which is what the template's own two
   strings are.  With a single record there is nothing to share, so the offset
   is simply where the string lands: twenty-eight bytes in, after the sixteen
   byte header and the one twelve byte record.  Returns the length of the whole
   block. */
static size_t put_mluc(unsigned char *p, const char *utf8)
{
    struct cgs_mluc_record record = { "enUS", utf8 };

    return put_mluc_records(p, &record, 1);
}

/* One constant tag, as a length or as the block itself: a stored block is
   copied whole, and a list of translations is built into an 'mluc'.  Which of
   the two a tag is shows in which array the row filled.  Returns the length,
   and writes nothing at all when p is null, for the reason put_mluc_records
   gives. */
static size_t put_extra_tag(unsigned char *p, const struct cgs_rgb_v4 *v, int i)
{
    if (v->extraRecords[i])
        return put_mluc_records(p, v->extraRecords[i], v->extraRecordCount[i]);
    if (p)
        memcpy(p, v->extraData[i], (size_t)v->extraLen[i]);
    return (size_t)v->extraLen[i];
}

/* 'text', the tag the copyright uses in a v2 profile where the v4 template
   uses an 'mluc'.  It is a signature, four reserved bytes, and then the string
   with its terminator and nothing else -- no length field, so the block is
   exactly eight bytes longer than the text. */
static size_t put_text(unsigned char *p, const char *ascii)
{
    size_t n = strlen(ascii);

    memcpy(p, "text", 4);
    put_be32(p + 4, 0);
    memcpy(p + 8, ascii, n + 1);
    return 8 + n + 1;
}

/* 'desc', the description type a v2 profile uses in place of an 'mluc'.

   The length in the header counts the terminator, so an eleven-character name
   records twelve and occupies twelve bytes, and the ASCII half is not padded
   out to the sixty-seven bytes its type nominally allows -- the Unicode half
   starts immediately after.  What follows it is the Unicode language code, the
   Unicode count, the ScriptCode code, the ScriptCode description and a final
   count byte, and the first four of those are zero on every profile here: the
   description is ASCII and has no Unicode form.

   The ScriptCode half is where the profiles differ.  Most leave it zero to the
   end, and one -- the generic RGB space -- writes the same ASCII into it behind
   a count of twenty, so the block is a byte longer than the empty form and the
   count sits in front of the description rather than only after it.  The count
   includes the terminator, as the ASCII count above does, and the description
   is still padded out to sixty-seven. */
static size_t put_desc(unsigned char *p, const char *ascii, int scriptCode)
{
    size_t n = strlen(ascii);
    unsigned char *q = p + 12 + n + 1;

    memcpy(p, "desc", 4);
    put_be32(p + 4, 0);
    put_be32(p + 8, (int32_t)(n + 1));
    memcpy(p + 12, ascii, n + 1);
    memset(q, 0, 4 + 4 + 2);
    q += 4 + 4 + 2;
    if (scriptCode) {
        *q++ = (unsigned char)(n + 1);
        memcpy(q, ascii, n + 1);
        q += n + 1;
        memset(q, 0, 67 - (n + 1));
    } else {
        memset(q, 0, 67);
    }
    q += 67;
    *q = 0;
    return (size_t)(q + 1 - p);
}

/* One tone curve, of either kind.  Returns the length of the block. */
static size_t put_icc_curve(unsigned char *p, const struct cgs_icc_curve *c)
{
    int i;

    if (!c->parametric) {
        memcpy(p, "curv", 4);
        put_be32(p + 4, 0);
        put_be32(p + 8, 1);
        put_be16(p + 12, (unsigned)c->gamma);
        return 14;
    }
    memcpy(p, "para", 4);
    put_be32(p + 4, 0);
    put_be16(p + 8, (unsigned)c->function);
    put_be16(p + 10, 0);
    for (i = 0; i < c->words; i++)
        put_be32(p + 12 + 4 * i, c->word[i]);
    return 12 + 4 * c->words;
}

/* The 'chad' block: the Bradford inverse as nine s15Fixed16.  It is an s15Fixed16
   array type rather than a matrix type, so it carries its own signature and
   reserved bytes and holds no tag-style colourants. */
/* Four reserved bytes then the four coding bytes: which primaries, which
   transfer, which matrix, and whether the range is full.  All three spaces
   here are identity-matrix and full-range, so only the primaries differ. */
static void put_cicp(unsigned char *p, const int32_t v[4])
{
    memcpy(p, "cicp", 4);
    put_be32(p + 4, 0);
    p[8] = (unsigned char)v[0];
    p[9] = (unsigned char)v[1];
    p[10] = (unsigned char)v[2];
    p[11] = (unsigned char)v[3];
}

static void put_chad(unsigned char *p, const int32_t v[9])
{
    int i;

    memcpy(p, "sf32", 4);
    put_be32(p + 4, 0);
    for (i = 0; i < 9; i++)
        put_be32(p + 8 + 4 * i, v[i]);
}

/* The ICC header, which the v4 RGB template and the v2.1 gray one share.

   Every field is written out rather than copied from a transcribed template, so
   the two that vary -- the size and the creation date -- are patched last and
   the rest are visible at their offsets.  The profile ID is left zero, which
   for eight of the eleven v4 profiles is not a placeholder: they carry no
   digest, and the three that do have put_profile_id fill it in afterwards.

   The illuminant is three bare s15Fixed16 with no 'XYZ ' signature in front of
   them, the one XYZ triple in a profile that is not a tag.  Only the version
   and the colour space signature separate the two templates: the gray profile
   is v2.1 rather than v4 and describes a 'GRAY' space, though like the RGB
   ones it has an XYZ PCS and every other field to match. */
static void put_icc_header(unsigned char *p, size_t len, const int created[6],
    int32_t version, const char *space)
{
    int i;

    put_be32(p + CGICCProfileSizeOffset, (int32_t)len);
    memcpy(p + 4, "appl", 4);
    put_be32(p + CGICCVersionOffset, version);
    memcpy(p + CGICCDeviceClassOffset, "mntr", 4);
    memcpy(p + CGICCColorSpaceOffset, space, 4);
    memcpy(p + CGICCColorSpaceOffset + 4, "XYZ ", 4);
    for (i = 0; i < 6; i++)
        put_be16(p + 24 + 2 * i, (unsigned)created[i]);
    memcpy(p + CGICCSignatureOffset, "acsp", 4);
    memcpy(p + 40, "APPL", 4);
    memcpy(p + 48, "APPL", 4);
    put_be32(p + 68, 0x0000f6d6);
    put_be32(p + 72, 0x00010000);
    put_be32(p + 76, 0x0000d32d);
    memcpy(p + 80, "appl", 4);
}

/* The header for an RGB space on this template.  Sixteen of the nineteen
   profiles are v4, and CoreMedia709, GenericRGBLinear and GenericRGB are v2.1
   and v2.2 -- which is why they carry the legacy string types, and is the
   reason the version is a row's to give rather than a constant here. */
static void put_rgb_v4_header(unsigned char *p, size_t len, const int created[6],
    int32_t version, int lowercaseManufacturer)
{
    put_icc_header(p, len, created, version, "RGB ");
    /* Most profiles here give their manufacturer as the four upper-case bytes
       the CMM signature uses, and three give the same four characters lower
       case.  Nothing about the format requires either, and the two spellings
       are 0x20 apart in one header field, so the row says which it wants. */
    if (lowercaseManufacturer)
        memcpy(p + 48, "appl", 4);
}

/* Assemble the profile for one of these spaces.  The blocks are laid out one
   after another -- the two strings, the white point, the three colorants, the
   shared curve, the Bradford inverse, and any tags the space carries that no
   other one does -- and the tag table is pointed at them afterwards, which is
   what lets the three curve tags share one block.  The number of tags is not
   fixed: the ten the rest carry, eleven when a 'cicp' says what the
   description leaves out, and twelve for the one space that brings two tags of
   its own. */
static unsigned char *put_rgb_v4(const struct cgs_rgb_v4 *v, size_t *outLen)
{
    /* The constant tags take their offsets from just past the named blocks
       rather than from a slot among them, so that adding one cannot collide
       with a block name. */
    enum {
        desc, cprt, wtpt, rXYZ, gXYZ, bXYZ, curve, chad, cicp, blockCount,
        extra = blockCount
    };
    static const char *const fixed[6] = {
        "desc", "cprt", "wtpt", "rXYZ", "gXYZ", "bXYZ"
    };
    static const char *const trcOrder[2][3] = {
        { "rTRC", "bTRC", "gTRC" },
        { "bTRC", "rTRC", "gTRC" }
    };
    unsigned char *p;
    size_t dlen, clen, tlen, len;
    /* Room for the blocks above plus the constant tags, whose offsets sit
       immediately after 'extra' so that the loop below can index them. */
    int32_t tagOff[blockCount + CGICCRGBV4MaxExtraTags];
    int32_t tagLen[blockCount + CGICCRGBV4MaxExtraTags];
    /* The tag table as an explicit order rather than as arithmetic on the loop
       index: the two strings, the white point, the colorants in red, green,
       blue order, the first tone curve, any constant tags, 'chad', an optional
       'cicp', and the other two curves.  Three things vary, and each of them
       moves the tags after it -- which component owns the shared curve block,
       whether the coding tag is present, and how many constant tags sit in the
       middle -- so positions counted off by hand are what a fixed index gets
       wrong.  Built first, and its length is the tag count, so the count the
       header records and the table written below cannot come apart. */
    const char *name[CGICCRGBV4MaxTags];
    int block[CGICCRGBV4MaxTags];
    int n = 0, i, k;

    /* The first six blocks are the six fixed tags, in the same order -- except
       that a row putting its constant tags after the description has the first
       of them next, before the copyright rather than among the rest. */
    for (i = 0; i < CGICCRGBV4FixedTags; i++) {
        name[n] = fixed[i];
        block[n] = i;
        n++;
        if (i == desc && v->extrasWhere == CGICCRGBV4ExtrasAfterDesc) {
            for (k = 0; k < v->extraCount; k++) {
                name[n] = v->extraName[k];
                block[n++] = extra + k;
            }
        }
    }
    name[n] = trcOrder[v->trcOwner][0];
    block[n++] = curve;
    if (v->extrasWhere == CGICCRGBV4ExtrasAfterCurve) {
        for (i = 0; i < v->extraCount; i++) {
            name[n] = v->extraName[i];
            block[n++] = extra + i;
        }
    }
    if (!v->omitChad) {
        name[n] = "chad";
        block[n++] = chad;
    }
    if (v->cicp) {
        name[n] = "cicp";
        block[n++] = cicp;
    }
    name[n] = trcOrder[v->trcOwner][1];
    block[n++] = curve;
    name[n] = trcOrder[v->trcOwner][2];
    block[n++] = curve;

    /* The two string forms size themselves from the string they carry, so in
       either case the two lengths are worked out here rather than written
       down. */
    if (v->legacyStrings) {
        dlen = 12 + (strlen(v->desc) + 1) + 4 + 4 + 2
            + (v->descScriptCode ? 1 : 0) + 67 + 1;
        clen = 8 + strlen(v->cprt) + 1;
    } else {
        dlen = 28 + 2 * strlen(v->desc);
        clen = 28 + 2 * strlen(v->cprt);
    }
    tlen = v->trc.parametric ? 12 + 4 * (size_t)v->trc.words : 14;

    /* Lay the blocks out first so the total size is known before anything is
       allocated.  The offsets are assigned here and used again below, so the
       two passes cannot disagree about where a block went. */
    len = CGICCTagTableOffset + (size_t)n * CGICCTagEntrySize;
    tagLen[desc] = (int32_t)dlen;
    tagOff[desc] = (int32_t)icc_pad(len);
    len = icc_pad(len) + dlen;
    /* The blocks are laid out in the order the tag table lists them, so a row
       putting its constant tags after the description has them take their
       offsets here rather than below. */
    if (v->extrasWhere == CGICCRGBV4ExtrasAfterDesc) {
        for (i = 0; i < v->extraCount; i++) {
            size_t elen = put_extra_tag(NULL, v, i);

            tagLen[extra + i] = (int32_t)elen;
            tagOff[extra + i] = (int32_t)icc_pad(len);
            len = icc_pad(len) + elen;
        }
    }
    tagLen[cprt] = (int32_t)clen;
    tagOff[cprt] = (int32_t)icc_pad(len);
    len = icc_pad(len) + clen;
    for (i = wtpt; i <= bXYZ; i++) {
        tagLen[i] = CGICCXYZLength;
        tagOff[i] = (int32_t)icc_pad(len);
        len = icc_pad(len) + CGICCXYZLength;
    }
    tagLen[curve] = (int32_t)tlen;
    tagOff[curve] = (int32_t)icc_pad(len);
    len = icc_pad(len) + tlen;
    /* Or between the first curve and 'chad', which is where CoreMedia709 lists
       its two. */
    if (v->extrasWhere == CGICCRGBV4ExtrasAfterCurve) {
        for (i = 0; i < v->extraCount; i++) {
            size_t elen = put_extra_tag(NULL, v, i);

            tagLen[extra + i] = (int32_t)elen;
            tagOff[extra + i] = (int32_t)icc_pad(len);
            len = icc_pad(len) + elen;
        }
    }
    if (!v->omitChad) {
        tagLen[chad] = CGICCRGBV4ChadLength;
        tagOff[chad] = (int32_t)icc_pad(len);
        len = icc_pad(len) + CGICCRGBV4ChadLength;
    }
    if (v->cicp) {
        tagLen[cicp] = CGICCRGBV4CicpLength;
        tagOff[cicp] = (int32_t)icc_pad(len);
        len = icc_pad(len) + CGICCRGBV4CicpLength;
    }

    /* The profile is padded out to a four-byte boundary.  Every block after
       the first already is, so this has gone unnoticed until a profile whose
       last block ended a byte or two short of one came along -- the recorded
       size is the padded length, so it has to happen before the header is
       written. */
    len = icc_pad(len);

    p = calloc(1, len);
    if (!p)
        return NULL;

    put_rgb_v4_header(p, len, v->created, v->version ? v->version : CGICCVersionV4,
        v->lowercaseManufacturer);
    put_be32(p + CGICCTagCountOffset, (uint32_t)n);

    for (i = 0; i < n; i++) {
        unsigned char *e = p + CGICCTagTableOffset + i * CGICCTagEntrySize;

        memcpy(e, name[i], 4);
        put_be32(e + 4, tagOff[block[i]]);
        put_be32(e + 8, tagLen[block[i]]);
    }

    if (v->legacyStrings) {
        put_desc(p + tagOff[desc], v->desc, v->descScriptCode);
        put_text(p + tagOff[cprt], v->cprt);
    } else {
        put_mluc(p + tagOff[desc], v->desc);
        put_mluc(p + tagOff[cprt], v->cprt);
    }
    put_xyz_i32(p + tagOff[wtpt], v->wtpt);
    for (i = 0; i < 3; i++)
        put_xyz_i32(p + tagOff[rXYZ + i], v->colorants + 3 * i);
    put_icc_curve(p + tagOff[curve], &v->trc);
    for (i = 0; i < v->extraCount; i++)
        put_extra_tag(p + tagOff[extra + i], v, i);
    if (!v->omitChad)
        put_chad(p + tagOff[chad], v->chad);
    if (v->cicp)
        put_cicp(p + tagOff[cicp], v->cicpValue);
    if (v->profileId)
        put_profile_id(p, len);

    *outLen = len;
    return p;
}

/* These spaces.  Each row was recovered from the profile its name resolves
   to, and put_rgb_v4 was checked against every one of them by rebuilding each
   profile and comparing it byte for byte with the one Apple hands back.

   Two details are visible in the tag table rather than in the values, so they
   are worth stating here.  All three tone curve tags point at one shared block
   and that block belongs to whichever component's tag comes first -- red in
   every space here except DisplayP3_709OETF, which lists blue first.  And
   'wtpt' sits one quantisation unit below the header illuminant in seven of
   the eight; Rec. ITU-R BT.2020-1 is the one whose white point equals the
   illuminant. */

/* The eight profiles that share the ten-tag v4 template, one row each.

   Every field below was recovered from the profile its name resolves to,
   and the emitter that consumes these rows was checked against all eight
   by rebuilding each profile and comparing it byte for byte.  Nothing here
   is computed at runtime: the colorants, the Bradford inverse that 'chad'
   holds and the tone curve parameters are all constants, so the table is a
   few hundred bytes of data rather than a second colorimetry
   implementation.

   Two details are worth naming because they show in the tag table rather
   than in the values.  All three tone curve tags point at one shared
   block, and that block belongs to whichever component's tag is listed
   first -- red for every space here except DisplayP3_709OETF, which lists
   blue first.  And 'wtpt' sits one quantisation unit below the header
   illuminant for seven of the eight; Rec. ITU-R BT.2020-1 is the one whose
   white point equals the illuminant. */

static const struct cgs_rgb_v4 DisplayP3 = {
    .desc = "Display P3",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2022",
    .wtpt = { 0xf6d5, 0x10000, 0xd32c },
    .colorants = { 0x83df, 0x3dbf, -0x45, 0x4abf, 0xb137, 0xab9, 0x2838, 0x110b, 0xc8b9 },
    .trc = { 1, 3, 0, 5, { 0x26666, 0xf2a7, 0xd59, 0x13d0, 0xa5b } },  /* parametric curve, function type 3 */
    .chad = { 0x10c42, 0x5de, -0xcda, 0x793, 0xfd90, -0x45e, -0x25d, 0x3dc, 0xc06e },
};
static const struct cgs_rgb_v4 ITUR_709 = {
    .desc = "Rec. ITU-R BT.709-5",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2022",
    .wtpt = { 0xf6d5, 0x10000, 0xd32c },
    .colorants = { 0x6fa2, 0x38f5, 0x390, 0x6299, 0xb785, 0x18da, 0x24a0, 0xf84, 0xb6cf },
    .trc = { 1, 3, 0, 5, { 0x238e4, 0xe8f0, 0x1710, 0x38e4, 0x14bc } },  /* parametric curve, function type 3 */
    .chad = { 0x10c42, 0x5de, -0xcda, 0x793, 0xfd90, -0x45e, -0x25d, 0x3dc, 0xc06e },
};
static const struct cgs_rgb_v4 ITUR_2020 = {
    .desc = "Rec. ITU-R BT.2020-1",
    .created = { 2023, 6, 9, 9, 54, 38 },
    .cprt = "Copyright Apple Inc., 2023",
    .wtpt = { 0xf6d6, 0x10000, 0xd32d },
    .colorants = { 0xac69, 0x476f, -0x7f, 0x2a69, 0xace3, 0x7ad, 0x2003, 0xbad, 0xcbfe },
    .trc = { 1, 3, 0, 5, { 0x238e4, 0xe8e0, 0x1720, 0x38e4, 0x14bc } },  /* parametric curve, function type 3 */
    .chad = { 0x10c42, 0x5de, -0xcda, 0x793, 0xfd90, -0x45e, -0x25d, 0x3dc, 0xc06e },
};
static const struct cgs_rgb_v4 ITUR_2020_sRGBGamma = {
    .desc = "Rec. ITU-R BT.2020-1; sRGB Gamma",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2022",
    .wtpt = { 0xf6d5, 0x10000, 0xd32c },
    .colorants = { 0xac69, 0x476f, -0x7f, 0x2a69, 0xace3, 0x7ad, 0x2003, 0xbad, 0xcbfe },
    .trc = { 1, 3, 0, 5, { 0x26666, 0xf2a7, 0xd59, 0x13d0, 0xa5b } },  /* parametric curve, function type 3 */
    .chad = { 0x10c42, 0x5de, -0xcda, 0x793, 0xfd90, -0x45e, -0x25d, 0x3dc, 0xc06e },
};
/* The three linearized spaces differ from their gamma counterparts in four
   ways and share the rest: the same colorants, the same 'chad', an identity
   tone curve, a 'cicp' tag, and a profile ID. */
static const struct cgs_rgb_v4 LinearSRGB = {
    .desc = "sRGB IEC61966-2.1 Linear",
    .created = { 2019, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2019",
    .wtpt = { 0xf6d6, 0x10000, 0xd32d },
    .colorants = { 0x6fa2, 0x38f5, 0x390, 0x6299, 0xb785, 0x18da, 0x24a0, 0xf84, 0xb6cf },
    .trc = { 1, 0, 0, 1, { 0x10000 } },  /* parametric curve, function type 0, one word of 1.0 */
    .chad = { 0x10c42, 0x5de, -0xcda, 0x793, 0xfd90, -0x45e, -0x25d, 0x3dc, 0xc06e },
    .cicp = 1,
    .cicpValue = { 1, 8, 0, 1 },
    .profileId = 1,
};
static const struct cgs_rgb_v4 LinearDisplayP3 = {
    .desc = "Display P3 Linear",
    .created = { 2019, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2019",
    .wtpt = { 0xf6d6, 0x10000, 0xd32d },
    .colorants = { 0x83df, 0x3dbf, -0x45, 0x4abf, 0xb137, 0xab9, 0x2838, 0x110b, 0xc8b9 },
    .trc = { 1, 0, 0, 1, { 0x10000 } },  /* parametric curve, function type 0, one word of 1.0 */
    .chad = { 0x10c42, 0x5de, -0xcda, 0x793, 0xfd90, -0x45e, -0x25d, 0x3dc, 0xc06e },
    .cicp = 1,
    .cicpValue = { 12, 8, 0, 1 },
    .profileId = 1,
};
static const struct cgs_rgb_v4 LinearITUR_2020 = {
    .desc = "Rec. ITU-R BT.2020-1 Linear",
    .created = { 2019, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2019",
    .wtpt = { 0xf6d6, 0x10000, 0xd32d },
    .colorants = { 0xac69, 0x476f, -0x7f, 0x2a69, 0xace3, 0x7ad, 0x2003, 0xbad, 0xcbfe },
    .trc = { 1, 0, 0, 1, { 0x10000 } },  /* parametric curve, function type 0, one word of 1.0 */
    .chad = { 0x10c42, 0x5de, -0xcda, 0x793, 0xfd90, -0x45e, -0x25d, 0x3dc, 0xc06e },
    .cicp = 1,
    .cicpValue = { 9, 8, 0, 1 },
    .profileId = 1,
};
static const struct cgs_rgb_v4 DisplayP3_709OETF = {
    .desc = "Display P3; ITU-R 709 OETF",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2022",
    .wtpt = { 0xf6d5, 0x10000, 0xd32c },
    .colorants = { 0x83df, 0x3dbf, -0x45, 0x4abf, 0xb137, 0xab9, 0x2838, 0x110b, 0xc8b9 },
    .trc = { 1, 3, 0, 5, { 0x238e4, 0xe8f0, 0x1710, 0x38e4, 0x14bc } },  /* parametric curve, function type 3 */
    .chad = { 0x10c42, 0x5de, -0xcda, 0x793, 0xfd90, -0x45e, -0x25d, 0x3dc, 0xc06e },
    .trcOwner = CGICCRGBTRCBlueFirst,
};
static const struct cgs_rgb_v4 ROMMRGB = {
    .desc = "ROMM RGB: ISO 22028-2:2013",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2022",
    .wtpt = { 0xf6d5, 0x10000, 0xd32c },
    .colorants = { 0xcc34, 0x49bd, 0x0, 0x229c, 0xb63e, 0x0, 0x807, 0x6, 0xd340 },
    .trc = { 1, 3, 0, 5, { 0x1cccd, 0x10000, 0x0, 0x1000, 0x80 } },  /* parametric curve, function type 3 */
    .chad = { 0x10000, 0x0, 0x0, 0x0, 0x10000, 0x0, 0x0, 0x0, 0x10000 },
};
static const struct cgs_rgb_v4 DCIP3 = {
    .desc = "SMPTE RP 431-2-2007 DCI (P3)",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2022",
    .wtpt = { 0xf6d5, 0x10000, 0xd32c },
    .colorants = { 0x7c75, 0x3a08, -0x35, 0x52e8, 0xb5d8, 0xb11, 0x2779, 0x1020, 0xc850 },
    .trc = { 1, 0, 0, 1, { 0x2999a } },  /* parametric curve, function type 0 */
    .chad = { 0x112e6, 0x9ef, -0x972, 0xe3a, 0xf6c8, -0x3ac, -0x118, 0x15b, 0xdcdf },
};
static const struct cgs_rgb_v4 ACESCGLinear = {
    .desc = "ACES CG Linear (Academy Color Encoding System AP1)",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2022",
    .wtpt = { 0xf6d5, 0x10000, 0xd32c },
    .colorants = { 0xb09c, 0x48d6, -0x18c, 0x2657, 0xabf4, 0x290, 0x1fe3, 0xb36, 0xd229 },
    .trc = { 0, 0, 256, 0, { 0x0 } },  /* u8Fixed8 gamma */
    .chad = { 0x108bf, 0x44e, -0x997, 0x589, 0xfe03, -0x341, -0x1c6, 0x2e6, 0xd021 },
};

/* The two constant tags in CoreMedia709, stored whole and copied verbatim.

   Both blocks carry their own signature and their own length, which is what
   makes them worth keeping as bytes rather than as something to rebuild: the
   signature is what the tag table points at, and the length is what a reader
   inside the block agrees with.  Copying the block copies both.

   'vcgt' is a video characteristics tag and 'ndin' a natural dimming one, and
   what they encode is not written down anywhere this file could check it
   against -- there is no Apple API that reads either back.  So the numbers are
   recorded rather than interpreted.  Both are here because CoreMedia709 is the
   one profile among these that carries them, and they are what make it twelve
   tags where the rest are ten or eleven. */
static const unsigned char CoreMedia709VCGT[48] = {
    /* 'vcgt', four reserved, then six pairs of flags. */
    0x76, 0x63, 0x67, 0x74, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
};
static const unsigned char CoreMedia709NDIN[62] = {
    /* 'ndin', four reserved, a length of 54, then the block itself. */
    0x6e, 0x64, 0x69, 0x6e, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x36,
    0x00, 0x00, 0xa3, 0xd7, 0x00, 0x00, 0x54, 0x7b,
    0x00, 0x00, 0x4c, 0xcd, 0x00, 0x00, 0x99, 0x9a,
    0x00, 0x00, 0x26, 0x66, 0x00, 0x00, 0x0f, 0x5c,
    0x00, 0x00, 0x50, 0x0d, 0x00, 0x00, 0x54, 0x39,
    0x00, 0x01, 0xf6, 0x04, 0x00, 0x01, 0xf6, 0x04,
    0x00, 0x01, 0xf6, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};

/* CoreMedia709 is one of three profiles on this template that are not v4: its
   header declares v2.1, which is why it carries the legacy 'desc' and 'text'
   strings rather than 'mluc', and it is the only one with two tags in the
   middle of the table.  Its white point is not quantised to the
   value the others share, and its copyright is the one of the nineteen that
   does not follow the 'Copyright Apple Inc., <year>' form.

   What it does share is worth as much: the BT.709 colorants to the last bit,
   the Bradford inverse, and a header illuminant equal to the D50 the rest use.
   Its tone curve is a u8Fixed8 gamma of 502/256, which is not BT.709's 2.4 --
   the space is named for the primaries, and the transfer function is a
   separate question the profile answers on its own. */
static const struct cgs_rgb_v4 CoreMedia709 = {
    .desc = "HDTV",
    .version = CGICCVersionV21,
    .legacyStrings = 1,
    .created = { 2005, 4, 1, 1, 1, 1 },
    .cprt = "Copyright 2007 Apple Inc.",
    .wtpt = { 0xf351, 0x10000, 0x116cc },
    .colorants = { 0x6fa2, 0x38f5, 0x390, 0x6299, 0xb785, 0x18da, 0x24a0, 0xf84, 0xb6cf },
    .trc = { 0, 0, 502, 0, { 0x0 } },  /* u8Fixed8 gamma, 502/256 */
    .chad = { 0x10c42, 0x5de, -0xcda, 0x793, 0xfd90, -0x45e, -0x25d, 0x3dc, 0xc06e },
    .extraName = { "vcgt", "ndin" },
    .extraData = { CoreMedia709VCGT, CoreMedia709NDIN },
    .extraLen = { sizeof CoreMedia709VCGT, sizeof CoreMedia709NDIN },
    .extraCount = 2,
};

/* GenericRGBLinear is the second of the three profiles here that are not v4,
   and shares CoreMedia709's legacy string types rather than the v4 'mluc'
   records.  It is the only profile on this template carrying no 'chad' at all
   -- nine tags where CoreMedia709 has twelve.  Its creation
   date is the most recent of any profile in this file, 2025 against the 2023 of
   the BT.2020 space next door, and its copyright follows the
   'Copyright Apple Inc., <year>' form every profile but CoreMedia709 uses.

   The name is the interesting part.  Its tone curve is a u8Fixed8 gamma of
   256/256 -- the identity, written out as a gamma rather than left as the
   zero-count form the same identity could take -- so the space really is
   linear, and needs no 'cicp' tag to say what the three linearized spaces say
   with one.  What distinguishes it from kCGColorSpaceGenericRGB is therefore
   its colorants rather than its transfer function.  Those are close to the
   other space's and not the same: red moves 0x3dee to 0x3e1b and green 0xac73
   to 0xaca5, so the linear space is the more saturated of the two by a little,
   which is not the direction the name alone would suggest -- and which is still
   not far enough for Apple to call either of them wide gamut.

   Its white point is the header illuminant itself, one of three values the
   template uses, where CoreMedia709's and GenericRGB's are quantised
   somewhere else entirely.  All three tone curves share one block. */
static const struct cgs_rgb_v4 GenericRGBLinear = {
    .desc = "Generic RGB Linear Profile",
    .version = CGICCVersionV21,
    .legacyStrings = 1,
    .created = { 2025, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2025",
    .wtpt = { 0xf6d6, 0x10000, 0xd32d },
    .colorants = { 0x744d, 0x3e1b, 0x3ca, 0x5a72, 0xaca5, 0x1724, 0x2817, 0x155a, 0xb831 },
    .trc = { 0, 0, 256, 0, { 0x0 } },  /* u8Fixed8 gamma of 1.0, the identity */
    .omitChad = 1,
};

/* GenericRGB is the last of the fourteen profiles on this template and the
   only one at v2.2 -- a version that differs from v2.1 in nothing this file
   writes, since both carry the legacy string types, but that the header
   records separately.

   Three things about it are unlike any of the others.  It is the only profile
   whose description is a translation list rather than a single en-US string:
   'dscm' is an 'mluc' of thirty-one languages, and every other 'mluc' here is
   one record long.  Its constant tag comes second, straight after the
   description, where CoreMedia709's two come after the first tone curve.  And
   it is the only one to spell its manufacturer lower case, which is one header
   field and one row flag between them.

   Its white point is a D65 quantised to 0xf352 and 0x116cf.  The rest of the
   template splits between 0xf6d5 and 0xd32c, which is the same illuminant
   quantised down, and the header's own 0xf6d6 and 0xd32d; CoreMedia709 has the
   same pair as this one one unit off in each direction, at 0xf351 and 0x116cc,
   which is why the two look like the same measurement taken twice.  Its
   Bradford inverse is the shared matrix with three of its nine words off by
   one: 0xfd90 and 0xc06e becoming 0xfd91 and 0xc06c, and 0x793 becoming 0x792.
   Close enough to look like a transcription error, and not one -- which is why
   the words are copied rather than computed from the colorants, as everything
   else here is.

   What it shares with GenericRGBLinear is the colorants to the first and
   second words of red and green, 0x744d and 0x3dee, 0x5a75 and 0xac73; the two
   spaces differ in the rest, and the gamma here is 461/256 rather than the
   identity the linear one carries. */
static const struct cgs_mluc_record GenericRGBDscm[31] = {
    /* Slovak first and Arabic last, with nothing in between sorted: the order
       is Apple's own and is reproduced as found.  Two entries repeat an
       earlier string rather than carrying a second copy of it -- svSE after
       nbNO and esES after ptPO -- which put_mluc_records finds by comparing
       the text. */
    { "skSK", "Všeobecný RGB profil" },
    { "daDK", "Generel RGB-profil" },
    { "caES", "Perfil RGB genèric" },
    { "viVN", "Cấu hình RGB Chung" },
    { "ptBR", "Perfil RGB Genérico" },
    { "ukUA", "Загальний профайл RGB" },
    { "frFU", "Profil générique RVB" },
    { "huHU", "Általános RGB profil" },
    { "zhTW", "通用RGB色彩描述" },
    { "koKR", "일반 RGB 프로파일" },
    { "nbNO", "Generisk RGB-profil" },
    { "csCZ", "Obecný RGB profil" },
    { "heIL", "פרופיל RGB כללי" },
    { "roRO", "Profil RGB generic" },
    { "deDE", "Allgemeines RGB-Profil" },
    { "itIT", "Profilo RGB generico" },
    { "svSE", "Generisk RGB-profil" },
    { "zhCN", "普通RGB描述文件" },
    { "jaJP", "一般 RGB プロファイル" },
    { "elGR", "Γενικό προφίλ RGB" },
    { "ptPO", "Perfil RGB genérico" },
    { "nlNL", "Algemeen RGB-profiel" },
    { "esES", "Perfil RGB genérico" },
    { "thTH", "โปรไฟล์ RGB ทั่วไป" },
    { "trTR", "Genel RGB Profili" },
    { "fiFI", "Yleinen RGB-profiili" },
    { "hrHR", "Generički RGB profil" },
    { "plPL", "Uniwersalny profil RGB" },
    { "ruRU", "Общий профиль RGB" },
    { "enUS", "Generic RGB Profile" },
    { "arEG", "ملف تعريف RGB العام" }
};

static const struct cgs_rgb_v4 GenericRGB = {
    .desc = "Generic RGB Profile",
    .version = CGICCVersionV22,
    .legacyStrings = 1,
    .descScriptCode = 1,
    .lowercaseManufacturer = 1,
    .created = { 2009, 2, 25, 11, 26, 11 },
    .cprt = "Copyright 2007 Apple Inc., all rights reserved.",
    .wtpt = { 0xf352, 0x10000, 0x116cf },
    .colorants = { 0x744d, 0x3dee, 0x3d0, 0x5a75, 0xac73, 0x1734, 0x281a, 0x159f, 0xb836 },
    .trc = { 0, 0, 461, 0, { 0x0 } },  /* u8Fixed8 gamma, 461/256 */
    .chad = { 0x10c42, 0x5de, -0xcda, 0x792, 0xfd91, -0x45e, -0x25d, 0x3dc, 0xc06c },
    .extraName = { "dscm" },
    .extraRecords = { GenericRGBDscm },
    .extraRecordCount = { 31 },
    .extraCount = 1,
    .extrasWhere = CGICCRGBV4ExtrasAfterDesc,
};

/* The nineteen names those fourteen profiles answer to, and the space each one
   builds.  An extended-range name takes the same profile bytes as its base --
   the profile carries no range, and the two spaces are reported unequal
   because the flag below is what differs -- so it is the name and the flag that
   are per-space rather than the profile.

   Every one of the nineteen is an immortal singleton: two calls answer the same
   pointer at the immortal retain count, exactly as the device names do.  So
   each row is built on first use and then kept. */
enum { CGColorSpaceNamedRGBV4Count = 19 };

static const struct {
    const char *name;
    const struct cgs_rgb_v4 *profile;
    bool extended;
} CGColorSpaceNamedRGBV4[CGColorSpaceNamedRGBV4Count] = {
    { "kCGColorSpaceDisplayP3", &DisplayP3, false },
    { "kCGColorSpaceExtendedDisplayP3", &DisplayP3, true },
    { "kCGColorSpaceITUR_709", &ITUR_709, false },
    { "kCGColorSpaceITUR_2020", &ITUR_2020, false },
    { "kCGColorSpaceExtendedITUR_2020", &ITUR_2020, true },
    { "kCGColorSpaceITUR_2020_sRGBGamma", &ITUR_2020_sRGBGamma, false },
    { "kCGColorSpaceDisplayP3_709OETF", &DisplayP3_709OETF, false },
    { "kCGColorSpaceROMMRGB", &ROMMRGB, false },
    { "kCGColorSpaceDCIP3", &DCIP3, false },
    { "kCGColorSpaceACESCGLinear", &ACESCGLinear, false },
    { "kCGColorSpaceCoreMedia709", &CoreMedia709, false },
    { "kCGColorSpaceGenericRGB", &GenericRGB, false },
    { "kCGColorSpaceGenericRGBLinear", &GenericRGBLinear, false },
    { "kCGColorSpaceLinearSRGB", &LinearSRGB, false },
    { "kCGColorSpaceExtendedLinearSRGB", &LinearSRGB, true },
    { "kCGColorSpaceLinearDisplayP3", &LinearDisplayP3, false },
    { "kCGColorSpaceExtendedLinearDisplayP3", &LinearDisplayP3, true },
    { "kCGColorSpaceLinearITUR_2020", &LinearITUR_2020, false },
    { "kCGColorSpaceExtendedLinearITUR_2020", &LinearITUR_2020, true }
};

static struct CGColorSpace *CGColorSpaceNamedRGBV4State[CGColorSpaceNamedRGBV4Count];

/* Build, or find, the space one of the nineteen names on this template
   resolves to.  Answers NULL for every other name, so the caller can hand it
   the name it failed to recognise and get the same answer back. */
static CGColorSpaceRef CGColorSpaceCreateNamedRGBV4(CFStringRef name)
{
    struct CGColorSpace *s;
    unsigned char *profile;
    size_t len;
    int i;

    for (i = 0; i < CGColorSpaceNamedRGBV4Count; i++) {
        if (!CGColorSpaceNameEqualsASCII(name, CGColorSpaceNamedRGBV4[i].name))
            continue;
        if (CGColorSpaceNamedRGBV4State[i])
            return CGColorSpaceNamedRGBV4State[i];
        profile = put_rgb_v4(CGColorSpaceNamedRGBV4[i].profile, &len);
        if (!profile)
            return NULL;
        s = calloc(1, sizeof *s);
        if (!s) {
            free(profile);
            return NULL;
        }
        s->immortal = true;
        s->model = kCGColorSpaceModelRGB;
        s->type = CGColorSpaceTypeICC;
        s->ncomp = 3;
        /* A literal like the device names', and not owned. */
        s->name = CGColorSpaceNamedRGBV4[i].name;
        s->extended = CGColorSpaceNamedRGBV4[i].extended;
        s->profile = profile;
        s->profileLen = len;
        CGColorSpaceNamedRGBV4State[i] = s;
        return s;
    }
    return NULL;
}

/* The v2.1 monochrome/TRC template.

   Linear Gray is a gray profile rather than an RGB one and a v2.1 profile
   rather than a v4, so it shares almost nothing with the eleven above but the
   header: four tags rather than ten or eleven, the description and copyright as
   the legacy 'desc' and 'text' types rather than 'mluc' records, no colorants
   at all, and one tone curve rather than three.  What it shares is the white
   point and the header illuminant, which agree exactly here -- the quantisation
   step the RGB profiles mostly lose a unit to is not lost by this one.

   The tag order is description, copyright, white point, tone curve, and it is
   the same as the v4 template's for as far as the two run: 'desc', 'cprt',
   'wtpt'.  The tone curve is the one-word identity every other tone curve here
   is a special case of.

   The four tags are laid out the same way, each block aligned and the next
   beginning after it, so the data starts at 132 plus four entries and the two
   strings run into each other's padding: the description ends at 282, the
   copyright is written at 284 and ends at 319, and the white point follows at
   320.  That leaves the tone curve's fourteen bytes finishing at 354, and the
   profile is the 356 bytes that pads that to a four-byte boundary. */
enum { CGICCGrayV2TagCount = 4 };

/* One gray space's parameters.  As with the RGB rows above, these are
   constants recovered from the profile the name resolves to rather than
   anything worked out at run time. */
struct cgs_gray_v2 {
    const char *desc;
    const char *cprt;
    int created[6];
    int32_t wtpt[3];
    struct cgs_icc_curve trc;
};

/* Assemble the profile.  Four blocks go out -- the two strings, the white point
   and the single tone curve -- and the four tags are pointed at them. */
static unsigned char *put_gray_v2(const struct cgs_gray_v2 *v, size_t *outLen)
{
    enum { desc, cprt, wtpt, curve, blockCount };
    static const char *const fixed[blockCount] = {
        "desc", "cprt", "wtpt", "kTRC"
    };
    unsigned char *p;
    size_t dlen, tlen, len;
    int32_t tagOff[blockCount], tagLen[blockCount];
    int i;

    /* put_desc and put_text both size themselves from the string, so the two
       lengths are worked out here rather than written down. */
    dlen = 12 + (strlen(v->desc) + 1) + 4 + 4 + 2 + 67 + 1;
    tlen = v->trc.parametric ? 12 + 4 * (size_t)v->trc.words : 14;

    /* As in put_rgb_v4, the offsets are assigned in one pass and used again
       below, so the two cannot disagree about where a block went. */
    len = CGICCTagTableOffset + (size_t)CGICCGrayV2TagCount * CGICCTagEntrySize;
    tagLen[desc] = (int32_t)dlen;
    tagOff[desc] = (int32_t)icc_pad(len);
    len = icc_pad(len) + dlen;
    tagLen[cprt] = (int32_t)(8 + strlen(v->cprt) + 1);
    tagOff[cprt] = (int32_t)icc_pad(len);
    len = icc_pad(len) + tagLen[cprt];
    tagLen[wtpt] = CGICCXYZLength;
    tagOff[wtpt] = (int32_t)icc_pad(len);
    len = icc_pad(len) + CGICCXYZLength;
    tagLen[curve] = (int32_t)tlen;
    tagOff[curve] = (int32_t)icc_pad(len);
    len = icc_pad(len) + tlen;
    /* The tone curve is fourteen bytes and leaves the profile two short of a
       boundary, so the size counts the padding rather than the last tag. */
    len = icc_pad(len);

    p = calloc(1, len);
    if (!p)
        return NULL;

    put_icc_header(p, len, v->created, CGICCVersionV21, "GRAY");
    put_be32(p + CGICCTagCountOffset, (uint32_t)CGICCGrayV2TagCount);
    for (i = 0; i < CGICCGrayV2TagCount; i++) {
        unsigned char *e = p + CGICCTagTableOffset + i * CGICCTagEntrySize;

        memcpy(e, fixed[i], 4);
        put_be32(e + 4, tagOff[i]);
        put_be32(e + 8, tagLen[i]);
    }

    /* The gray template has one description and it takes the empty ScriptCode
       form, which put_desc writes when told not to fill it. */
    put_desc(p + tagOff[desc], v->desc, 0);
    put_text(p + tagOff[cprt], v->cprt);
    put_xyz_i32(p + tagOff[wtpt], v->wtpt);
    put_icc_curve(p + tagOff[curve], &v->trc);

    *outLen = len;
    return p;
}

/* The one profile, which the two names below share.  Recovered from the space
   kCGColorSpaceLinearGray resolves to; put_gray_v2 was checked against it by
   rebuilding the profile and comparing it byte for byte. */
static const struct cgs_gray_v2 LinearGray = {
    .desc = "Linear Gray",
    .created = { 2016, 1, 1, 0, 0, 0 },
    .cprt = "Copyright Apple Inc., 2016",
    /* The D50 illuminant, quantised exactly -- unlike seven of the eight
       non-linear RGB spaces, whose white point lands a unit below it. */
    .wtpt = { 0xf6d6, 0x10000, 0xd32d },
    .trc = { 0, 0, 256, 0, { 0x0 } },  /* u8Fixed8 gamma of 1.0 */
};

/* The two names, and the extended flag each carries.

   kCGColorSpaceExtendedLinearGray takes the very same 356 bytes as the space
   it is named after: the profile records no range, so the profile cannot be
   what tells the two apart.  They are reported unequal and are the same space
   once the range is ignored, which is the same arrangement the extended RGB
   names use. */
enum { CGColorSpaceNamedGrayV2Count = 2 };

static const struct {
    const char *name;
    bool extended;
} CGColorSpaceNamedGrayV2[CGColorSpaceNamedGrayV2Count] = {
    { "kCGColorSpaceLinearGray", false },
    { "kCGColorSpaceExtendedLinearGray", true }
};

static struct CGColorSpace *CGColorSpaceNamedGrayV2State[CGColorSpaceNamedGrayV2Count];

/* Build, or find, the space one of the two gray names resolves to.  Answers
   NULL for every other name, so the caller can fall through to the RGB
   template the same way it falls through from there. */
static CGColorSpaceRef CGColorSpaceCreateNamedGrayV2(CFStringRef name)
{
    struct CGColorSpace *s;
    unsigned char *profile;
    size_t len;
    int i;

    for (i = 0; i < CGColorSpaceNamedGrayV2Count; i++) {
        if (!CGColorSpaceNameEqualsASCII(name, CGColorSpaceNamedGrayV2[i].name))
            continue;
        if (CGColorSpaceNamedGrayV2State[i])
            return CGColorSpaceNamedGrayV2State[i];
        profile = put_gray_v2(&LinearGray, &len);
        if (!profile)
            return NULL;
        s = calloc(1, sizeof *s);
        if (!s) {
            free(profile);
            return NULL;
        }
        s->immortal = true;
        s->model = kCGColorSpaceModelMonochrome;
        s->type = CGColorSpaceTypeICC;
        s->ncomp = 1;
        /* A literal like the device names', and not owned. */
        s->name = CGColorSpaceNamedGrayV2[i].name;
        s->extended = CGColorSpaceNamedGrayV2[i].extended;
        s->profile = profile;
        s->profileLen = len;
        CGColorSpaceNamedGrayV2State[i] = s;
        return s;
    }
    return NULL;
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

/* The six HDR spaces: three primaries, each in a perceptual quantizer and a
   hybrid log-gamma one.

   These are a third template rather than a variant of either of the two above.
   Where an 'mft2' profile carries one matrix and one curve per component, these
   carry an 'mAB ' tag and its mirror 'mBA ': a lut16Type whose sections are laid
   out by the five offsets in its own header rather than by the fixed order the
   older type uses, whose matrix is a block of plain s15Fixed16 rather than
   colourant-tagged 'XYZ ' triples, and whose curves are sampled tables rather
   than parameters.  'mBA ' is 'mAB ' with the two ends swapped, so one emitter
   serves both and the tag signature is their only difference.

   Everything that varies across the six was recovered by comparing the six
   profiles against each other, and the sharing between them is what keeps these
   tables as small as they are.  The sixteen lut tags hold four distinct sampled
   curves -- one per family and direction -- because the curve follows the
   transfer function alone and not the primaries; each tag repeats its table
   three times, once per component.  The matrix is the only per-primitive data,
   six pairs, and a PQ profile and its HLG counterpart share theirs exactly.
   The M curves and the CLUT vary by family and direction but not by primaries
   either, so four of each cover all sixteen tags.  'wtpt' and 'chad' are the
   same in all six and are written from the values the v4 rows above already
   carry.  Only 'desc', 'cicp' and 'lumi' are per-space.

   The sampled curves are the one part of this template that is stored data
   rather than parameters, and they are stored verbatim for that reason.  A PQ
   or HLG quantizer is normally written as a closed form, but the nearest such
   formula misses these values by far more than the step they are quantised at,
   so a table is the only way to reproduce the profiles byte for byte.  Four
   tables of 1024 and 512 entries is most of the data in this section, and it is
   sampled data rather than a second implementation of a colour space. */
enum {
    /* Eight tags, against ten in the v4 template: no per-component matrix or
       curve tags, since the two lut tags carry three components' worth between
       them, and a 'lumi' in place of nothing. */
    CGICCHDRTagCount = 8,
    /* 'sf32', four reserved bytes, then nine s15Fixed16.  The lut tag does not
       use it -- it has no Bradford block -- but the profile does, and the two
       are byte for byte the same. */
    CGICCHDRChadLength = CGICCRGBV4ChadLength,
    /* 'cicp', four reserved bytes, then the four coding bytes. */
    CGICCHDRCicpLength = CGICCRGBV4CicpLength,
    /* An 'XYZ ' tag, as 'wtpt' is. */
    CGICCHDRLumiLength = CGICCXYZLength,
    /* The five section offsets every lut tag here declares.  The B curves start
       at 32 because the header is thirty-two bytes; the sampled curves start at
       232 because the four sections before them are fixed width -- three
       twelve-byte identity curves, a thirty-six byte matrix, twelve bytes of
       pad, three twenty-four byte M curves and a forty-four byte CLUT. */
    CGICCHDRBOffset = 32,
    CGICCHDRMatrixOffset = 68,
    CGICCHDRMOffset = 116,
    CGICCHDRCLUTOffset = 188,
    CGICCHDRCurveOffset = 232,
    /* One identity 'curv': a signature, four reserved bytes, and a count of
       zero, which is what makes it the identity rather than a gamma. */
    CGICCHDRIdentityCurveLength = 12,
    /* One 'para', as put_icc_curve writes it, with three parameters. */
    CGICCHDRMCurveLength = 24,
    /* The matrix, and the pad that separates it from the M curves. */
    CGICCHDRMatrixLength = 36,
    CGICCHDRMatrixPad = 12
};

/* Which of the two sampled curves a lut tag carries. */
enum {
    CGICCHDRForward = 0,
    CGICCHDRReverse = 1
};

/* The sampled tone curves.  Each of the sixteen lut tags holds the same table
   three times over, once per component, so a single copy is stored and written
   out three times; the count is in the table's name. */
static const uint16_t cgsHdrTone_pq_ab[1024] = {
    0x0000, 0x055d, 0x06c9, 0x07cc, 0x089d, 0x0950, 0x09ef, 0x0a7e, 0x0b03, 0x0b7e, 0x0bf2, 0x0c60,
    0x0cc9, 0x0d2d, 0x0d8d, 0x0de9, 0x0e42, 0x0e99, 0x0eed, 0x0f3f, 0x0f8e, 0x0fdc, 0x1028, 0x1072,
    0x10bb, 0x1102, 0x1149, 0x118d, 0x11d1, 0x1214, 0x1255, 0x1296, 0x12d6, 0x1315, 0x1353, 0x1390,
    0x13cd, 0x1409, 0x1444, 0x147e, 0x14b8, 0x14f2, 0x152b, 0x1563, 0x159b, 0x15d2, 0x1609, 0x163f,
    0x1675, 0x16ab, 0x16e0, 0x1715, 0x1749, 0x177d, 0x17b1, 0x17e4, 0x1817, 0x184a, 0x187c, 0x18ae,
    0x18e0, 0x1911, 0x1942, 0x1973, 0x19a4, 0x19d5, 0x1a05, 0x1a35, 0x1a65, 0x1a94, 0x1ac3, 0x1af3,
    0x1b21, 0x1b50, 0x1b7f, 0x1bad, 0x1bdb, 0x1c09, 0x1c37, 0x1c65, 0x1c92, 0x1cbf, 0x1cec, 0x1d19,
    0x1d46, 0x1d73, 0x1da0, 0x1dcc, 0x1df8, 0x1e25, 0x1e51, 0x1e7c, 0x1ea8, 0x1ed4, 0x1f00, 0x1f2b,
    0x1f56, 0x1f82, 0x1fad, 0x1fd8, 0x2003, 0x202d, 0x2058, 0x2083, 0x20ad, 0x20d8, 0x2102, 0x212c,
    0x2157, 0x2181, 0x21ab, 0x21d5, 0x21ff, 0x2228, 0x2252, 0x227c, 0x22a5, 0x22cf, 0x22f8, 0x2322,
    0x234b, 0x2374, 0x239d, 0x23c6, 0x23f0, 0x2419, 0x2442, 0x246a, 0x2493, 0x24bc, 0x24e5, 0x250e,
    0x2536, 0x255f, 0x2587, 0x25b0, 0x25d8, 0x2601, 0x2629, 0x2651, 0x267a, 0x26a2, 0x26ca, 0x26f2,
    0x271b, 0x2743, 0x276b, 0x2793, 0x27bb, 0x27e3, 0x280b, 0x2833, 0x285b, 0x2882, 0x28aa, 0x28d2,
    0x28fa, 0x2922, 0x2949, 0x2971, 0x2999, 0x29c0, 0x29e8, 0x2a10, 0x2a37, 0x2a5f, 0x2a86, 0x2aae,
    0x2ad5, 0x2afd, 0x2b24, 0x2b4c, 0x2b73, 0x2b9b, 0x2bc2, 0x2be9, 0x2c11, 0x2c38, 0x2c60, 0x2c87,
    0x2cae, 0x2cd6, 0x2cfd, 0x2d24, 0x2d4b, 0x2d73, 0x2d9a, 0x2dc1, 0x2de9, 0x2e10, 0x2e37, 0x2e5e,
    0x2e86, 0x2ead, 0x2ed4, 0x2efb, 0x2f22, 0x2f4a, 0x2f71, 0x2f98, 0x2fbf, 0x2fe7, 0x300e, 0x3035,
    0x305c, 0x3083, 0x30ab, 0x30d2, 0x30f9, 0x3120, 0x3147, 0x316f, 0x3196, 0x31bd, 0x31e4, 0x320c,
    0x3233, 0x325a, 0x3281, 0x32a9, 0x32d0, 0x32f7, 0x331e, 0x3346, 0x336d, 0x3394, 0x33bb, 0x33e3,
    0x340a, 0x3431, 0x3459, 0x3480, 0x34a7, 0x34cf, 0x34f6, 0x351e, 0x3545, 0x356c, 0x3594, 0x35bb,
    0x35e3, 0x360a, 0x3631, 0x3659, 0x3680, 0x36a8, 0x36cf, 0x36f7, 0x371f, 0x3746, 0x376e, 0x3795,
    0x37bd, 0x37e4, 0x380c, 0x3834, 0x385b, 0x3883, 0x38ab, 0x38d3, 0x38fa, 0x3922, 0x394a, 0x3972,
    0x3999, 0x39c1, 0x39e9, 0x3a11, 0x3a39, 0x3a61, 0x3a89, 0x3ab1, 0x3ad9, 0x3b01, 0x3b29, 0x3b51,
    0x3b79, 0x3ba1, 0x3bc9, 0x3bf1, 0x3c19, 0x3c41, 0x3c69, 0x3c92, 0x3cba, 0x3ce2, 0x3d0a, 0x3d33,
    0x3d5b, 0x3d83, 0x3dac, 0x3dd4, 0x3dfc, 0x3e25, 0x3e4d, 0x3e76, 0x3e9e, 0x3ec7, 0x3eef, 0x3f18,
    0x3f41, 0x3f69, 0x3f92, 0x3fbb, 0x3fe3, 0x400c, 0x4035, 0x405e, 0x4087, 0x40b0, 0x40d8, 0x4101,
    0x412a, 0x4153, 0x417c, 0x41a5, 0x41ce, 0x41f8, 0x4221, 0x424a, 0x4273, 0x429c, 0x42c6, 0x42ef,
    0x4318, 0x4341, 0x436b, 0x4394, 0x43be, 0x43e7, 0x4411, 0x443a, 0x4464, 0x448d, 0x44b7, 0x44e1,
    0x450a, 0x4534, 0x455e, 0x4588, 0x45b2, 0x45dc, 0x4605, 0x462f, 0x4659, 0x4683, 0x46ad, 0x46d8,
    0x4702, 0x472c, 0x4756, 0x4780, 0x47ab, 0x47d5, 0x47ff, 0x482a, 0x4854, 0x487e, 0x48a9, 0x48d3,
    0x48fe, 0x4929, 0x4953, 0x497e, 0x49a9, 0x49d3, 0x49fe, 0x4a29, 0x4a54, 0x4a7f, 0x4aaa, 0x4ad5,
    0x4b00, 0x4b2b, 0x4b56, 0x4b81, 0x4bac, 0x4bd8, 0x4c03, 0x4c2e, 0x4c5a, 0x4c85, 0x4cb0, 0x4cdc,
    0x4d07, 0x4d33, 0x4d5f, 0x4d8a, 0x4db6, 0x4de2, 0x4e0d, 0x4e39, 0x4e65, 0x4e91, 0x4ebd, 0x4ee9,
    0x4f15, 0x4f41, 0x4f6d, 0x4f99, 0x4fc6, 0x4ff2, 0x501e, 0x504b, 0x5077, 0x50a3, 0x50d0, 0x50fc,
    0x5129, 0x5156, 0x5182, 0x51af, 0x51dc, 0x5209, 0x5235, 0x5262, 0x528f, 0x52bc, 0x52e9, 0x5316,
    0x5344, 0x5371, 0x539e, 0x53cb, 0x53f9, 0x5426, 0x5453, 0x5481, 0x54ae, 0x54dc, 0x550a, 0x5537,
    0x5565, 0x5593, 0x55c1, 0x55ef, 0x561d, 0x564b, 0x5679, 0x56a7, 0x56d5, 0x5703, 0x5731, 0x5760,
    0x578e, 0x57bc, 0x57eb, 0x5819, 0x5848, 0x5876, 0x58a5, 0x58d4, 0x5903, 0x5931, 0x5960, 0x598f,
    0x59be, 0x59ed, 0x5a1c, 0x5a4b, 0x5a7b, 0x5aaa, 0x5ad9, 0x5b09, 0x5b38, 0x5b67, 0x5b97, 0x5bc7,
    0x5bf6, 0x5c26, 0x5c56, 0x5c86, 0x5cb5, 0x5ce5, 0x5d15, 0x5d45, 0x5d75, 0x5da6, 0x5dd6, 0x5e06,
    0x5e36, 0x5e67, 0x5e97, 0x5ec8, 0x5ef8, 0x5f29, 0x5f5a, 0x5f8a, 0x5fbb, 0x5fec, 0x601d, 0x604e,
    0x607f, 0x60b0, 0x60e1, 0x6113, 0x6144, 0x6175, 0x61a7, 0x61d8, 0x620a, 0x623b, 0x626d, 0x629f,
    0x62d0, 0x6302, 0x6334, 0x6366, 0x6398, 0x63ca, 0x63fc, 0x642f, 0x6461, 0x6493, 0x64c6, 0x64f8,
    0x652b, 0x655d, 0x6590, 0x65c3, 0x65f5, 0x6628, 0x665b, 0x668e, 0x66c1, 0x66f4, 0x6728, 0x675b,
    0x678e, 0x67c2, 0x67f5, 0x6829, 0x685c, 0x6890, 0x68c4, 0x68f7, 0x692b, 0x695f, 0x6993, 0x69c7,
    0x69fc, 0x6a30, 0x6a64, 0x6a98, 0x6acd, 0x6b01, 0x6b36, 0x6b6b, 0x6b9f, 0x6bd4, 0x6c09, 0x6c3e,
    0x6c73, 0x6ca8, 0x6cdd, 0x6d12, 0x6d48, 0x6d7d, 0x6db2, 0x6de8, 0x6e1d, 0x6e53, 0x6e89, 0x6ebf,
    0x6ef4, 0x6f2a, 0x6f60, 0x6f96, 0x6fcd, 0x7003, 0x7039, 0x7070, 0x70a6, 0x70dd, 0x7113, 0x714a,
    0x7181, 0x71b8, 0x71ee, 0x7225, 0x725d, 0x7294, 0x72cb, 0x7302, 0x733a, 0x7371, 0x73a9, 0x73e0,
    0x7418, 0x7450, 0x7488, 0x74c0, 0x74f8, 0x7530, 0x7568, 0x75a0, 0x75d8, 0x7611, 0x7649, 0x7682,
    0x76bb, 0x76f3, 0x772c, 0x7765, 0x779e, 0x77d7, 0x7810, 0x784a, 0x7883, 0x78bc, 0x78f6, 0x792f,
    0x7969, 0x79a3, 0x79dd, 0x7a17, 0x7a51, 0x7a8b, 0x7ac5, 0x7aff, 0x7b39, 0x7b74, 0x7bae, 0x7be9,
    0x7c24, 0x7c5e, 0x7c99, 0x7cd4, 0x7d0f, 0x7d4a, 0x7d86, 0x7dc1, 0x7dfc, 0x7e38, 0x7e73, 0x7eaf,
    0x7eeb, 0x7f26, 0x7f62, 0x7f9e, 0x7fdb, 0x8017, 0x8053, 0x808f, 0x80cc, 0x8108, 0x8145, 0x8182,
    0x81bf, 0x81fc, 0x8239, 0x8276, 0x82b3, 0x82f0, 0x832e, 0x836b, 0x83a9, 0x83e6, 0x8424, 0x8462,
    0x84a0, 0x84de, 0x851c, 0x855b, 0x8599, 0x85d7, 0x8616, 0x8655, 0x8693, 0x86d2, 0x8711, 0x8750,
    0x878f, 0x87ce, 0x880e, 0x884d, 0x888d, 0x88cc, 0x890c, 0x894c, 0x898c, 0x89cc, 0x8a0c, 0x8a4c,
    0x8a8d, 0x8acd, 0x8b0e, 0x8b4e, 0x8b8f, 0x8bd0, 0x8c11, 0x8c52, 0x8c93, 0x8cd4, 0x8d16, 0x8d57,
    0x8d99, 0x8dda, 0x8e1c, 0x8e5e, 0x8ea0, 0x8ee2, 0x8f24, 0x8f67, 0x8fa9, 0x8fec, 0x902e, 0x9071,
    0x90b4, 0x90f7, 0x913a, 0x917d, 0x91c1, 0x9204, 0x9248, 0x928b, 0x92cf, 0x9313, 0x9357, 0x939b,
    0x93df, 0x9423, 0x9468, 0x94ac, 0x94f1, 0x9536, 0x957b, 0x95c0, 0x9605, 0x964a, 0x968f, 0x96d5,
    0x971a, 0x9760, 0x97a6, 0x97ec, 0x9832, 0x9878, 0x98be, 0x9904, 0x994b, 0x9992, 0x99d8, 0x9a1f,
    0x9a66, 0x9aad, 0x9af4, 0x9b3c, 0x9b83, 0x9bcb, 0x9c13, 0x9c5a, 0x9ca2, 0x9cea, 0x9d33, 0x9d7b,
    0x9dc3, 0x9e0c, 0x9e55, 0x9e9d, 0x9ee6, 0x9f2f, 0x9f79, 0x9fc2, 0xa00b, 0xa055, 0xa09f, 0xa0e8,
    0xa132, 0xa17c, 0xa1c7, 0xa211, 0xa25b, 0xa2a6, 0xa2f1, 0xa33c, 0xa387, 0xa3d2, 0xa41d, 0xa468,
    0xa4b4, 0xa500, 0xa54b, 0xa597, 0xa5e3, 0xa62f, 0xa67c, 0xa6c8, 0xa715, 0xa762, 0xa7ae, 0xa7fb,
    0xa848, 0xa896, 0xa8e3, 0xa931, 0xa97e, 0xa9cc, 0xaa1a, 0xaa68, 0xaab6, 0xab05, 0xab53, 0xaba2,
    0xabf1, 0xac40, 0xac8f, 0xacde, 0xad2d, 0xad7d, 0xadcd, 0xae1c, 0xae6c, 0xaebc, 0xaf0d, 0xaf5d,
    0xafad, 0xaffe, 0xb04f, 0xb0a0, 0xb0f1, 0xb142, 0xb194, 0xb1e5, 0xb237, 0xb289, 0xb2db, 0xb32d,
    0xb37f, 0xb3d2, 0xb424, 0xb477, 0xb4ca, 0xb51d, 0xb570, 0xb5c4, 0xb617, 0xb66b, 0xb6bf, 0xb713,
    0xb767, 0xb7bb, 0xb810, 0xb864, 0xb8b9, 0xb90e, 0xb963, 0xb9b8, 0xba0e, 0xba63, 0xbab9, 0xbb0f,
    0xbb65, 0xbbbb, 0xbc12, 0xbc68, 0xbcbf, 0xbd16, 0xbd6d, 0xbdc4, 0xbe1b, 0xbe73, 0xbecb, 0xbf23,
    0xbf7b, 0xbfd3, 0xc02b, 0xc084, 0xc0dd, 0xc135, 0xc18e, 0xc1e8, 0xc241, 0xc29b, 0xc2f4, 0xc34e,
    0xc3a8, 0xc403, 0xc45d, 0xc4b8, 0xc513, 0xc56d, 0xc5c9, 0xc624, 0xc67f, 0xc6db, 0xc737, 0xc793,
    0xc7ef, 0xc84c, 0xc8a8, 0xc905, 0xc962, 0xc9bf, 0xca1c, 0xca7a, 0xcad7, 0xcb35, 0xcb93, 0xcbf2,
    0xcc50, 0xccaf, 0xcd0d, 0xcd6c, 0xcdcb, 0xce2b, 0xce8a, 0xceea, 0xcf4a, 0xcfaa, 0xd00a, 0xd06b,
    0xd0cc, 0xd12c, 0xd18e, 0xd1ef, 0xd250, 0xd2b2, 0xd314, 0xd376, 0xd3d8, 0xd43b, 0xd49d, 0xd500,
    0xd563, 0xd5c6, 0xd62a, 0xd68d, 0xd6f1, 0xd755, 0xd7ba, 0xd81e, 0xd883, 0xd8e8, 0xd94d, 0xd9b2,
    0xda18, 0xda7d, 0xdae3, 0xdb49, 0xdbb0, 0xdc16, 0xdc7d, 0xdce4, 0xdd4b, 0xddb2, 0xde1a, 0xde82,
    0xdeea, 0xdf52, 0xdfbb, 0xe023, 0xe08c, 0xe0f5, 0xe15f, 0xe1c8, 0xe232, 0xe29c, 0xe306, 0xe371,
    0xe3db, 0xe446, 0xe4b1, 0xe51d, 0xe588, 0xe5f4, 0xe660, 0xe6cc, 0xe739, 0xe7a6, 0xe813, 0xe880,
    0xe8ed, 0xe95b, 0xe9c9, 0xea37, 0xeaa5, 0xeb14, 0xeb82, 0xebf2, 0xec61, 0xecd0, 0xed40, 0xedb0,
    0xee20, 0xee91, 0xef02, 0xef73, 0xefe4, 0xf055, 0xf0c7, 0xf139, 0xf1ab, 0xf21e, 0xf290, 0xf303,
    0xf376, 0xf3ea, 0xf45e, 0xf4d2, 0xf546, 0xf5ba, 0xf62f, 0xf6a4, 0xf719, 0xf78f, 0xf804, 0xf87a,
    0xf8f1, 0xf967, 0xf9de, 0xfa55, 0xfacc, 0xfb44, 0xfbbc, 0xfc34, 0xfcac, 0xfd25, 0xfd9e, 0xfe17,
    0xfe90, 0xff0a, 0xff84, 0xffff,
};
static const uint16_t cgsHdrTone_pq_ba[1024] = {
    0x0000, 0x0000, 0x0000, 0x0002, 0x0005, 0x0009, 0x0010, 0x0019, 0x0025, 0x0034, 0x0047, 0x005e,
    0x007a, 0x009a, 0x00bf, 0x00e8, 0x0117, 0x014a, 0x0183, 0x01c1, 0x0203, 0x024b, 0x0298, 0x02e9,
    0x033f, 0x039a, 0x03fa, 0x045e, 0x04c6, 0x0532, 0x05a3, 0x0617, 0x0690, 0x070c, 0x078b, 0x080e,
    0x0894, 0x091d, 0x09aa, 0x0a39, 0x0acb, 0x0b5f, 0x0bf6, 0x0c8f, 0x0d2b, 0x0dc9, 0x0e68, 0x0f0a,
    0x0fae, 0x1053, 0x10fa, 0x11a3, 0x124d, 0x12f8, 0x13a5, 0x1452, 0x1501, 0x15b1, 0x1662, 0x1714,
    0x17c7, 0x187b, 0x192f, 0x19e4, 0x1a99, 0x1b50, 0x1c06, 0x1cbd, 0x1d75, 0x1e2c, 0x1ee4, 0x1f9d,
    0x2055, 0x210e, 0x21c7, 0x2280, 0x2339, 0x23f2, 0x24ab, 0x2564, 0x261d, 0x26d6, 0x278f, 0x2847,
    0x2900, 0x29b8, 0x2a70, 0x2b28, 0x2bdf, 0x2c97, 0x2d4d, 0x2e04, 0x2eba, 0x2f70, 0x3026, 0x30db,
    0x3190, 0x3245, 0x32f9, 0x33ac, 0x345f, 0x3512, 0x35c4, 0x3676, 0x3727, 0x37d8, 0x3888, 0x3938,
    0x39e7, 0x3a96, 0x3b44, 0x3bf2, 0x3c9f, 0x3d4b, 0x3df7, 0x3ea3, 0x3f4e, 0x3ff8, 0x40a2, 0x414b,
    0x41f3, 0x429b, 0x4343, 0x43e9, 0x448f, 0x4535, 0x45da, 0x467e, 0x4722, 0x47c5, 0x4868, 0x490a,
    0x49ab, 0x4a4c, 0x4aec, 0x4b8c, 0x4c2b, 0x4cc9, 0x4d67, 0x4e04, 0x4ea0, 0x4f3c, 0x4fd7, 0x5072,
    0x510c, 0x51a5, 0x523e, 0x52d6, 0x536e, 0x5405, 0x549b, 0x5531, 0x55c6, 0x565b, 0x56ef, 0x5782,
    0x5815, 0x58a7, 0x5939, 0x59ca, 0x5a5b, 0x5aea, 0x5b7a, 0x5c08, 0x5c97, 0x5d24, 0x5db1, 0x5e3d,
    0x5ec9, 0x5f55, 0x5fdf, 0x6069, 0x60f3, 0x617c, 0x6204, 0x628c, 0x6313, 0x639a, 0x6420, 0x64a6,
    0x652b, 0x65b0, 0x6634, 0x66b7, 0x673a, 0x67bd, 0x683f, 0x68c0, 0x6941, 0x69c1, 0x6a41, 0x6ac0,
    0x6b3f, 0x6bbd, 0x6c3b, 0x6cb8, 0x6d35, 0x6db1, 0x6e2d, 0x6ea8, 0x6f23, 0x6f9d, 0x7017, 0x7090,
    0x7109, 0x7181, 0x71f9, 0x7270, 0x72e7, 0x735d, 0x73d3, 0x7448, 0x74bd, 0x7532, 0x75a6, 0x7619,
    0x768c, 0x76ff, 0x7771, 0x77e3, 0x7854, 0x78c5, 0x7935, 0x79a5, 0x7a14, 0x7a83, 0x7af2, 0x7b60,
    0x7bce, 0x7c3b, 0x7ca8, 0x7d15, 0x7d81, 0x7dec, 0x7e57, 0x7ec2, 0x7f2c, 0x7f96, 0x8000, 0x8069,
    0x80d2, 0x813a, 0x81a2, 0x820a, 0x8271, 0x82d7, 0x833e, 0x83a4, 0x8409, 0x846e, 0x84d3, 0x8538,
    0x859c, 0x85ff, 0x8663, 0x86c6, 0x8728, 0x878a, 0x87ec, 0x884d, 0x88ae, 0x890f, 0x8970, 0x89d0,
    0x8a2f, 0x8a8e, 0x8aed, 0x8b4c, 0x8baa, 0x8c08, 0x8c65, 0x8cc3, 0x8d1f, 0x8d7c, 0x8dd8, 0x8e34,
    0x8e8f, 0x8eeb, 0x8f45, 0x8fa0, 0x8ffa, 0x9054, 0x90ad, 0x9107, 0x9160, 0x91b8, 0x9210, 0x9268,
    0x92c0, 0x9317, 0x936e, 0x93c5, 0x941b, 0x9471, 0x94c7, 0x951c, 0x9572, 0x95c6, 0x961b, 0x966f,
    0x96c3, 0x9717, 0x976a, 0x97bd, 0x9810, 0x9863, 0x98b5, 0x9907, 0x9958, 0x99aa, 0x99fb, 0x9a4c,
    0x9a9c, 0x9aec, 0x9b3c, 0x9b8c, 0x9bdc, 0x9c2b, 0x9c7a, 0x9cc8, 0x9d17, 0x9d65, 0x9db3, 0x9e00,
    0x9e4d, 0x9e9a, 0x9ee7, 0x9f34, 0x9f80, 0x9fcc, 0xa018, 0xa063, 0xa0af, 0xa0fa, 0xa144, 0xa18f,
    0xa1d9, 0xa223, 0xa26d, 0xa2b7, 0xa300, 0xa349, 0xa392, 0xa3da, 0xa423, 0xa46b, 0xa4b3, 0xa4fa,
    0xa542, 0xa589, 0xa5d0, 0xa617, 0xa65d, 0xa6a3, 0xa6e9, 0xa72f, 0xa775, 0xa7ba, 0xa7ff, 0xa844,
    0xa889, 0xa8ce, 0xa912, 0xa956, 0xa99a, 0xa9dd, 0xaa21, 0xaa64, 0xaaa7, 0xaaea, 0xab2c, 0xab6f,
    0xabb1, 0xabf3, 0xac35, 0xac76, 0xacb8, 0xacf9, 0xad3a, 0xad7a, 0xadbb, 0xadfb, 0xae3c, 0xae7c,
    0xaebb, 0xaefb, 0xaf3a, 0xaf79, 0xafb8, 0xaff7, 0xb036, 0xb074, 0xb0b2, 0xb0f1, 0xb12e, 0xb16c,
    0xb1aa, 0xb1e7, 0xb224, 0xb261, 0xb29e, 0xb2da, 0xb317, 0xb353, 0xb38f, 0xb3cb, 0xb407, 0xb442,
    0xb47d, 0xb4b9, 0xb4f4, 0xb52e, 0xb569, 0xb5a4, 0xb5de, 0xb618, 0xb652, 0xb68c, 0xb6c5, 0xb6ff,
    0xb738, 0xb771, 0xb7aa, 0xb7e3, 0xb81c, 0xb854, 0xb88d, 0xb8c5, 0xb8fd, 0xb935, 0xb96c, 0xb9a4,
    0xb9db, 0xba12, 0xba49, 0xba80, 0xbab7, 0xbaee, 0xbb24, 0xbb5a, 0xbb90, 0xbbc6, 0xbbfc, 0xbc32,
    0xbc68, 0xbc9d, 0xbcd2, 0xbd07, 0xbd3c, 0xbd71, 0xbda6, 0xbdda, 0xbe0f, 0xbe43, 0xbe77, 0xbeab,
    0xbedf, 0xbf12, 0xbf46, 0xbf79, 0xbfac, 0xbfdf, 0xc012, 0xc045, 0xc078, 0xc0aa, 0xc0dd, 0xc10f,
    0xc141, 0xc173, 0xc1a5, 0xc1d7, 0xc208, 0xc23a, 0xc26b, 0xc29c, 0xc2cd, 0xc2fe, 0xc32f, 0xc360,
    0xc390, 0xc3c1, 0xc3f1, 0xc421, 0xc451, 0xc481, 0xc4b1, 0xc4e1, 0xc510, 0xc540, 0xc56f, 0xc59e,
    0xc5cd, 0xc5fc, 0xc62b, 0xc65a, 0xc688, 0xc6b7, 0xc6e5, 0xc713, 0xc741, 0xc76f, 0xc79d, 0xc7cb,
    0xc7f9, 0xc826, 0xc854, 0xc881, 0xc8ae, 0xc8db, 0xc908, 0xc935, 0xc962, 0xc98e, 0xc9bb, 0xc9e7,
    0xca13, 0xca3f, 0xca6b, 0xca97, 0xcac3, 0xcaef, 0xcb1b, 0xcb46, 0xcb71, 0xcb9d, 0xcbc8, 0xcbf3,
    0xcc1e, 0xcc49, 0xcc74, 0xcc9e, 0xccc9, 0xccf3, 0xcd1d, 0xcd48, 0xcd72, 0xcd9c, 0xcdc6, 0xcdf0,
    0xce19, 0xce43, 0xce6c, 0xce96, 0xcebf, 0xcee8, 0xcf11, 0xcf3b, 0xcf63, 0xcf8c, 0xcfb5, 0xcfde,
    0xd006, 0xd02f, 0xd057, 0xd07f, 0xd0a7, 0xd0cf, 0xd0f7, 0xd11f, 0xd147, 0xd16f, 0xd196, 0xd1be,
    0xd1e5, 0xd20c, 0xd234, 0xd25b, 0xd282, 0xd2a9, 0xd2cf, 0xd2f6, 0xd31d, 0xd343, 0xd36a, 0xd390,
    0xd3b7, 0xd3dd, 0xd403, 0xd429, 0xd44f, 0xd475, 0xd49b, 0xd4c0, 0xd4e6, 0xd50b, 0xd531, 0xd556,
    0xd57b, 0xd5a1, 0xd5c6, 0xd5eb, 0xd610, 0xd635, 0xd659, 0xd67e, 0xd6a3, 0xd6c7, 0xd6eb, 0xd710,
    0xd734, 0xd758, 0xd77c, 0xd7a0, 0xd7c4, 0xd7e8, 0xd80c, 0xd830, 0xd853, 0xd877, 0xd89a, 0xd8be,
    0xd8e1, 0xd904, 0xd927, 0xd94a, 0xd96d, 0xd990, 0xd9b3, 0xd9d6, 0xd9f9, 0xda1b, 0xda3e, 0xda60,
    0xda83, 0xdaa5, 0xdac7, 0xdae9, 0xdb0b, 0xdb2e, 0xdb4f, 0xdb71, 0xdb93, 0xdbb5, 0xdbd7, 0xdbf8,
    0xdc1a, 0xdc3b, 0xdc5c, 0xdc7e, 0xdc9f, 0xdcc0, 0xdce1, 0xdd02, 0xdd23, 0xdd44, 0xdd65, 0xdd85,
    0xdda6, 0xddc7, 0xdde7, 0xde08, 0xde28, 0xde48, 0xde69, 0xde89, 0xdea9, 0xdec9, 0xdee9, 0xdf09,
    0xdf29, 0xdf49, 0xdf68, 0xdf88, 0xdfa7, 0xdfc7, 0xdfe6, 0xe006, 0xe025, 0xe044, 0xe064, 0xe083,
    0xe0a2, 0xe0c1, 0xe0e0, 0xe0ff, 0xe11d, 0xe13c, 0xe15b, 0xe179, 0xe198, 0xe1b7, 0xe1d5, 0xe1f3,
    0xe212, 0xe230, 0xe24e, 0xe26c, 0xe28a, 0xe2a8, 0xe2c6, 0xe2e4, 0xe302, 0xe320, 0xe33d, 0xe35b,
    0xe379, 0xe396, 0xe3b4, 0xe3d1, 0xe3ee, 0xe40c, 0xe429, 0xe446, 0xe463, 0xe480, 0xe49d, 0xe4ba,
    0xe4d7, 0xe4f4, 0xe511, 0xe52d, 0xe54a, 0xe567, 0xe583, 0xe5a0, 0xe5bc, 0xe5d9, 0xe5f5, 0xe611,
    0xe62d, 0xe649, 0xe666, 0xe682, 0xe69e, 0xe6ba, 0xe6d5, 0xe6f1, 0xe70d, 0xe729, 0xe744, 0xe760,
    0xe77c, 0xe797, 0xe7b3, 0xe7ce, 0xe7e9, 0xe805, 0xe820, 0xe83b, 0xe856, 0xe871, 0xe88c, 0xe8a7,
    0xe8c2, 0xe8dd, 0xe8f8, 0xe913, 0xe92e, 0xe948, 0xe963, 0xe97d, 0xe998, 0xe9b3, 0xe9cd, 0xe9e7,
    0xea02, 0xea1c, 0xea36, 0xea50, 0xea6b, 0xea85, 0xea9f, 0xeab9, 0xead3, 0xeaec, 0xeb06, 0xeb20,
    0xeb3a, 0xeb54, 0xeb6d, 0xeb87, 0xeba0, 0xebba, 0xebd3, 0xebed, 0xec06, 0xec20, 0xec39, 0xec52,
    0xec6b, 0xec84, 0xec9d, 0xecb7, 0xecd0, 0xece9, 0xed01, 0xed1a, 0xed33, 0xed4c, 0xed65, 0xed7d,
    0xed96, 0xedaf, 0xedc7, 0xede0, 0xedf8, 0xee11, 0xee29, 0xee41, 0xee5a, 0xee72, 0xee8a, 0xeea2,
    0xeeba, 0xeed2, 0xeeea, 0xef02, 0xef1a, 0xef32, 0xef4a, 0xef62, 0xef7a, 0xef91, 0xefa9, 0xefc1,
    0xefd8, 0xeff0, 0xf008, 0xf01f, 0xf036, 0xf04e, 0xf065, 0xf07d, 0xf094, 0xf0ab, 0xf0c2, 0xf0d9,
    0xf0f1, 0xf108, 0xf11f, 0xf136, 0xf14d, 0xf164, 0xf17a, 0xf191, 0xf1a8, 0xf1bf, 0xf1d6, 0xf1ec,
    0xf203, 0xf219, 0xf230, 0xf247, 0xf25d, 0xf274, 0xf28a, 0xf2a0, 0xf2b7, 0xf2cd, 0xf2e3, 0xf2f9,
    0xf310, 0xf326, 0xf33c, 0xf352, 0xf368, 0xf37e, 0xf394, 0xf3aa, 0xf3c0, 0xf3d6, 0xf3eb, 0xf401,
    0xf417, 0xf42d, 0xf442, 0xf458, 0xf46d, 0xf483, 0xf499, 0xf4ae, 0xf4c3, 0xf4d9, 0xf4ee, 0xf504,
    0xf519, 0xf52e, 0xf543, 0xf559, 0xf56e, 0xf583, 0xf598, 0xf5ad, 0xf5c2, 0xf5d7, 0xf5ec, 0xf601,
    0xf616, 0xf62b, 0xf63f, 0xf654, 0xf669, 0xf67e, 0xf692, 0xf6a7, 0xf6bc, 0xf6d0, 0xf6e5, 0xf6f9,
    0xf70e, 0xf722, 0xf737, 0xf74b, 0xf75f, 0xf774, 0xf788, 0xf79c, 0xf7b0, 0xf7c5, 0xf7d9, 0xf7ed,
    0xf801, 0xf815, 0xf829, 0xf83d, 0xf851, 0xf865, 0xf879, 0xf88d, 0xf8a0, 0xf8b4, 0xf8c8, 0xf8dc,
    0xf8ef, 0xf903, 0xf917, 0xf92a, 0xf93e, 0xf952, 0xf965, 0xf979, 0xf98c, 0xf99f, 0xf9b3, 0xf9c6,
    0xf9d9, 0xf9ed, 0xfa00, 0xfa13, 0xfa27, 0xfa3a, 0xfa4d, 0xfa60, 0xfa73, 0xfa86, 0xfa99, 0xfaac,
    0xfabf, 0xfad2, 0xfae5, 0xfaf8, 0xfb0b, 0xfb1e, 0xfb30, 0xfb43, 0xfb56, 0xfb69, 0xfb7b, 0xfb8e,
    0xfba0, 0xfbb3, 0xfbc6, 0xfbd8, 0xfbeb, 0xfbfd, 0xfc10, 0xfc22, 0xfc34, 0xfc47, 0xfc59, 0xfc6b,
    0xfc7e, 0xfc90, 0xfca2, 0xfcb4, 0xfcc7, 0xfcd9, 0xfceb, 0xfcfd, 0xfd0f, 0xfd21, 0xfd33, 0xfd45,
    0xfd57, 0xfd69, 0xfd7b, 0xfd8d, 0xfd9e, 0xfdb0, 0xfdc2, 0xfdd4, 0xfde6, 0xfdf7, 0xfe09, 0xfe1b,
    0xfe2c, 0xfe3e, 0xfe4f, 0xfe61, 0xfe73, 0xfe84, 0xfe95, 0xfea7, 0xfeb8, 0xfeca, 0xfedb, 0xfeec,
    0xfefe, 0xff0f, 0xff20, 0xff32, 0xff43, 0xff54, 0xff65, 0xff76, 0xff87, 0xff98, 0xffaa, 0xffbb,
    0xffcc, 0xffdd, 0xffee, 0xffff,
};
static const uint16_t cgsHdrTone_hlg_ab[512] = {
    0x0000, 0x004a, 0x0094, 0x00de, 0x0128, 0x0172, 0x01bc, 0x0206, 0x0250, 0x029a, 0x02e4, 0x032e,
    0x0378, 0x03c2, 0x040c, 0x0456, 0x04a0, 0x04ea, 0x0534, 0x057e, 0x05c8, 0x0612, 0x065c, 0x06a7,
    0x06f1, 0x073b, 0x0785, 0x07cf, 0x0819, 0x0863, 0x08ad, 0x08f7, 0x0941, 0x098b, 0x09d5, 0x0a1f,
    0x0a69, 0x0ab3, 0x0afd, 0x0b47, 0x0b91, 0x0bdb, 0x0c25, 0x0c6f, 0x0cb9, 0x0d03, 0x0d4e, 0x0d98,
    0x0de2, 0x0e2c, 0x0e76, 0x0ec0, 0x0f0a, 0x0f54, 0x0f9e, 0x0fe8, 0x1032, 0x107c, 0x10c6, 0x1110,
    0x115a, 0x11a4, 0x11ee, 0x1238, 0x1282, 0x12cc, 0x1316, 0x1360, 0x13ab, 0x13f5, 0x143f, 0x1489,
    0x14d3, 0x151d, 0x1567, 0x15b1, 0x15fb, 0x1645, 0x168f, 0x16d9, 0x1723, 0x176d, 0x17b7, 0x1801,
    0x184b, 0x1895, 0x18df, 0x1929, 0x1973, 0x19bd, 0x1a07, 0x1a52, 0x1a9c, 0x1ae6, 0x1b30, 0x1b7a,
    0x1bc4, 0x1c0e, 0x1c58, 0x1ca2, 0x1cec, 0x1d36, 0x1d80, 0x1dca, 0x1e14, 0x1e5e, 0x1ea8, 0x1ef2,
    0x1f3c, 0x1f86, 0x1fd0, 0x201a, 0x2064, 0x20af, 0x20f9, 0x2143, 0x218d, 0x21d7, 0x2221, 0x226b,
    0x22b5, 0x22ff, 0x2349, 0x2393, 0x23dd, 0x2427, 0x2471, 0x24bb, 0x2505, 0x254f, 0x2599, 0x25e3,
    0x262d, 0x2677, 0x26c1, 0x270b, 0x2756, 0x27a0, 0x27ea, 0x2834, 0x287e, 0x28c8, 0x2912, 0x295c,
    0x29a6, 0x29f0, 0x2a3a, 0x2a84, 0x2ace, 0x2b18, 0x2b62, 0x2bac, 0x2bf6, 0x2c40, 0x2c8a, 0x2cd4,
    0x2d1e, 0x2d68, 0x2db3, 0x2dfd, 0x2e47, 0x2e91, 0x2edb, 0x2f25, 0x2f6f, 0x2fb9, 0x3003, 0x304d,
    0x3097, 0x30e1, 0x312b, 0x3175, 0x31bf, 0x3209, 0x3253, 0x329d, 0x32e7, 0x3331, 0x337b, 0x33c5,
    0x340f, 0x345a, 0x34a4, 0x34ee, 0x3538, 0x3582, 0x35cc, 0x3616, 0x3660, 0x36aa, 0x36f4, 0x373e,
    0x3788, 0x37d2, 0x381c, 0x3866, 0x38b0, 0x38fa, 0x3944, 0x398e, 0x39d8, 0x3a22, 0x3a6c, 0x3ab6,
    0x3b01, 0x3b4b, 0x3b95, 0x3bdf, 0x3c29, 0x3c73, 0x3cbd, 0x3d07, 0x3d51, 0x3d9b, 0x3de5, 0x3e2f,
    0x3e79, 0x3ec3, 0x3f0d, 0x3f57, 0x3fa1, 0x3feb, 0x4035, 0x407f, 0x40c9, 0x4113, 0x415e, 0x41a8,
    0x41f2, 0x423c, 0x4286, 0x42d0, 0x431a, 0x4364, 0x43ae, 0x43f8, 0x4442, 0x448c, 0x44d6, 0x4520,
    0x456a, 0x45b4, 0x45fe, 0x4648, 0x4692, 0x46dc, 0x4726, 0x4770, 0x47ba, 0x4805, 0x484f, 0x4899,
    0x48e3, 0x492d, 0x4977, 0x49c1, 0x4a0b, 0x4a55, 0x4aa1, 0x4aec, 0x4b38, 0x4b85, 0x4bd2, 0x4c20,
    0x4c6e, 0x4cbd, 0x4d0d, 0x4d5c, 0x4dad, 0x4dfe, 0x4e50, 0x4ea2, 0x4ef5, 0x4f48, 0x4f9c, 0x4ff0,
    0x5045, 0x509b, 0x50f1, 0x5148, 0x519f, 0x51f7, 0x5250, 0x52a9, 0x5302, 0x535d, 0x53b8, 0x5413,
    0x546f, 0x54cc, 0x552a, 0x5588, 0x55e6, 0x5646, 0x56a6, 0x5706, 0x5767, 0x57c9, 0x582c, 0x588f,
    0x58f3, 0x5957, 0x59bc, 0x5a22, 0x5a88, 0x5aef, 0x5b57, 0x5bc0, 0x5c29, 0x5c92, 0x5cfd, 0x5d68,
    0x5dd4, 0x5e41, 0x5eae, 0x5f1c, 0x5f8a, 0x5ffa, 0x606a, 0x60db, 0x614c, 0x61bf, 0x6232, 0x62a5,
    0x631a, 0x638f, 0x6405, 0x647c, 0x64f3, 0x656b, 0x65e4, 0x665e, 0x66d9, 0x6754, 0x67d0, 0x684d,
    0x68ca, 0x6949, 0x69c8, 0x6a48, 0x6ac9, 0x6b4a, 0x6bcd, 0x6c50, 0x6cd4, 0x6d59, 0x6dde, 0x6e65,
    0x6eec, 0x6f74, 0x6ffd, 0x7087, 0x7112, 0x719d, 0x722a, 0x72b7, 0x7345, 0x73d4, 0x7464, 0x74f5,
    0x7587, 0x7619, 0x76ac, 0x7741, 0x77d6, 0x786c, 0x7903, 0x799b, 0x7a34, 0x7ace, 0x7b69, 0x7c04,
    0x7ca1, 0x7d3f, 0x7ddd, 0x7e7d, 0x7f1d, 0x7fbe, 0x8061, 0x8104, 0x81a8, 0x824e, 0x82f4, 0x839b,
    0x8444, 0x84ed, 0x8597, 0x8643, 0x86ef, 0x879c, 0x884b, 0x88fa, 0x89ab, 0x8a5c, 0x8b0f, 0x8bc2,
    0x8c77, 0x8d2d, 0x8de4, 0x8e9c, 0x8f55, 0x900f, 0x90ca, 0x9186, 0x9244, 0x9302, 0x93c2, 0x9483,
    0x9545, 0x9608, 0x96cc, 0x9791, 0x9858, 0x9920, 0x99e8, 0x9ab2, 0x9b7e, 0x9c4a, 0x9d18, 0x9de6,
    0x9eb6, 0x9f88, 0xa05a, 0xa12e, 0xa203, 0xa2d9, 0xa3b0, 0xa489, 0xa562, 0xa63e, 0xa71a, 0xa7f8,
    0xa8d7, 0xa9b7, 0xaa98, 0xab7b, 0xac5f, 0xad45, 0xae2c, 0xaf14, 0xaffd, 0xb0e8, 0xb1d4, 0xb2c2,
    0xb3b1, 0xb4a1, 0xb593, 0xb686, 0xb77a, 0xb870, 0xb967, 0xba60, 0xbb5a, 0xbc55, 0xbd52, 0xbe51,
    0xbf51, 0xc052, 0xc155, 0xc259, 0xc35f, 0xc466, 0xc56f, 0xc679, 0xc785, 0xc892, 0xc9a1, 0xcab1,
    0xcbc3, 0xccd7, 0xcdec, 0xcf03, 0xd01b, 0xd135, 0xd250, 0xd36d, 0xd48c, 0xd5ac, 0xd6ce, 0xd7f1,
    0xd917, 0xda3e, 0xdb66, 0xdc90, 0xddbc, 0xdeea, 0xe019, 0xe14a, 0xe27d, 0xe3b2, 0xe4e8, 0xe620,
    0xe75a, 0xe895, 0xe9d2, 0xeb11, 0xec52, 0xed95, 0xeeda, 0xf020, 0xf168, 0xf2b2, 0xf3fe, 0xf54c,
    0xf69b, 0xf7ed, 0xf940, 0xfa96, 0xfbed, 0xfd46, 0xfea1, 0xffff,
};
static const uint16_t cgsHdrTone_hlg_ba[512] = {
    0x0000, 0x001d, 0x0053, 0x0099, 0x00eb, 0x0149, 0x01b1, 0x0221, 0x029b, 0x031b, 0x03a4, 0x0433,
    0x04c9, 0x0565, 0x0608, 0x06b0, 0x075e, 0x0812, 0x08cb, 0x0989, 0x0a4c, 0x0b14, 0x0be1, 0x0cb3,
    0x0d8a, 0x0e64, 0x0f44, 0x1027, 0x110f, 0x11fb, 0x12ec, 0x13e0, 0x14d8, 0x15d4, 0x16d4, 0x17d8,
    0x18df, 0x19ea, 0x1af9, 0x1c0b, 0x1d21, 0x1e3b, 0x1f58, 0x2077, 0x2195, 0x22b1, 0x23ca, 0x24e0,
    0x25f4, 0x2705, 0x2814, 0x2920, 0x2a2b, 0x2b32, 0x2c38, 0x2d3b, 0x2e3c, 0x2f3b, 0x3038, 0x3133,
    0x322c, 0x3322, 0x3417, 0x350a, 0x35fb, 0x36ea, 0x37d8, 0x38c3, 0x39ad, 0x3a95, 0x3b7c, 0x3c61,
    0x3d44, 0x3e25, 0x3f05, 0x3fe4, 0x40c1, 0x419c, 0x4276, 0x434e, 0x4425, 0x44fb, 0x45cf, 0x46a2,
    0x4774, 0x4844, 0x4913, 0x49e1, 0x4aad, 0x4b78, 0x4c42, 0x4d0b, 0x4dd3, 0x4e99, 0x4f5f, 0x5023,
    0x50e6, 0x51a8, 0x5268, 0x5328, 0x53e7, 0x54a5, 0x5561, 0x561d, 0x56d8, 0x5791, 0x584a, 0x5902,
    0x59b8, 0x5a6e, 0x5b23, 0x5bd7, 0x5c8a, 0x5d3c, 0x5dee, 0x5e9e, 0x5f4e, 0x5ffc, 0x60aa, 0x6157,
    0x6204, 0x62af, 0x635a, 0x6404, 0x64ad, 0x6555, 0x65fd, 0x66a3, 0x6749, 0x67ef, 0x6893, 0x6937,
    0x69da, 0x6a7d, 0x6b1e, 0x6bc0, 0x6c60, 0x6d00, 0x6d9f, 0x6e3d, 0x6edb, 0x6f78, 0x7014, 0x70b0,
    0x714b, 0x71e6, 0x727f, 0x7319, 0x73b1, 0x7449, 0x74e1, 0x7578, 0x760e, 0x76a4, 0x7739, 0x77ce,
    0x7862, 0x78f5, 0x7988, 0x7a1b, 0x7aad, 0x7b3e, 0x7bcf, 0x7c5f, 0x7cef, 0x7d7e, 0x7e0d, 0x7e9b,
    0x7f29, 0x7fb6, 0x8043, 0x80cf, 0x815b, 0x81e6, 0x8271, 0x82fb, 0x8385, 0x840e, 0x8497, 0x8520,
    0x85a8, 0x862f, 0x86b6, 0x873d, 0x87c3, 0x8849, 0x88ce, 0x8953, 0x89d8, 0x8a5c, 0x8ae0, 0x8b63,
    0x8be6, 0x8c68, 0x8cea, 0x8d6c, 0x8ded, 0x8e6e, 0x8eef, 0x8f6f, 0x8fee, 0x906e, 0x90ec, 0x916b,
    0x91e9, 0x9267, 0x92e4, 0x9361, 0x93de, 0x945a, 0x94d6, 0x9552, 0x95cd, 0x9648, 0x96c3, 0x973d,
    0x97b7, 0x9830, 0x98a9, 0x9922, 0x999b, 0x9a13, 0x9a8b, 0x9b02, 0x9b79, 0x9bf0, 0x9c67, 0x9cdd,
    0x9d53, 0x9dc8, 0x9e3e, 0x9eb3, 0x9f27, 0x9f9b, 0xa00f, 0xa083, 0xa0f7, 0xa16a, 0xa1dd, 0xa24f,
    0xa2c1, 0xa333, 0xa3a5, 0xa416, 0xa487, 0xa4f8, 0xa569, 0xa5d9, 0xa649, 0xa6b8, 0xa728, 0xa797,
    0xa806, 0xa874, 0xa8e3, 0xa951, 0xa9be, 0xaa2c, 0xaa99, 0xab06, 0xab73, 0xabdf, 0xac4b, 0xacb7,
    0xad23, 0xad8e, 0xadfa, 0xae65, 0xaecf, 0xaf3a, 0xafa4, 0xb00e, 0xb077, 0xb0e1, 0xb14a, 0xb1b3,
    0xb21c, 0xb284, 0xb2ed, 0xb355, 0xb3bc, 0xb424, 0xb48b, 0xb4f2, 0xb559, 0xb5c0, 0xb626, 0xb68d,
    0xb6f3, 0xb758, 0xb7be, 0xb823, 0xb888, 0xb8ed, 0xb952, 0xb9b6, 0xba1b, 0xba7f, 0xbae2, 0xbb46,
    0xbba9, 0xbc0d, 0xbc70, 0xbcd2, 0xbd35, 0xbd97, 0xbdfa, 0xbe5c, 0xbebd, 0xbf1f, 0xbf80, 0xbfe1,
    0xc042, 0xc0a3, 0xc104, 0xc164, 0xc1c4, 0xc224, 0xc284, 0xc2e4, 0xc343, 0xc3a2, 0xc401, 0xc460,
    0xc4bf, 0xc51d, 0xc57c, 0xc5da, 0xc638, 0xc696, 0xc6f3, 0xc751, 0xc7ae, 0xc80b, 0xc868, 0xc8c4,
    0xc921, 0xc97d, 0xc9da, 0xca36, 0xca91, 0xcaed, 0xcb48, 0xcba4, 0xcbff, 0xcc5a, 0xccb5, 0xcd0f,
    0xcd6a, 0xcdc4, 0xce1e, 0xce78, 0xced2, 0xcf2c, 0xcf85, 0xcfdf, 0xd038, 0xd091, 0xd0ea, 0xd142,
    0xd19b, 0xd1f3, 0xd24b, 0xd2a3, 0xd2fb, 0xd353, 0xd3ab, 0xd402, 0xd45a, 0xd4b1, 0xd508, 0xd55e,
    0xd5b5, 0xd60c, 0xd662, 0xd6b8, 0xd70e, 0xd764, 0xd7ba, 0xd810, 0xd865, 0xd8bb, 0xd910, 0xd965,
    0xd9ba, 0xda0f, 0xda63, 0xdab8, 0xdb0c, 0xdb61, 0xdbb5, 0xdc09, 0xdc5d, 0xdcb0, 0xdd04, 0xdd57,
    0xddaa, 0xddfe, 0xde51, 0xdea3, 0xdef6, 0xdf49, 0xdf9b, 0xdfee, 0xe040, 0xe092, 0xe0e4, 0xe136,
    0xe187, 0xe1d9, 0xe22a, 0xe27c, 0xe2cd, 0xe31e, 0xe36f, 0xe3c0, 0xe410, 0xe461, 0xe4b1, 0xe501,
    0xe552, 0xe5a2, 0xe5f2, 0xe641, 0xe691, 0xe6e1, 0xe730, 0xe77f, 0xe7cf, 0xe81e, 0xe86d, 0xe8bb,
    0xe90a, 0xe959, 0xe9a7, 0xe9f5, 0xea44, 0xea92, 0xeae0, 0xeb2e, 0xeb7c, 0xebc9, 0xec17, 0xec64,
    0xecb2, 0xecff, 0xed4c, 0xed99, 0xede6, 0xee32, 0xee7f, 0xeecc, 0xef18, 0xef64, 0xefb1, 0xeffd,
    0xf049, 0xf095, 0xf0e0, 0xf12c, 0xf178, 0xf1c3, 0xf20e, 0xf25a, 0xf2a5, 0xf2f0, 0xf33b, 0xf385,
    0xf3d0, 0xf41b, 0xf465, 0xf4b0, 0xf4fa, 0xf544, 0xf58e, 0xf5d8, 0xf622, 0xf66c, 0xf6b6, 0xf6ff,
    0xf749, 0xf792, 0xf7db, 0xf825, 0xf86e, 0xf8b7, 0xf8ff, 0xf948, 0xf991, 0xf9da, 0xfa22, 0xfa6a,
    0xfab3, 0xfafb, 0xfb43, 0xfb8b, 0xfbd3, 0xfc1b, 0xfc63, 0xfcaa, 0xfcf2, 0xfd39, 0xfd81, 0xfdc8,
    0xfe0f, 0xfe56, 0xfe9d, 0xfee4, 0xff2b, 0xff71, 0xffb8, 0xffff,
};


/* The four M curves, one 'para' each, as a function type and three parameters.
   All three components of a tag repeat one of these, and the pair differs
   between the two directions: the forward curve carries the quantizer's
   exponent and scale, the reverse curve the two values that invert it. */
struct cgs_hdr_m {
    int function;
    int32_t word[3];
};

static const struct cgs_hdr_m cgsHdrM[2][2] = {
    {   /* PQ */
        { 1, { 0x50000, 0x2830b, 0x0 } },        /* forward */
        { 1, { 0x3333, 0x7, 0x0 } }              /* reverse */
    },
    {   /* HLG */
        { 1, { 0x20000, 0x1f309, 0x0 } },        /* forward */
        { 1, { 0x5555, 0x435e, 0x0 } }           /* reverse */
    }
};

/* The CLUT every lut tag here carries, transcribed rather than decoded.  Its
   forty-four bytes are identical in all sixteen tags and in both directions.
   The two grid sizes and the precision that ought to account for the block do
   not explain all of it -- a 2-point grid on three inputs and outputs is
   twenty-four bytes, not forty-four -- so the block is stored as it stands
   rather than reconstructed from a reading of its own header. */
static const unsigned char cgsHdrCLUT[44] = {
    0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff,
    0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff
};

/* The six colourant matrices, each a forward half followed by a reverse one so
   that a row can point at both without naming them separately.  Rec. ITU-R
   BT.2020-1 and BT.2100 share a row, which is why there are six and not eight,
   and a PQ space and its HLG counterpart share theirs exactly -- the transfer
   function changes the curves and nothing here.

   The reverse halves are the inverses of the forward ones and are not close to
   them: the forward matrices are near the primaries they name, while the
   reverse ones carry the large cancelling terms that make the inversion exact at
   quantisation. */
static const int32_t cgsHdrMatrix2020[18] = {
    /* forward */
    0x5634, 0x1534, 0x1001, 0x23b7, 0x5671, 0x5d6, -0x3f, 0x3d6, 0x65ff,
    /* reverse */
    0x34b65, -0xc983, -0x78cd, -0x15d7d, 0x34b96, 0x691, 0xf2d, -0x2036, 0x281fe
};
static const int32_t cgsHdrMatrix709[18] = {
    /* forward */
    0x37d1, 0x314c, 0x1250, 0x1c7a, 0x5bc2, 0x7c2, 0x1c8, 0xc6d, 0x5b67,
    /* reverse */
    0x6449d, -0x33c0f, -0xfb33, -0x1f519, 0x3d51f, 0x111f, 0x24da, -0x753e, 0x2cf92
};
static const int32_t cgsHdrMatrixP3[18] = {
    /* forward */
    0x41ef, 0x255f, 0x141c, 0x1edf, 0x589b, 0x885, -0x22, 0x55c, 0x645c,
    /* reverse */
    0x4cedc, -0x1fad3, -0xcb9b, -0x1af37, 0x39903, 0x838, 0x18a8, -0x31da, 0x28c4e
};

/* One space's parameters.  Every field is a constant recovered from the profile
   the name resolves to, except the primaries, which the description names and
   the profile then does not store. */
struct cgs_hdr {
    const char *desc;
    const char *cprt;
    int created[6];
    /* Which sampled curves and which M curves this family uses: PQ is the first
       index and HLG the second. */
    int family;
    int32_t cicp[4];
    /* The reference luminance of the transfer function, which is the only
       non-zero coordinate of 'lumi': 100 for PQ and 203 for HLG. */
    int32_t lumi;
    /* The primaries this profile is named for, as nine s15Fixed16 in red,
       green, blue order.  The profile names them in its description and then
       stores no colorants, so CGColorSpaceIsWideGamutRGB is answered from
       these rather than from the profile; they are the very values Apple's
       gamma profiles for the same primaries carry, which is what makes the
       area test below come out as it does for those. */
    int32_t primaries[9];
};

/* One 'curv' block holding a sampled table: a signature, four reserved bytes,
   the entry count as a four-byte field, and that many u8Fixed8 values.  This is
   the form the two curve sections of a lut tag use, and it is why the curves
   above are sixteen-bit values rather than the parameters put_icc_curve
   writes. */
static size_t put_curve_sampled(unsigned char *p, const uint16_t *v, size_t n)
{
    size_t i;

    memcpy(p, "curv", 4);
    put_be32(p + 4, 0);
    put_be32(p + 8, (int32_t)n);
    for (i = 0; i < n; i++)
        put_be16(p + 12 + 2 * i, v[i]);
    return 12 + 2 * n;
}

/* One 'curv' block with a count of zero, which is the identity.  The B curves of
   every lut tag here are three of these. */
static void put_curve_identity(unsigned char *p)
{
    memcpy(p, "curv", 4);
    put_be32(p + 4, 0);
    put_be32(p + 8, 0);
}

/* One lut tag: 'mAB ' when reverse is zero and 'mBA ' when it is not, and nothing
   else between the two.

   The header is thirty-two bytes: a signature, four reserved, three input
   channels, three output channels, two more reserved, then the five section
   offsets.  The two reserved bytes matter -- the offsets start on the third, so
   a writer that treats the channel counts as four-byte fields reads every
   section from the wrong place.

   The identity curves and the matrix pad are written out rather than left to
   the calloc that cleared the profile, because the pad sits between two
   sections rather than at the end of the profile, and a block whose bytes
   depend on the allocator having zeroed memory is not one worth keeping. */
static void put_hdr_lut(unsigned char *p, int reverse, const int32_t *matrix,
    const uint16_t *tone, size_t toneCount, const struct cgs_hdr_m *m)
{
    static const int32_t section[5] = {
        CGICCHDRBOffset, CGICCHDRMatrixOffset, CGICCHDRMOffset,
        CGICCHDRCLUTOffset, CGICCHDRCurveOffset
    };
    size_t step = 12 + 2 * toneCount;
    int i, k;

    memcpy(p, reverse ? "mBA " : "mAB ", 4);
    put_be32(p + 4, 0);
    p[8] = 3;
    p[9] = 3;
    put_be16(p + 10, 0);
    for (i = 0; i < 5; i++)
        put_be32(p + 12 + 4 * i, section[i]);

    for (i = 0; i < 3; i++)
        put_curve_identity(p + CGICCHDRBOffset + i * CGICCHDRIdentityCurveLength);

    for (i = 0; i < 9; i++)
        put_be32(p + CGICCHDRMatrixOffset + 4 * i, matrix[i]);
    memset(p + CGICCHDRMatrixOffset + CGICCHDRMatrixLength, 0, CGICCHDRMatrixPad);

    for (i = 0; i < 3; i++) {
        unsigned char *q = p + CGICCHDRMOffset + i * CGICCHDRMCurveLength;

        memcpy(q, "para", 4);
        put_be32(q + 4, 0);
        put_be16(q + 8, (unsigned)m->function);
        put_be16(q + 10, 0);
        for (k = 0; k < 3; k++)
            put_be32(q + 12 + 4 * k, m->word[k]);
    }

    memcpy(p + CGICCHDRCLUTOffset, cgsHdrCLUT, sizeof cgsHdrCLUT);

    for (i = 0; i < 3; i++)
        put_curve_sampled(p + CGICCHDRCurveOffset + i * step, tone, toneCount);
}

/* Assemble the profile.  Eight blocks go out -- the two strings, the white point,
   the two lut tags, 'chad', 'cicp' and 'lumi' -- and the eight tags are pointed
   at them.

   The tag order matches the v4 template for as far as the two run, 'desc',
   'cprt' and 'wtpt' first, and then diverges: where the v4 profiles list nine
   per-component tags after the white point, these list the two lut tags in
   their place. */
static unsigned char *put_hdr(const struct cgs_hdr *v, const int32_t *matrix,
    size_t *outLen)
{
    static const char *const fixed[CGICCHDRTagCount] = {
        "desc", "cprt", "wtpt", "A2B0", "B2A0", "chad", "cicp", "lumi"
    };
    /* The white point and the Bradford inverse are the same in all eight of
       these spaces, and both match the values the v4 rows above carry for
       Display P3 -- which is to say the D65 pair the header illuminant also
       names.  So they are written once here rather than repeated per row. */
    static const int32_t bradfordInverse[9] = {
        0x10c42, 0x5de, -0xcda, 0x793, 0xfd90, -0x45e, -0x25d, 0x3dc, 0xc06e
    };
    static const int32_t whitePoint[3] = { 0xf6d5, 0x10000, 0xd32c };
    enum { desc, cprt, wtpt, A2B0, B2A0, chad, cicp, lumi };
    const uint16_t *tone[2];
    size_t toneCount[2], lutLen[2];
    unsigned char *p;
    size_t dlen, clen, len;
    int32_t tagOff[CGICCHDRTagCount], tagLen[CGICCHDRTagCount];
    int i;

    /* Which sampled curves this family uses, and how long they are.  PQ is
       sampled at 1024 entries and HLG at 512, which is why the four tables
       above differ in size and why the two families' profiles differ in length
       by more than the tags around them. */
    if (v->family) {
        tone[CGICCHDRForward] = cgsHdrTone_hlg_ab;
        tone[CGICCHDRReverse] = cgsHdrTone_hlg_ba;
        toneCount[CGICCHDRForward] =
            sizeof cgsHdrTone_hlg_ab / sizeof cgsHdrTone_hlg_ab[0];
        toneCount[CGICCHDRReverse] =
            sizeof cgsHdrTone_hlg_ba / sizeof cgsHdrTone_hlg_ba[0];
    } else {
        tone[CGICCHDRForward] = cgsHdrTone_pq_ab;
        tone[CGICCHDRReverse] = cgsHdrTone_pq_ba;
        toneCount[CGICCHDRForward] =
            sizeof cgsHdrTone_pq_ab / sizeof cgsHdrTone_pq_ab[0];
        toneCount[CGICCHDRReverse] =
            sizeof cgsHdrTone_pq_ba / sizeof cgsHdrTone_pq_ba[0];
    }

    /* Both lut tags of one space are the same length: the two directions carry
       the same table twice, at different counts only across families. */
    for (i = 0; i < 2; i++)
        lutLen[i] = CGICCHDRCurveOffset + 3 * (12 + 2 * toneCount[i]);

    /* put_mluc sizes itself from the string, so the two string lengths are
       worked out here rather than written down. */
    dlen = 28 + 2 * strlen(v->desc);
    clen = 28 + 2 * strlen(v->cprt);

    /* As in put_rgb_v4, the offsets are assigned in one pass and used again
       below, so the two cannot disagree about where a block went. */
    len = CGICCTagTableOffset + (size_t)CGICCHDRTagCount * CGICCTagEntrySize;
    tagLen[desc] = (int32_t)dlen;
    tagOff[desc] = (int32_t)icc_pad(len);
    len = icc_pad(len) + dlen;
    tagLen[cprt] = (int32_t)clen;
    tagOff[cprt] = (int32_t)icc_pad(len);
    len = icc_pad(len) + clen;
    tagLen[wtpt] = CGICCHDRLumiLength;
    tagOff[wtpt] = (int32_t)icc_pad(len);
    len = icc_pad(len) + CGICCHDRLumiLength;
    tagLen[A2B0] = (int32_t)lutLen[CGICCHDRForward];
    tagOff[A2B0] = (int32_t)icc_pad(len);
    len = icc_pad(len) + lutLen[CGICCHDRForward];
    tagLen[B2A0] = (int32_t)lutLen[CGICCHDRReverse];
    tagOff[B2A0] = (int32_t)icc_pad(len);
    len = icc_pad(len) + lutLen[CGICCHDRReverse];
    tagLen[chad] = CGICCHDRChadLength;
    tagOff[chad] = (int32_t)icc_pad(len);
    len = icc_pad(len) + CGICCHDRChadLength;
    tagLen[cicp] = CGICCHDRCicpLength;
    tagOff[cicp] = (int32_t)icc_pad(len);
    len = icc_pad(len) + CGICCHDRCicpLength;
    tagLen[lumi] = CGICCHDRLumiLength;
    tagOff[lumi] = (int32_t)icc_pad(len);
    len = icc_pad(len) + CGICCHDRLumiLength;
    /* Every block here is a multiple of four bytes, so the profile ends on a
       boundary and there is no trailing pad to account for. */
    len = icc_pad(len);

    p = calloc(1, len);
    if (!p)
        return NULL;

    put_rgb_v4_header(p, len, v->created, CGICCVersionV4, 0);
    put_be32(p + CGICCTagCountOffset, (uint32_t)CGICCHDRTagCount);
    for (i = 0; i < CGICCHDRTagCount; i++) {
        unsigned char *e = p + CGICCTagTableOffset + i * CGICCTagEntrySize;

        memcpy(e, fixed[i], 4);
        put_be32(e + 4, tagOff[i]);
        put_be32(e + 8, tagLen[i]);
    }

    put_mluc(p + tagOff[desc], v->desc);
    put_mluc(p + tagOff[cprt], v->cprt);
    put_xyz_i32(p + tagOff[wtpt], whitePoint);
    /* The matrix halves are adjacent in the table above, so the reverse tag
       reads the second nine and the forward tag the first. */
    put_hdr_lut(p + tagOff[A2B0], CGICCHDRForward, matrix,
        tone[CGICCHDRForward], toneCount[CGICCHDRForward],
        &cgsHdrM[v->family][CGICCHDRForward]);
    put_hdr_lut(p + tagOff[B2A0], CGICCHDRReverse, matrix + 9,
        tone[CGICCHDRReverse], toneCount[CGICCHDRReverse],
        &cgsHdrM[v->family][CGICCHDRReverse]);
    put_chad(p + tagOff[chad], bradfordInverse);
    put_cicp(p + tagOff[cicp], v->cicp);
    /* 'lumi' is an 'XYZ ' tag like 'wtpt', of which only Y is non-zero. */
    {
        int32_t l[3] = { 0, v->lumi, 0 };

        put_xyz_i32(p + tagOff[lumi], l);
    }

    *outLen = len;
    return p;
}

/* The four spaces, one row per family-and-primaries pair.  Two things separate
   these rows from the v4 ones above.  A description here names both the
   primaries and the transfer function, where a v4 description names the
   primaries alone and a separate 'cicp' tag carries the coding.  And there are
   six rows rather than eight, because the two 2100 names resolve to the very
   same spaces as the 2020 ones -- the same six profiles, under the same six
   spaces. */
static const struct cgs_hdr ITUR_2020_PQ = {
    .desc = "Rec. ITU-R BT.2100 PQ",
    .cprt = "Copyright Apple Inc., 2022",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .family = 0,
    .cicp = { 9, 16, 0, 1 },
    .lumi = 0x640000,
    .primaries = { 0xac69, 0x476f, -0x7f, 0x2a69, 0xace3, 0x7ad, 0x2003, 0xbad, 0xcbfe }
};
static const struct cgs_hdr ITUR_2020_HLG = {
    .desc = "Rec. ITU-R BT.2100 HLG",
    .cprt = "Copyright Apple Inc., 2022",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .family = 1,
    .cicp = { 9, 18, 0, 1 },
    .lumi = 0xcb0000,
    .primaries = { 0xac69, 0x476f, -0x7f, 0x2a69, 0xace3, 0x7ad, 0x2003, 0xbad, 0xcbfe }
};
static const struct cgs_hdr ITUR_709_PQ = {
    .desc = "Rec. ITU-R BT.709-5; SMPTE ST 2084 PQ",
    .cprt = "Copyright Apple Inc., 2022",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .family = 0,
    .cicp = { 1, 16, 0, 1 },
    .lumi = 0x640000,
    .primaries = { 0x6fa2, 0x38f5, 0x390, 0x6299, 0xb785, 0x18da, 0x24a0, 0xf84, 0xb6cf }
};
static const struct cgs_hdr ITUR_709_HLG = {
    .desc = "Rec. ITU-R BT.709-5; ARIB STD-B67 HLG",
    .cprt = "Copyright Apple Inc., 2022",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .family = 1,
    .cicp = { 1, 18, 0, 1 },
    .lumi = 0xcb0000,
    .primaries = { 0x6fa2, 0x38f5, 0x390, 0x6299, 0xb785, 0x18da, 0x24a0, 0xf84, 0xb6cf }
};
static const struct cgs_hdr DisplayP3_PQ = {
    .desc = "Display P3; SMPTE ST 2084 PQ",
    .cprt = "Copyright Apple Inc., 2022",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .family = 0,
    .cicp = { 12, 16, 0, 1 },
    .lumi = 0x640000,
    .primaries = { 0x83df, 0x3dbf, -0x45, 0x4abf, 0xb137, 0xab9, 0x2838, 0x110b, 0xc8b9 }
};
static const struct cgs_hdr DisplayP3_HLG = {
    .desc = "Display P3; ARIB STD-B67 HLG",
    .cprt = "Copyright Apple Inc., 2022",
    .created = { 2022, 1, 1, 0, 0, 0 },
    .family = 1,
    .cicp = { 12, 18, 0, 1 },
    .lumi = 0xcb0000,
    .primaries = { 0x83df, 0x3dbf, -0x45, 0x4abf, 0xb137, 0xab9, 0x2838, 0x110b, 0xc8b9 }
};

/* The ten names, and the space each one builds.  They are immortal
   singletons, as every other named space in this file is: two calls answer the
   same pointer at the immortal retain count.

   Six of the ten build six spaces rather than ten.  Four of the ten are second
   spellings of a space that already exists: the 2020 and the 2100 spelling of
   each HDR family are one space and not two, and the two EOTF spellings are
   likewise another name for a PQ space rather than a space of their own.
   Asking for any of those hands back the very pointer its sibling answers and
   reports the sibling's name, which is why the reported column differs from
   the name column on those rows and why the slot column repeats.

   Unlike the v4 table these rows carry no extended flag, because none of the
   ten names is an extended-range alias of another. */
enum {
    /* Ten names over six profiles.  The two extra rows are the EOTF spellings
       of the two PQ spaces, which Apple treats exactly like the 2100 pair:
       another spelling of a space that already exists, so it shares the slot
       and reports the name that names it. */
    CGColorSpaceNamedHDRCount = 10,
    /* One per profile, so the rows sharing a space share one. */
    CGColorSpaceNamedHDRSlots = 6
};

static const struct {
    const char *name;
    const char *reported;
    const struct cgs_hdr *profile;
    const int32_t *matrix;
    int slot;
} CGColorSpaceNamedHDR[CGColorSpaceNamedHDRCount] = {
    { "kCGColorSpaceITUR_2020_PQ", "kCGColorSpaceITUR_2100_PQ", &ITUR_2020_PQ, cgsHdrMatrix2020, 0 },
    { "kCGColorSpaceITUR_2100_PQ", "kCGColorSpaceITUR_2100_PQ", &ITUR_2020_PQ, cgsHdrMatrix2020, 0 },
    { "kCGColorSpaceITUR_2020_PQ_EOTF", "kCGColorSpaceITUR_2100_PQ", &ITUR_2020_PQ, cgsHdrMatrix2020, 0 },
    { "kCGColorSpaceITUR_2020_HLG", "kCGColorSpaceITUR_2100_HLG", &ITUR_2020_HLG, cgsHdrMatrix2020, 1 },
    { "kCGColorSpaceITUR_2100_HLG", "kCGColorSpaceITUR_2100_HLG", &ITUR_2020_HLG, cgsHdrMatrix2020, 1 },
    { "kCGColorSpaceITUR_709_PQ", "kCGColorSpaceITUR_709_PQ", &ITUR_709_PQ, cgsHdrMatrix709, 2 },
    { "kCGColorSpaceITUR_709_HLG", "kCGColorSpaceITUR_709_HLG", &ITUR_709_HLG, cgsHdrMatrix709, 3 },
    { "kCGColorSpaceDisplayP3_PQ", "kCGColorSpaceDisplayP3_PQ", &DisplayP3_PQ, cgsHdrMatrixP3, 4 },
    { "kCGColorSpaceDisplayP3_PQ_EOTF", "kCGColorSpaceDisplayP3_PQ", &DisplayP3_PQ, cgsHdrMatrixP3, 4 },
    { "kCGColorSpaceDisplayP3_HLG", "kCGColorSpaceDisplayP3_HLG", &DisplayP3_HLG, cgsHdrMatrixP3, 5 }
};

static struct CGColorSpace *CGColorSpaceNamedHDRState[CGColorSpaceNamedHDRSlots];

/* Build, or find, the space one of the ten HDR names resolves to.  Answers NULL
   for every other name, so the caller can hand it the name it failed to
   recognise and get the same answer back. */
static CGColorSpaceRef CGColorSpaceCreateNamedHDR(CFStringRef name)
{
    struct CGColorSpace *s;
    unsigned char *profile;
    size_t len;
    int i;

    for (i = 0; i < CGColorSpaceNamedHDRCount; i++) {
        int slot = CGColorSpaceNamedHDR[i].slot;

        if (!CGColorSpaceNameEqualsASCII(name, CGColorSpaceNamedHDR[i].name))
            continue;
        if (CGColorSpaceNamedHDRState[slot])
            return CGColorSpaceNamedHDRState[slot];
        profile = put_hdr(CGColorSpaceNamedHDR[i].profile,
            CGColorSpaceNamedHDR[i].matrix, &len);
        if (!profile)
            return NULL;
        s = calloc(1, sizeof *s);
        if (!s) {
            free(profile);
            return NULL;
        }
        s->immortal = true;
        s->model = kCGColorSpaceModelRGB;
        s->type = CGColorSpaceTypeICC;
        s->ncomp = 3;
        /* A literal like the device names', and not owned.  The reported
           spelling rather than the asked-for one, so the two 2020 names report
           the 2100 one. */
        s->name = CGColorSpaceNamedHDR[i].reported;
        s->profile = profile;
        s->profileLen = len;
        s->primaries = CGColorSpaceNamedHDR[i].profile->primaries;
        CGColorSpaceNamedHDRState[slot] = s;
        return s;
    }
    return NULL;
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

    /* The white point the caller asked for is kept even where the profile had
       to collapse it, because that is what Apple compares: two Lab spaces can
       carry the same 496 bytes and still be unequal because the points differ.
       Taken here, before lab_xyz has had a chance to reject the values. */
    s->hasWhitePoint = true;
    memcpy(s->whitePoint, whitePoint, sizeof s->whitePoint);

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

/* Whether three colorants describe a gamut wider than sRGB, from nine
   s15Fixed16 values in red, green, blue order.  Shared by the two ways a space
   can supply its primaries: read out of the profile's colorant tags, or kept
   beside the profile for one that stores none. */
static bool icc_primaries_are_wide(const int32_t colorants[9])
{
    double xy[3][2];
    double area;
    int i;

    for (i = 0; i < 3; i++) {
        double sum;

        sum = (double)colorants[3 * i] / 65536.0
            + (double)colorants[3 * i + 1] / 65536.0
            + (double)colorants[3 * i + 2] / 65536.0;
        if (!(sum > 0.0))
            return false;
        xy[i][0] = (double)colorants[3 * i] / 65536.0 / sum;
        xy[i][1] = (double)colorants[3 * i + 1] / 65536.0 / sum;
    }

    area = 0.5 * fabs(xy[0][0] * (xy[1][1] - xy[2][1])
        + xy[1][0] * (xy[2][1] - xy[0][1])
        + xy[2][0] * (xy[0][1] - xy[1][1]));
    return area > 0.13446869;
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
    int32_t colorants[9];
    size_t i;

    for (i = 0; i < 3; i++) {
        size_t tagLen;
        long at = icc_find_tag(p, len, prim[i], &tagLen);

        /* An XYZType is 'XYZ ', four reserved bytes, then three s15Fixed16 --
           the components start at offset 8, not 4. */
        if (at < 0 || tagLen < 20)
            return false;
        if (memcmp(p + at, "XYZ ", 4) != 0)
            return false;
        colorants[3 * i] = (int32_t)get_be32(p + at + 8);
        colorants[3 * i + 1] = (int32_t)get_be32(p + at + 12);
        colorants[3 * i + 2] = (int32_t)get_be32(p + at + 16);
    }

    return icc_primaries_are_wide(colorants);
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
    free(s->indexed);
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
    for (i = 0; i < CG_COLORSPACE_BUILT_IN_ALIAS_COUNT; i++) {
        if (CGColorSpaceNameEqualsASCII(name, CGColorSpaceBuiltInAliases[i].name))
            return CGColorSpaceBuiltInAliases[i].id;
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

/* The comparison behind both entry points, with ignoreRange selecting whether
   the range is part of the answer.  The flag is threaded through the recursion
   rather than consulted only at the top, because a pattern or indexed space
   reaches its range through its base and would otherwise keep it. */
static bool colorspace_equal(CGColorSpaceRef space1, CGColorSpaceRef space2,
    bool ignoreRange)
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
        return colorspace_equal(a->base, b->base, ignoreRange);
    /* A pattern space is never equal to a space that is not a pattern, and in
       particular is not equal to the space it wraps: Apple answers false for
       CGColorSpaceEqualToColorSpace(Pattern(rgb), rgb). */
    if (a->type == CGColorSpaceTypePattern || b->type == CGColorSpaceTypePattern)
        return false;
    /* Two indexed spaces are equal when they are the same lookup: same base,
       same table, same length.  Comparing the shape alone would call every
       pair of indexed spaces equal, since they all report model 5 and one
       component, so the table has to be compared -- and the length with it,
       because two tables can share a prefix and differ only by carrying
       different numbers of entries, which is exactly what changing the largest
       index does. */
    if (a->type == CGColorSpaceTypeIndexed || b->type == CGColorSpaceTypeIndexed) {
        if (a->type != b->type)
            return false;
        if (a->indexedLen != b->indexedLen)
            return false;
        if (!colorspace_equal(a->base, b->base, ignoreRange))
            return false;
        if (a->indexedLen
            && memcmp(a->indexed, b->indexed, a->indexedLen) != 0)
            return false;
        return true;
    }
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
       look identical here.  Its white point is in the same position, and is
       the clearer case: a white point too fine for s15Fixed16 is dropped from
       the profile altogether, so D65 and D50 produce the very same 496 bytes
       and are still unequal.  The generic Lab space recorded by name carries
       that same profile with no white point behind it, which is what keeps it
       from being equal to either of them.

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
        /* The output range is the first thing ignoreRange drops, then the
           extended flag, which is the same thing said the other way round:
           CGColorSpaceUsesExtendedRange is what reports it.  Neither is
           derivable from the profile -- an extended space's bytes are its
           base's byte for byte -- so both are compared here and both are
           skipped there.

           The linearized flag is kept even when the range is ignored.  It is
           not a range: a linearized space reports no extended range, so
           dropping it would merge answers that are about different spaces.  No
           pair Apple exposes distinguishes more than this, since every alias
           it has differs in the extended flag alone. */
        if (!ignoreRange) {
            if (a->hasRange != b->hasRange)
                return false;
            if (a->hasRange && memcmp(a->range, b->range, sizeof a->range) != 0)
                return false;
        }
        if (a->hasWhitePoint != b->hasWhitePoint)
            return false;
        if (a->hasWhitePoint
            && memcmp(a->whitePoint, b->whitePoint,
                sizeof a->whitePoint) != 0)
            return false;
        if ((!ignoreRange && a->extended != b->extended)
            || a->linearized != b->linearized)
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

bool CGColorSpaceEqualToColorSpace(CGColorSpaceRef space1,
    CGColorSpaceRef space2)
{
    return colorspace_equal(space1, space2, false);
}

bool CGColorSpaceEqualToColorSpaceIgnoringRange(CGColorSpaceRef space1,
    CGColorSpaceRef space2)
{
    /* Only the range and the extended flag are dropped.  Every space that has
       an extended alias is reported unequal to it, and equal here: the three
       extended RGB names and the extended gray one all take their base's
       profile byte for byte. */
    return colorspace_equal(space1, space2, true);
}

/* Capabilities. */

bool CGColorSpaceSupportsOutput(CGColorSpaceRef space)
{
    struct CGColorSpace *s = space;

    /* A pattern space describes a paint, not somewhere to paint, and an
       indexed space describes a lookup rather than a place a drawing can land,
       so neither can be a drawing destination. */
    return s != NULL && s->type != CGColorSpaceTypePattern
        && s->type != CGColorSpaceTypeIndexed;
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

    /* A space built from a profile normally answers from its colorants, and
       only an RGB one has the primaries the measure needs.  The extended flag
       still overrides, though: the extended-range linear sRGB hands back the
       very bytes the unextended one does, and calls that wide gamut while the
       other is not, so the profile alone cannot answer.  The type has to be
       tested before the flag path below, which is how the same profile handed
       to a calibrated RGB caller comes out a different answer.

       That override is itself confined to RGB, because a gray profile has no
       primaries to be wider than anything.  kCGColorSpaceExtendedLinearGray
       reports an extended range and is nevertheless not wide gamut, so
       letting the flag stand on its own would call it one. */
    if (s->type == CGColorSpaceTypeICC)
        return s->model == kCGColorSpaceModelRGB
               && (s->extended || (s->profile != NULL
                                   && (s->primaries != NULL
                                       ? icc_primaries_are_wide(s->primaries)
                                       : icc_rgb_is_wide(s->profile, s->profileLen))));

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
