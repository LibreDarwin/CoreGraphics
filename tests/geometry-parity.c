/* CoreGraphics - geometry-parity.c
   Copyright (C) 2026, LibreDarwin
   SPDX-License-Identifier: BSD-3-Clause

   Differential harness for the CoreGraphics value/geometry layer.

   This one source file is compiled twice by tests/geometry-parity.sh:

     - against Apple's CoreGraphics in the dyld shared cache, which is the
       oracle, and
     - against our built dylib, linked by explicit path so dyld loads ours
       rather than the cached copy.

   Both binaries must print byte-identical output.  Anything that differs is
   a behavioural difference from Apple.

   Output is deliberately line-oriented and fixed-format: hex for doubles
   (via %a) so signed zero, infinities and NaN payloads survive, and %d for
   booleans.  Comparing rendered decimals would hide -0.0 versus 0.0. */

#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFString.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CoreGraphics SPI that is exported by the framework but absent from the
   public SDK headers.  Declared here so this harness can exercise it; the
   same declarations live in src/CGSPI.h for the framework itself. */
extern bool CGPointNearlyEqualToPoint(CGPoint, CGPoint);
extern bool CGPointNearlyEqualToPointWithTolerance(CGPoint, CGPoint, CGFloat);
extern bool CGSizeNearlyEqualToSize(CGSize, CGSize);
extern bool CGSizeNearlyEqualToSizeWithTolerance(CGSize, CGSize, CGFloat);
extern bool CGVectorNearlyEqualToVector(CGVector, CGVector);
extern bool CGVectorNearlyEqualToVectorWithTolerance(CGVector, CGVector, CGFloat);
extern bool CGRectNearlyEqualToRect(CGRect, CGRect);
extern bool CGRectNearlyEqualToRectWithTolerance(CGRect, CGRect, CGFloat);
extern bool CGRectIsIntegral(CGRect);

/* The affine SPI.  Like the NearlyEqual family above these are in the
   SDK's .tbd, so the oracle can link and call them even though the public
   headers never declare them. */
extern bool CGAffineTransformIsSingular(const CGAffineTransform *);
extern bool CGAffineTransformIsRectilinear(const CGAffineTransform *);
extern CGAffineTransform CGAffineTransformMakeWithRect(CGRect);
extern bool CGAffineTransformDecompose_SPI(CGAffineTransform, CGSize *,
    CGFloat *, bool *, CGVector *);

/* The color space SPI: exported by the framework and listed in the SDK's
   CoreGraphics.tbd, so the oracle can link and call them, but absent from the
   public CGColorSpace.h.  These mirror src/CGSPI.h.

   Deliberately NOT declared or called here: CGColorSpaceGetIdentifier and
   CGColorSpaceGetMD5Digest, which fault in Apple on a space with no ICC
   profile; CGColorSpaceGetNames, CGColorSpaceGetColorants, and
   CGColorSpaceGetTintTransform, which abort
   the process on anything that is not a DeviceN space, and
   CGColorSpaceGetTintTransform, which aborts the same way.  There is no Apple
   answer to compare against for any of those five, so they are covered by
   inspection of the implementation instead. */
extern CGColorSpaceModel CGColorSpaceGetProcessColorModel(CGColorSpaceRef);
extern int CGColorSpaceGetType(CGColorSpaceRef);
extern int CGColorSpaceGetID(CGColorSpaceRef);
extern int CGColorSpaceIDFromName(CFStringRef);
extern CFStringRef CGColorSpaceNameFromID(int);
extern bool CGColorSpaceIsUncalibrated(CGColorSpaceRef);
extern bool CGColorSpaceIsICCCompatible(CGColorSpaceRef);
extern bool CGColorSpaceIsPSLevel2Compatible(CGColorSpaceRef);
extern bool CGColorSpaceEqualToColorSpace(CGColorSpaceRef, CGColorSpaceRef);
extern bool CGColorSpaceEqualToColorSpaceIgnoringRange(CGColorSpaceRef,
    CGColorSpaceRef);
extern int CGColorSpaceGetRenderingIntent(CGColorSpaceRef);
extern bool CGColorSpaceIgnoresIntent(CGColorSpaceRef);
extern bool CGColorSpaceUsesITUR_2100TF(CGColorSpaceRef);
extern CGColorSpaceRef CGColorSpaceGetAlternateColorSpace(CGColorSpaceRef);

static void p(const char *label, CGFloat v)
{
    printf("%s %a\n", label, (double)v);
}

static void r(const char *label, CGRect v)
{
    printf("%s %a %a %a %a\n", label, (double)v.origin.x, (double)v.origin.y,
        (double)v.size.width, (double)v.size.height);
}

static void pt(const char *label, CGPoint v)
{
    printf("%s %a %a\n", label, (double)v.x, (double)v.y);
}

static void sz(const char *label, CGSize v)
{
    printf("%s %a %a\n", label, (double)v.width, (double)v.height);
}

static void vec(const char *label, CGVector v)
{
    printf("%s %a %a\n", label, (double)v.dx, (double)v.dy);
}

static void b(const char *label, int v)
{
    printf("%s %d\n", label, v != 0);
}

static void tr(const char *label, CGAffineTransform t)
{
    printf("%s %a %a %a %a %a %a\n", label, (double)t.a, (double)t.b,
        (double)t.c, (double)t.d, (double)t.tx, (double)t.ty);
}

static void comp(const char *label, CGAffineTransformComponents v)
{
    printf("%s %a %a %a %a %a %a\n", label, (double)v.scale.width,
        (double)v.scale.height, (double)v.horizontalShear, (double)v.rotation,
        (double)v.translation.dx, (double)v.translation.dy);
}

/* Integers: model, type and ID numbers, component counts, CICP codes. */
static void n(const char *label, long long v)
{
    printf("%s %lld\n", label, v);
}

/* Print a CFString's contents, or "(null)" for a NULL one, so a name that
   Apple does not report compares equal to one of ours that does not. */
static void cfstr(const char *label, CFStringRef v)
{
    char buf[96];

    if (!v) {
        printf("%s (null)\n", label);
        return;
    }
    if (!CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8))
        printf("%s (unconvertible)\n", label);
    else
        printf("%s %s\n", label, buf);
}

/* An ICC profile as a single line: its length, the MD5 of its bytes, and
   its own profile ID.  The MD5 is what makes this a byte-for-byte check --
   the synthesis is only correct if all 380 bytes land in the same places,
   and comparing them as text would bury the diff under 760 hex digits.  The
   trailing fields are printed so that a mismatch is diagnosable from the
   transcript alone, without re-running anything.

   The ID is reported separately because it is the one field that is *not*
   independent: it is the MD5 of the finished profile, so it confirms the
   digest was computed over the same bytes rather than a stale copy.

   `mask_date' zeroes header bytes 24-35, the six big-endian 16-bit fields
   of the creation date, before hashing.  Lab stamps the local time of the
   call there instead of a constant, so two runs of the oracle seconds apart
   differ; without the mask the harness would report that clock skew as a
   synthesis difference and could never be a gate.  Every other byte is
   still compared, and the date is the only field any calibrated profile
   here leaves live. */
static void icc_impl(const char *label, CFDataRef d, int mask_date)
{
    unsigned char md[CC_MD5_DIGEST_LENGTH];
    unsigned char *tmp = NULL;
    const unsigned char *p;
    CFIndex len;

    if (!d) {
        printf("%s (null)\n", label);
        return;
    }
    p = CFDataGetBytePtr(d);
    len = CFDataGetLength(d);
    if (mask_date && len >= 36) {
        tmp = malloc((size_t)len);
        if (tmp) {
            memcpy(tmp, p, (size_t)len);
            memset(tmp + 24, 0, 12);
            p = tmp;
        } else {
            mask_date = 0;
        }
    }
    /* MD5 because that is the algorithm the ICC profile ID is defined in
       terms of, so a mismatch here and a mismatch there mean the same thing.
       This is a comparison key for a test, not a security decision. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5(p, (CC_LONG)len, md);
#pragma clang diagnostic pop
    printf("%s len=%ld md5=", label, (long)len);
    for (int i = 0; i < CC_MD5_DIGEST_LENGTH; i++) printf("%02x", md[i]);
    /* The profile ID is the 16 bytes at 84, and it is the last of the four
       regions that vary, so printing it catches a template that is right
       everywhere else.  For Lab it is deliberately all zeros: Apple leaves
       that field zero here rather than filling in the MD5, unlike the gray
       and RGB templates, so a zero here is the expected result and not a
       missing digest. */
    printf(" id=");
    if (len >= 100) {
        for (int i = 0; i < 16; i++) printf("%02x", p[84 + i]);
    } else {
        printf("(short)");
    }
    /* The declared size and the 'acsp' signature, so an implementation that
       built a structurally valid but differently-sized or mislabelled
       profile still shows why.  Both live in the header: the size is the
       first four bytes and the signature the last, at 36. */
    printf(" size=%u sig=%.4s\n",
        (unsigned)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
            ((uint32_t)p[2] << 8) | p[3]),
        len >= 40 ? (const char *)(p + 36) : "");
    free(tmp);
}

static void icc(const char *label, CFDataRef d)
{
    icc_impl(label, d, 0);
}

static void icc_live_date(const char *label, CFDataRef d)
{
    icc_impl(label, d, 1);
}

/* A spread of rects, including the degenerate cases where the
   null/infinite sentinels and the empty-vs-null distinction matter. */
static const CGRect rects[] = {
    { { 0, 0 }, { 0, 0 } },
    { { 1, 2 }, { 3, 4 } },
    { { -3.5, 2.25 }, { 7, 1.5 } },
    { { 5, 5 }, { -4, -4 } },          /* negative size: needs standardize */
    { { -0.0, 0.0 }, { 1, 1 } },       /* negative zero origin */
    { { 0.1, 0.2 }, { 0.3, 0.4 } },
    { { 1e300, -1e300 }, { 1e-300, 1e-300 } },
};

static const CGPoint points[] = {
    { 0, 0 }, { 1, 1 }, { -1, -1 }, { 0.5, -0.5 }, { 1e200, 1e-200 },
};

static const CGSize sizes[] = {
    { 0, 0 }, { 1, 1 }, { -1, -1 }, { 2.5, 3.5 },
};

int main(void)
{
    const size_t nr = sizeof rects / sizeof rects[0];
    const size_t np = sizeof points / sizeof points[0];
    const size_t ns = sizeof sizes / sizeof sizes[0];

    /* Constants.  These are the sentinels we could not read out of the
       extracted __TEXT-only binary; the oracle prints its own values so any
       mismatch is visible here. */
    r("const/CGRectNull", CGRectNull);
    r("const/CGRectInfinite", CGRectInfinite);
    r("const/CGRectZero", CGRectZero);
    pt("const/CGPointZero", CGPointZero);
    sz("const/CGSizeZero", CGSizeZero);
    tr("const/CGAffineTransformIdentity", CGAffineTransformIdentity);

    /* Per-rect accessors and predicates. */
    for (size_t i = 0; i < nr; i++) {
        char l[64];
        CGRect v = rects[i];

        snprintf(l, sizeof l, "g/minx/%zu", i); p(l, CGRectGetMinX(v));
        snprintf(l, sizeof l, "g/miny/%zu", i); p(l, CGRectGetMinY(v));
        snprintf(l, sizeof l, "g/maxx/%zu", i); p(l, CGRectGetMaxX(v));
        snprintf(l, sizeof l, "g/maxy/%zu", i); p(l, CGRectGetMaxY(v));
        snprintf(l, sizeof l, "g/midx/%zu", i); p(l, CGRectGetMidX(v));
        snprintf(l, sizeof l, "g/midy/%zu", i); p(l, CGRectGetMidY(v));
        snprintf(l, sizeof l, "g/width/%zu", i); p(l, CGRectGetWidth(v));
        snprintf(l, sizeof l, "g/height/%zu", i); p(l, CGRectGetHeight(v));
        snprintf(l, sizeof l, "g/isNull/%zu", i); b(l, CGRectIsNull(v));
        snprintf(l, sizeof l, "g/isEmpty/%zu", i); b(l, CGRectIsEmpty(v));
        snprintf(l, sizeof l, "g/isInfinite/%zu", i); b(l, CGRectIsInfinite(v));
        snprintf(l, sizeof l, "g/isIntegral/%zu", i); b(l, CGRectIsIntegral(v));
        snprintf(l, sizeof l, "g/std/%zu", i); r(l, CGRectStandardize(v));
        snprintf(l, sizeof l, "g/int/%zu", i); r(l, CGRectIntegral(v));
        snprintf(l, sizeof l, "g/inset/%zu", i);
        r(l, CGRectInset(v, 0.5, 0.25));
        snprintf(l, sizeof l, "g/offset/%zu", i);
        r(l, CGRectOffset(v, 1.5, -2.5));
        /* CGRectDivide(rect, slice, remainder, amount, edge): the line sits
           `amount' from `edge' and is parallel to that edge's side. */
        {
            static const CGRectEdge edges[4] = { CGRectMinXEdge, CGRectMaxXEdge,
                CGRectMinYEdge, CGRectMaxYEdge };
            for (int e = 0; e < 4; e++) {
                CGRect slice = CGRectZero, rem = CGRectZero;
                snprintf(l, sizeof l, "divide/%zu/%d", i, e);
                CGRectDivide(v, &slice, &rem, 1.5, edges[e]);
                r(l, slice);
                snprintf(l, sizeof l, "dividerem/%zu/%d", i, e); r(l, rem);
            }
        }
    }

    /* Containment, intersection, union. */
    for (size_t i = 0; i < nr; i++) {
        for (size_t j = 0; j < nr; j++) {
            char l[64];
            CGRect a = rects[i], c = rects[j];

            snprintf(l, sizeof l, "eq/%zu/%zu", i, j);
            b(l, CGRectEqualToRect(a, c));
            snprintf(l, sizeof l, "cont/%zu/%zu", i, j);
            b(l, CGRectContainsRect(a, c));
            snprintf(l, sizeof l, "ints/%zu/%zu", i, j);
            b(l, CGRectIntersectsRect(a, c));
            snprintf(l, sizeof l, "inter/%zu/%zu", i, j);
            r(l, CGRectIntersection(a, c));
            snprintf(l, sizeof l, "union/%zu/%zu", i, j);
            r(l, CGRectUnion(a, c));
            snprintf(l, sizeof l, "neareq/%zu/%zu", i, j);
            b(l, CGRectNearlyEqualToRectWithTolerance(a, c, 0.01));
        }
    }

    /* Point/size predicates and containment. */
    for (size_t i = 0; i < nr; i++) {
        for (size_t j = 0; j < np; j++) {
            char l[64];
            snprintf(l, sizeof l, "cpt/%zu/%zu", i, j);
            b(l, CGRectContainsPoint(rects[i], points[j]));
        }
    }
    for (size_t i = 0; i < np; i++) {
        for (size_t j = 0; j < np; j++) {
            char l[64];
            snprintf(l, sizeof l, "peq/%zu/%zu", i, j);
            b(l, CGPointEqualToPoint(points[i], points[j]));
            snprintf(l, sizeof l, "pneareq/%zu/%zu", i, j);
            b(l, CGPointNearlyEqualToPointWithTolerance(points[i], points[j], 1e-9));
        }
    }
    for (size_t i = 0; i < ns; i++) {
        for (size_t j = 0; j < ns; j++) {
            char l[64];
            snprintf(l, sizeof l, "seq/%zu/%zu", i, j);
            b(l, CGSizeEqualToSize(sizes[i], sizes[j]));
            snprintf(l, sizeof l, "sneareq/%zu/%zu", i, j);
            b(l, CGSizeNearlyEqualToSizeWithTolerance(sizes[i], sizes[j], 1e-9));
        }
    }

    /* Default-tolerance NearlyEqual: 2^-26.  Values straddling that
       threshold are the interesting ones. */
    {
        const CGFloat t = 0x1p-26;
        const CGFloat base = 1.0;
        CGFloat deltas[] = { 0, t / 2, t, t * 2, -t / 2, -t, -t * 2 };
        for (size_t i = 0; i < sizeof deltas / sizeof deltas[0]; i++) {
            char l[64];
            snprintf(l, sizeof l, "pnear/%zu", i);
            b(l, CGPointNearlyEqualToPointWithTolerance(CGPointMake(base, base),
                CGPointMake(base + deltas[i], base), 0));
            snprintf(l, sizeof l, "snear/%zu", i);
            b(l, CGSizeNearlyEqualToSizeWithTolerance(CGSizeMake(base, base),
                CGSizeMake(base + deltas[i], base), 0));
            snprintf(l, sizeof l, "rnear/%zu", i);
            b(l, CGRectNearlyEqualToRectWithTolerance(CGRectMake(base, base, 1, 1),
                CGRectMake(base + deltas[i], base, 1, 1), 0));
        }
    }

    /* Affine transforms. */
    {
        CGAffineTransform ts[] = {
            CGAffineTransformMake(1, 2, 3, 4, 5, 6),
            CGAffineTransformMakeTranslation(3, -4),
            CGAffineTransformMakeScale(2, 0.5),
            CGAffineTransformMakeRotation(0.75),
            CGAffineTransformIdentity,
        };
        const size_t nt = sizeof ts / sizeof ts[0];

        for (size_t i = 0; i < nt; i++) {
            char l[64];
            snprintf(l, sizeof l, "tid/%zu", i); b(l, CGAffineTransformIsIdentity(ts[i]));
            snprintf(l, sizeof l, "tconcat/%zu", i);
            tr(l, CGAffineTransformConcat(ts[i], ts[(i + 1) % nt]));
            snprintf(l, sizeof l, "tinv/%zu", i);
            tr(l, CGAffineTransformInvert(ts[i]));
            snprintf(l, sizeof l, "ttr/%zu", i);
            tr(l, CGAffineTransformTranslate(ts[i], 1.5, 2.5));
            snprintf(l, sizeof l, "tsc/%zu", i);
            tr(l, CGAffineTransformScale(ts[i], 3, 4));
            snprintf(l, sizeof l, "trot/%zu", i);
            tr(l, CGAffineTransformRotate(ts[i], 1.25));

            for (size_t k = 0; k < np; k++) {
                snprintf(l, sizeof l, "tpt/%zu/%zu", i, k);
                pt(l, CGPointApplyAffineTransform(points[k], ts[i]));
            }
            for (size_t k = 0; k < ns; k++) {
                snprintf(l, sizeof l, "tsz/%zu/%zu", i, k);
                sz(l, CGSizeApplyAffineTransform(sizes[k], ts[i]));
            }
            for (size_t k = 0; k < nr; k++) {
                snprintf(l, sizeof l, "trc/%zu/%zu", i, k);
                r(l, CGRectApplyAffineTransform(rects[k], ts[i]));
            }
        }

        /* Singular matrices: Invert must report and return identity. */
        {
            CGAffineTransform sing = CGAffineTransformMake(0, 1, 0, 0, 0, 0);
            tr("tsing/inv", CGAffineTransformInvert(sing));
            b("tsing/isId", CGAffineTransformIsIdentity(CGAffineTransformInvert(sing)));
            pt("tsing/pt", CGPointApplyAffineTransform(CGPointMake(3, 4), sing));
        }

        /* Equality. */
        for (size_t i = 0; i < nt; i++) {
            for (size_t j = 0; j < nt; j++) {
                char l[64];
                snprintf(l, sizeof l, "teq/%zu/%zu", i, j);
                b(l, CGAffineTransformEqualToTransform(ts[i], ts[j]));
            }
        }
    }

    /* The affine SPI.

       The IsSingular fixtures deliberately include a transform whose
       determinant is 1 - 2^-54: that product is inexact and rounds to 1,
       so a separately rounded a*d - b*c comes out exactly 0 and reports
       the matrix singular, while the fused form keeps the -2^-54 and does
       not.  The two spellings therefore disagree on this one input, which
       is what makes the case worth having. */
    {
        CGAffineTransform ats[] = {
            CGAffineTransformMake(1, 2, 3, 4, 5, 6),
            CGAffineTransformMake(0, 1, 0, 0, 0, 0),   /* a*d - b*c == 0 */
            CGAffineTransformMake(0, 0, 0, 0, 0, 0),   /* all zero */
            CGAffineTransformMake(1, 0, 0, 1, 0, 0),   /* identity */
            CGAffineTransformMake(1 + 0x1p-27, 1, 1, 1 - 0x1p-27, 0, 0),
            CGAffineTransformMake(1e200, 1e200, 1e200, 1e200, 0, 0),
            CGAffineTransformMake(1e-200, 1e-200, 1e-200, 1e-200, 0, 0),
            CGAffineTransformMake(0, 0, 0, 1, 0, 0),   /* rectilinear: b == c == 0 */
            CGAffineTransformMake(1, 0, 0, 0, 0, 0),   /* rectilinear: b == c == 0 */
            CGAffineTransformMake(0, 1, 0, 1, 0, 0),   /* a == 0 but d != 0 */
            CGAffineTransformMake(1, -0.0, -0.0, 1, 5, 6),
            CGAffineTransformMake(-0.0, 1, 1, -0.0, 0, 0),
            CGAffineTransformMake(0, 0, 0, 0, 9, 9),   /* translation only */
        };
        const size_t na = sizeof ats / sizeof ats[0];
        char l[64];

        for (size_t i = 0; i < na; i++) {
            snprintf(l, sizeof l, "spi/singular/%zu", i);
            b(l, CGAffineTransformIsSingular(&ats[i]));
            snprintf(l, sizeof l, "spi/rectilinear/%zu", i);
            b(l, CGAffineTransformIsRectilinear(&ats[i]));
        }

        /* CGAffineTransformMakeWithRect.  The three branches are: both
           sides non-negative; a negative side with a finite origin; and a
           negative side with an infinite origin.  The fourth fixture pins
           that the infinite-origin collapse is only reached *with* a
           negative side -- with both sides non-negative an infinite origin
           is carried through untouched. */
        {
            static const CGRect mwr[] = {
                { { 0, 0 }, { 1, 1 } },
                { { 1, 2 }, { 3, 4 } },
                { { 5, 5 }, { -4, -4 } },          /* both sides negative */
                { { 0, 0 }, { -1, 1 } },           /* width negative only */
                { { 0, 0 }, { 1, -1 } },           /* height negative only */
                { { -0.0, -0.0 }, { 1, 1 } },
                { { 0, 0 }, { -0.0, 1 } },         /* -0.0 is not < 0 */
                { { 0, 0 }, { 1, -0.0 } },
                { { 0, 0 }, { 0x1p-1074, 1 } },    /* smallest subnormal */
                { { INFINITY, INFINITY }, { -1, -1 } },
                { { INFINITY, 0 }, { -1, 1 } },    /* only origin.x infinite */
                { { 0, INFINITY }, { 1, -1 } },    /* only origin.y infinite */
                { { INFINITY, INFINITY }, { 1, 1 } },
                { { -INFINITY, 0 }, { 1, 1 } },    /* -inf is not +inf */
                { { 0, 0 }, { 0, 0 } },
            };
            const size_t nm = sizeof mwr / sizeof mwr[0];

            for (size_t i = 0; i < nm; i++) {
                snprintf(l, sizeof l, "spi/makerec/%zu", i);
                tr(l, CGAffineTransformMakeWithRect(mwr[i]));
            }
        }

        /* CGAffineTransformDecompose_SPI, including the NULL out-parameter
           cases and the return value's 2^-46 shear threshold. */
        {
            CGAffineTransform dts[] = {
                CGAffineTransformIdentity,
                CGAffineTransformMake(2, 0, 0, 3, 0, 0),
                CGAffineTransformMake(-2, 0, 0, 3, 0, 0),  /* negative scale */
                CGAffineTransformMake(0, 0, 0, 1, 0, 0),
                CGAffineTransformMakeRotation(0.75),
                CGAffineTransformMake(1, 2, 3, 4, 5, 6),
            };
            const size_t nd = sizeof dts / sizeof dts[0];

            for (size_t i = 0; i < nd; i++) {
                CGSize sc = { 0, 0 };
                CGFloat rot = 0;
                bool neg = false;
                CGVector trv = { 0, 0 };

                snprintf(l, sizeof l, "spi/dspi/ret/%zu", i);
                b(l, CGAffineTransformDecompose_SPI(dts[i], &sc, &rot, &neg,
                    &trv));
                snprintf(l, sizeof l, "spi/dspi/scale/%zu", i); sz(l, sc);
                snprintf(l, sizeof l, "spi/dspi/rot/%zu", i); p(l, rot);
                snprintf(l, sizeof l, "spi/dspi/neg/%zu", i); b(l, neg);
                snprintf(l, sizeof l, "spi/dspi/trans/%zu", i); vec(l, trv);

                /* Every out parameter optional. */
                snprintf(l, sizeof l, "spi/dspi/allnull/%zu", i);
                b(l, CGAffineTransformDecompose_SPI(dts[i], NULL, NULL, NULL,
                    NULL));
            }
        }

        /* CGAffineTransformDecompose itself.  This used to be reachable only
           through the SPI above, which is why its payload carried an
           allowlist entry; it is public, so it is compared here directly and
           on its own.

           The cases past the first six are the corners the disassembly
           transcription in src/CGAffineTransform.c turns on, each of which
           pins a specific instruction rather than just a numeric value:

             - a zero first row, so angle is atan2(0, 0) and the raw height
               is a signed zero.  The branch tests the height's sign bit, so
               this is where a `height < 0` comparison goes wrong and
               reports a rotation a full pi away.
             - a == 0 and b == 0 with the other row set, so angle is +-pi/2
               and cos(angle) is the inexact 6.12e-17 rather than 0.  This is
               what makes the width pick a/Y over b/X.
             - exactly singular rows, where the shear guard fires and the
               shear is reported as 0 rather than a division by 0.
             - a near-singular pair, where the shear is enormous and so is
               the last bit of it.
             - negative and positive scales of equal size, whose rotations
               differ by pi and whose widths differ only in sign.
             - a translation, which is copied through unrotated. */
        {
            CGAffineTransform dcs[] = {
                CGAffineTransformIdentity,
                CGAffineTransformMake(2, 0, 0, 3, 0, 0),
                CGAffineTransformMake(-2, 0, 0, 3, 0, 0),
                CGAffineTransformMake(0, 0, 0, 1, 0, 0),
                CGAffineTransformMakeRotation(0.75),
                CGAffineTransformMake(1, 2, 3, 4, 5, 6),

                /* zero first row: signed-zero height */
                CGAffineTransformMake(0, 0, 0, 0, 0, 0),
                CGAffineTransformMake(0, 0, 1, 0, 0, 0),
                CGAffineTransformMake(0, 0, -1, 0, 0, 0),
                CGAffineTransformMake(0, 0, 0, -1, 0, 0),
                CGAffineTransformMake(0, 0, 0, 1, 0, 0),

                /* a == 0: angle is +-pi/2, cos(angle) is 6.12e-17 */
                CGAffineTransformMake(0, 1, 0, 0, 0, 0),
                CGAffineTransformMake(0, 1, 1, 1, 0, 0),
                CGAffineTransformMake(0, 1, 1, -1, 0, 0),
                CGAffineTransformMake(0, -1, 1, 1, 0, 0),
                CGAffineTransformMake(0, 1, -1, 1, 0, 0),

                /* b == 0: angle is 0 or +-pi */
                CGAffineTransformMake(1, 0, 0, 1, 0, 0),
                CGAffineTransformMake(-1, 0, 0, 1, 0, 0),
                CGAffineTransformMake(1, 0, 0, -1, 0, 0),
                CGAffineTransformMake(-1, 0, 1, 0, 0, 0),

                /* exactly singular: the shear guard fires */
                CGAffineTransformMake(1, 2, 2, 4, 0, 0),
                CGAffineTransformMake(1, -1, 1, -1, 0, 0),
                CGAffineTransformMake(0.5, 0.25, 1, 0.5, 0, 0),

                /* near-singular: huge shear */
                CGAffineTransformMake(1, 1, 1, 1 + 0x1p-52, 0, 0),
                CGAffineTransformMake(1, 1, 1, 1 - 0x1p-52, 0, 0),

                /* equal and opposite scales: rotation differs by pi */
                CGAffineTransformMake(3, 0, 0, 3, 0, 0),
                CGAffineTransformMake(-3, 0, 0, 3, 0, 0),
                CGAffineTransformMake(3, 0, 0, -3, 0, 0),
                CGAffineTransformMake(-3, 0, 0, -3, 0, 0),

                /* translation passes through unrotated */
                CGAffineTransformMake(1, 0, 0, 1, 5, 6),
                CGAffineTransformMake(0, 1, -1, 0, 5, 6),
                CGAffineTransformMake(-2, 0, 0, 3, -7, 8),
                CGAffineTransformMakeRotation(0.75),
                CGAffineTransformMakeScale(-2, 3),
            };
            const size_t ndc = sizeof dcs / sizeof dcs[0];

            for (size_t i = 0; i < ndc; i++) {
                snprintf(l, sizeof l, "decompose/all/%zu", i);
                comp(l, CGAffineTransformDecompose(dcs[i]));
            }
        }
    }

    /* Dictionary round-trips, including the key set and the CFNumber type,
       which is what the persistent representation is for. */
    for (size_t i = 0; i < np; i++) {
        char l[64];
        CFDictionaryRef d = CGPointCreateDictionaryRepresentation(points[i]);
        CGPoint back;
        CFIndex n = d ? CFDictionaryGetCount(d) : -1;
        snprintf(l, sizeof l, "pdict/count/%zu", i); printf("%s %ld\n", l, (long)n);
        memset(&back, 0xAA, sizeof back);
        snprintf(l, sizeof l, "pdict/ok/%zu", i);
        b(l, CGPointMakeWithDictionaryRepresentation(d, &back));
        snprintf(l, sizeof l, "pdict/back/%zu", i); pt(l, back);
        if (d) CFRelease(d);
    }
    for (size_t i = 0; i < ns; i++) {
        char l[64];
        CFDictionaryRef d = CGSizeCreateDictionaryRepresentation(sizes[i]);
        CGSize back;
        memset(&back, 0xAA, sizeof back);
        snprintf(l, sizeof l, "sdict/count/%zu", i);
        printf("%s %ld\n", l, d ? (long)CFDictionaryGetCount(d) : -1L);
        snprintf(l, sizeof l, "sdict/ok/%zu", i);
        b(l, CGSizeMakeWithDictionaryRepresentation(d, &back));
        snprintf(l, sizeof l, "sdict/back/%zu", i); sz(l, back);
        if (d) CFRelease(d);
    }
    for (size_t i = 0; i < nr; i++) {
        char l[64];
        CFDictionaryRef d = CGRectCreateDictionaryRepresentation(rects[i]);
        CGRect back;
        memset(&back, 0xAA, sizeof back);
        snprintf(l, sizeof l, "rdict/count/%zu", i);
        printf("%s %ld\n", l, d ? (long)CFDictionaryGetCount(d) : -1L);
        snprintf(l, sizeof l, "rdict/ok/%zu", i);
        b(l, CGRectMakeWithDictionaryRepresentation(d, &back));
        snprintf(l, sizeof l, "rdict/back/%zu", i); r(l, back);
        if (d) CFRelease(d);
    }

    /* NULL and missing-key handling on the readers. */
    {
        CGPoint pz;
        CGSize sz2;
        CGRect rz;
        CFMutableDictionaryRef partial = CFDictionaryCreateMutable(NULL, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFNumberRef five = CFNumberCreate(NULL, kCFNumberCGFloatType,
            &(CGFloat){ 5 });
        CFDictionarySetValue(partial, CFSTR("X"), five);
        CFRelease(five);

        memset(&pz, 0xAA, sizeof pz);
        b("null/pdict", CGPointMakeWithDictionaryRepresentation(NULL, &pz));
        b("null/pout", CGPointMakeWithDictionaryRepresentation(partial, NULL));
        b("null/sdict", CGSizeMakeWithDictionaryRepresentation(NULL, &sz2));
        b("null/sout", CGSizeMakeWithDictionaryRepresentation(partial, NULL));
        b("null/rdict", CGRectMakeWithDictionaryRepresentation(NULL, &rz));
        b("null/rout", CGRectMakeWithDictionaryRepresentation(partial, NULL));

        /* Only "X" present: must short-circuit and leave the rest alone. */
        memset(&pz, 0xAA, sizeof pz);
        b("partial/ok", CGPointMakeWithDictionaryRepresentation(partial, &pz));
        p("partial/x", pz.x);
        p("partial/y", pz.y);

        CFRelease(partial);
    }

    /* A float-valued CFNumber must be accepted via the kCFNumberFloatType
       fallback, which is a real behaviour of the reader. */
    {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFNumberRef fx = CFNumberCreate(NULL, kCFNumberFloatType, &(float){ 1.5f });
        CFNumberRef fy = CFNumberCreate(NULL, kCFNumberFloatType, &(float){ 2.5f });
        CGPoint back;
        CFDictionarySetValue(d, CFSTR("X"), fx);
        CFDictionarySetValue(d, CFSTR("Y"), fy);
        CFRelease(fx);
        CFRelease(fy);
        memset(&back, 0xAA, sizeof back);
        b("fallback/ok", CGPointMakeWithDictionaryRepresentation(d, &back));
        pt("fallback/back", back);
        CFRelease(d);
    }

    /* The sentinels and every predicate that depends on them.  These are the
       cases the __TEXT-only extraction could not settle, so they are pinned
       here explicitly rather than left to the general rect table.

       CGRectUprightBoundsForRotation is deliberately absent: it is a private
       external, so a test linked against Apple's framework cannot call it
       and there is nothing to compare against. */
    {
        const CGRect probes[] = {
            { { 0, 0 }, { 0, 0 } },
            { { 1, 2 }, { 3, 4 } },
            { { -1, -1 }, { 1, 1 } },
            { { 1, 1 }, { -1, -1 } },
            { { -0.0, -0.0 }, { 0.0, 0.0 } },
            { { -0.0, 1.0 }, { 0.0, 2.0 } },
            { { 0, 0 }, { 1, 0 } },
            { { 0, 0 }, { 0, 1 } },
            { { 5, 5 }, { -4, -4 } },
            { { INFINITY, INFINITY }, { 0, 0 } },
            { { INFINITY, INFINITY }, { 1, 1 } },
            { { INFINITY, 0 }, { 0, 0 } },
            { { 0, INFINITY }, { 0, 0 } },
            { { INFINITY, INFINITY }, { INFINITY, INFINITY } },
            { { -CGFLOAT_MAX, -CGFLOAT_MAX }, { CGFLOAT_MAX, CGFLOAT_MAX } },
            { { -CGFLOAT_MAX, -CGFLOAT_MAX }, { 0, 0 } },
            { { 0, 0 }, { CGFLOAT_MAX, CGFLOAT_MAX } },
            { { CGFLOAT_MAX, CGFLOAT_MAX }, { 0, 0 } },
            { { -CGFLOAT_MAX, CGFLOAT_MAX }, { 1, 1 } },
        };
        const size_t ns = sizeof probes / sizeof probes[0];
        const CGRect canon[] = { CGRectZero, CGRectNull, CGRectInfinite };
        const size_t nc = sizeof canon / sizeof canon[0];
        char l[64];

        for (size_t c = 0; c < nc; c++) {
            snprintf(l, sizeof l, "s/canon/%zu", c);
            r(l, canon[c]);
        }

        for (size_t i = 0; i < ns; i++) {
            snprintf(l, sizeof l, "s/probe/%zu", i);
            r(l, probes[i]);
            snprintf(l, sizeof l, "s/isNull/%zu", i);
            b(l, CGRectIsNull(probes[i]));
            snprintf(l, sizeof l, "s/isInf/%zu", i);
            b(l, CGRectIsInfinite(probes[i]));
            snprintf(l, sizeof l, "s/isEmpty/%zu", i);
            b(l, CGRectIsEmpty(probes[i]));
            snprintf(l, sizeof l, "s/isInt/%zu", i);
            b(l, CGRectIsIntegral(probes[i]));
            snprintf(l, sizeof l, "s/std/%zu", i);
            r(l, CGRectStandardize(probes[i]));
            snprintf(l, sizeof l, "s/inset/%zu", i);
            r(l, CGRectInset(probes[i], 1, 1));
            snprintf(l, sizeof l, "s/offset/%zu", i);
            r(l, CGRectOffset(probes[i], 1, 1));
            snprintf(l, sizeof l, "s/union/%zu", i);
            r(l, CGRectUnion(probes[i], CGRectMake(-1, -1, 1, 1)));
            snprintf(l, sizeof l, "s/intersect/%zu", i);
            r(l, CGRectIntersection(probes[i], CGRectMake(-1, -1, 1, 1)));
            snprintf(l, sizeof l, "s/eqNull/%zu", i);
            b(l, CGRectEqualToRect(probes[i], CGRectNull));
            snprintf(l, sizeof l, "s/eqInf/%zu", i);
            b(l, CGRectEqualToRect(probes[i], CGRectInfinite));
            snprintf(l, sizeof l, "eqZero/%zu", i);
            b(l, CGRectEqualToRect(probes[i], CGRectZero));
            snprintf(l, sizeof l, "containsPt/%zu", i);
            b(l, CGRectContainsPoint(probes[i], CGPointMake(0, 0)));
            snprintf(l, sizeof l, "containsRect/%zu", i);
            b(l, CGRectContainsRect(probes[i], CGRectMake(-0.25, -0.25, 0.5, 0.5)));
        }
    }

    /* CGRectDivide on degenerate and negative-size rects. */
    {
        const CGRect drects[] = {
            { { 0, 0 }, { 0, 0 } },
            { { 1, 2 }, { 3, 4 } },
            { { 5, 5 }, { -4, -4 } },
            { { INFINITY, INFINITY }, { 0, 0 } },
            { { -CGFLOAT_MAX, -CGFLOAT_MAX }, { CGFLOAT_MAX, CGFLOAT_MAX } },
        };
        const size_t nd = sizeof drects / sizeof drects[0];
        const CGFloat amts[] = { 0, 0.25, 0.5, 1, -0.5, 2 };
        const size_t na = sizeof amts / sizeof amts[0];
        char l[64];

        for (size_t i = 0; i < nd; i++) {
            for (size_t k = 0; k < na; k++) {
                for (int e = 0; e < 4; e++) {
                    CGRect slice = { { 9, 9 }, { 9, 9 } };
                    CGRect rest = { { 7, 7 }, { 7, 7 } };
                    snprintf(l, sizeof l, "div/%zu/%zu/%d", i, k, e);
                    CGRectDivide(drects[i], &slice, &rest, amts[k],
                        (CGRectEdge)e);
                    r(l, slice);
                    r(l, rest);
                }
            }
        }
    }


    /* ---- CGColorSpace: device singletons, pattern spaces, accessors,
       and the built-in name/ID table. -------------------------------------

       The spaces are compared through the same accessors as any other
       space, so a record here is Apple's answer to the same question.  The
       functions that fault in Apple (GetIdentifier, GetMD5Digest, GetNames,
       GetColorants) are declared in the extern block above and deliberately
       not called. */
    {
        static const char *const tag[] = {
            "rgb", "gray", "cmyk", "p_rgb", "p_gray", "p_cmyk", "p_null",
            /* A second pattern on each base, and a second colored pattern,
               so the pairwise matrix below covers whether equality is by
               object identity or by the base. */
            "q_rgb", "q_gray", "q_cmyk", "q_null",
        };
        const size_t nspaces = sizeof tag / sizeof tag[0];
        CGColorSpaceRef sp[sizeof tag / sizeof tag[0]];
        char l[64];

        sp[0] = CGColorSpaceCreateDeviceRGB();
        sp[1] = CGColorSpaceCreateDeviceGray();
        sp[2] = CGColorSpaceCreateDeviceCMYK();
        sp[3] = CGColorSpaceCreatePattern(sp[0]);
        sp[4] = CGColorSpaceCreatePattern(sp[1]);
        sp[5] = CGColorSpaceCreatePattern(sp[2]);
        sp[6] = CGColorSpaceCreatePattern(NULL);
        sp[7] = CGColorSpaceCreatePattern(sp[0]);
        sp[8] = CGColorSpaceCreatePattern(sp[1]);
        sp[9] = CGColorSpaceCreatePattern(sp[2]);
        sp[10] = CGColorSpaceCreatePattern(NULL);

        n("cs/typeid", CGColorSpaceGetTypeID());

        /* The device spaces are process-wide singletons, so the same space
           must come back on every call. */
        b("cs/singleton/rgb", CGColorSpaceCreateDeviceRGB() == sp[0]);
        b("cs/singleton/gray", CGColorSpaceCreateDeviceGray() == sp[1]);
        b("cs/singleton/cmyk", CGColorSpaceCreateDeviceCMYK() == sp[2]);
        /* A pattern space is a fresh object each time. */
        CGColorSpaceRef freshp = CGColorSpaceCreatePattern(sp[0]);
        b("cs/singleton/pattern_fresh", freshp != sp[3]);
        CGColorSpaceRelease(freshp);

        for (size_t i = 0; i < nspaces; i++) {
            CGColorSpaceRef s = sp[i];
            CFDataRef icc;

            snprintf(l, sizeof l, "cs/%s/model", tag[i]);
            n(l, CGColorSpaceGetModel(s));
            snprintf(l, sizeof l, "cs/%s/type", tag[i]);
            n(l, CGColorSpaceGetType(s));
            snprintf(l, sizeof l, "cs/%s/process", tag[i]);
            n(l, CGColorSpaceGetProcessColorModel(s));
            snprintf(l, sizeof l, "cs/%s/ncomp", tag[i]);
            n(l, CGColorSpaceGetNumberOfComponents(s));
            snprintf(l, sizeof l, "cs/%s/renderingIntent", tag[i]);
            n(l, CGColorSpaceGetRenderingIntent(s));
            snprintf(l, sizeof l, "cs/%s/id", tag[i]);
            n(l, CGColorSpaceGetID(s));

            /* Names.  GetName is borrowed, so a stale pointer here would
               show up as garbage rather than as a clean mismatch. */
            snprintf(l, sizeof l, "cs/%s/name", tag[i]);
            cfstr(l, CGColorSpaceGetName(s));
            snprintf(l, sizeof l, "cs/%s/copyname", tag[i]);
            CFStringRef cn = CGColorSpaceCopyName(s);
            cfstr(l, cn);
            /* Apple returns the same pointer from GetName, CopyName and two
               successive GetName calls, and the string is an immortal CFSTR
               constant.  A caller that releases the result of CopyName is
               relying on that, so both facts are checked here rather than
               assumed: pointer equality proves the constant, and the immortal
               retain count proves releasing it is a no-op. */
            snprintf(l, sizeof l, "cs/%s/name_stable", tag[i]);
            b(l, CGColorSpaceGetName(s) == CGColorSpaceGetName(s));
            snprintf(l, sizeof l, "cs/%s/copyname_same", tag[i]);
            b(l, CGColorSpaceGetName(s) == cn);
            snprintf(l, sizeof l, "cs/%s/name_immortal", tag[i]);
            n(l, cn ? (long long)CFGetRetainCount(cn) : -1);
            if (cn) CFRelease(cn);

            /* The base of a pattern space, and the empty base of a device
               space. */
            snprintf(l, sizeof l, "cs/%s/base_is_null", tag[i]);
            b(l, CGColorSpaceGetBaseColorSpace(s) == NULL);
            snprintf(l, sizeof l, "cs/%s/copybase_is_null", tag[i]);
            b(l, CGColorSpaceCopyBaseColorSpace(s) == NULL);
            snprintf(l, sizeof l, "cs/%s/base_name", tag[i]);
            CGColorSpaceRef base = CGColorSpaceCopyBaseColorSpace(s);
            cfstr(l, base ? CGColorSpaceGetName(base) : NULL);
            if (base) CGColorSpaceRelease(base);

            /* The device spaces carry no embedded profile. */
            icc = CGColorSpaceCopyICCData(s);
            snprintf(l, sizeof l, "cs/%s/icc_is_null", tag[i]);
            b(l, icc == NULL);
            snprintf(l, sizeof l, "cs/%s/icc_len", tag[i]);
            n(l, icc ? (long long)CFDataGetLength(icc) : -1);
            if (icc) CFRelease(icc);
            snprintf(l, sizeof l, "cs/%s/iccprofile_is_null", tag[i]);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            b(l, CGColorSpaceCopyICCProfile(s) == NULL);
#pragma clang diagnostic pop
            snprintf(l, sizeof l, "cs/%s/colortable_count", tag[i]);
            n(l, CGColorSpaceGetColorTableCount(s));
            {
                uint8_t table[16];
                int untouched = 1;

                for (int k = 0; k < 16; k++) table[k] = 0xAA;
                CGColorSpaceGetColorTable(s, table);
                for (int k = 0; k < 16; k++)
                    if (table[k] != 0xAA) untouched = 0;
                snprintf(l, sizeof l, "cs/%s/colortable_untouched", tag[i]);
                b(l, untouched);
            }

            /* Capability flags. */
            snprintf(l, sizeof l, "cs/%s/supportsOutput", tag[i]);
            b(l, CGColorSpaceSupportsOutput(s));
            snprintf(l, sizeof l, "cs/%s/isHDR", tag[i]);
            b(l, CGColorSpaceIsHDR(s));
            snprintf(l, sizeof l, "cs/%s/isHLG", tag[i]);
            b(l, CGColorSpaceIsHLGBased(s));
            snprintf(l, sizeof l, "cs/%s/isPQ", tag[i]);
            b(l, CGColorSpaceIsPQBased(s));
            snprintf(l, sizeof l, "cs/%s/isWideGamut", tag[i]);
            b(l, CGColorSpaceIsWideGamutRGB(s));
            snprintf(l, sizeof l, "cs/%s/usesExtended", tag[i]);
            b(l, CGColorSpaceUsesExtendedRange(s));
            snprintf(l, sizeof l, "cs/%s/uncalibrated", tag[i]);
            b(l, CGColorSpaceIsUncalibrated(s));
            snprintf(l, sizeof l, "cs/%s/iccCompatible", tag[i]);
            b(l, CGColorSpaceIsICCCompatible(s));
            snprintf(l, sizeof l, "cs/%s/psLevel2", tag[i]);
            b(l, CGColorSpaceIsPSLevel2Compatible(s));
            snprintf(l, sizeof l, "cs/%s/ignoresIntent", tag[i]);
            b(l, CGColorSpaceIgnoresIntent(s));
            snprintf(l, sizeof l, "cs/%s/itur2100TF", tag[i]);
            b(l, CGColorSpaceUsesITUR_2100TF(s));

            /* The one remaining SPI accessor that Apple can be asked about
               safely on every space here. */
            snprintf(l, sizeof l, "cs/%s/alternate_is_null", tag[i]);
            b(l, CGColorSpaceGetAlternateColorSpace(s) == NULL);
        }

        /* 2b: the calibrated gray space.  Each case is one profile, and the
           point of the section is the whole profile, not a few accessors --
           the synthesis is a template with four patched regions, and any of
           them can be wrong while every accessor still reports sensibly. */
        {
            struct {
                const char *name;
                CGFloat wp[3];
                CGFloat bp[3];      /* all zero, and a flag, means NULL */
                int has_bp;
                CGFloat gamma;
            } cases[] = {
                /* D50, no black point: the commonest call, and the one the
                   template was taken from. */
                { "d50_2.2", { 0.9505, 1.0, 1.0890 }, { 0, 0, 0 }, 0, 2.2 },
                /* Gamma alone, to show it is the tone curve and nothing
                   else that moves. */
                { "d50_1.8", { 0.9505, 1.0, 1.0890 }, { 0, 0, 0 }, 0, 1.8 },
                { "d50_1.0", { 0.9505, 1.0, 1.0890 }, { 0, 0, 0 }, 0, 1.0 },
                { "d50_2.4", { 0.9505, 1.0, 1.0890 }, { 0, 0, 0 }, 0, 2.4 },
                /* A gamma that is not representable in binary, and one that
                   is a whole number, since the u8Fixed8 quantisation is the
                   easiest part of this to get wrong. */
                { "d50_sqrt2", { 0.9505, 1.0, 1.0890 }, { 0, 0, 0 }, 0, 1.4142135623730951 },
                /* D65 against D50, and the standard-library D50, which is
                   not the 0.9505 spelling used above. */
                { "d65_2.2", { 0.95047, 1.0, 1.08883 }, { 0, 0, 0 }, 0, 2.2 },
                { "d50std_2.2", { 0.9642, 1.0, 0.8249 }, { 0, 0, 0 }, 0, 2.2 },
                /* An explicit black point, including a negative component:
                   s15Fixed16 has a sign bit and a naive cast would lose it. */
                { "d50_bp", { 0.9505, 1.0, 1.0890 }, { 0.1, 0.2, 0.3 }, 1, 2.2 },
                { "d50_bp_neg", { 0.9505, 1.0, 1.0890 }, { -0.05, 0.0, 0.12 }, 1, 1.8 },
                /* An all-zero black point passed explicitly, which must
                   agree with passing NULL. */
                { "d50_bp_zero", { 0.9505, 1.0, 1.0890 }, { 0.0, 0.0, 0.0 }, 1, 2.2 },
                /* A white point whose double scaled value falls just under a
                   half-unit boundary -- 38545.4994 -- so it quantises to
                   0x9691 in double and to 0x9692 once narrowed to float
                   first.  This is the case that pins the narrowing down; the
                   round-numbered white points above cannot tell the two apart. */
                { "wp_halfway", { 0.5881576452780182, 0.9470020567646694,
                    0.7229488766692795 }, { 0, 0, 0 }, 0, 2.2 },
            };
            size_t ncases = sizeof cases / sizeof cases[0];

            for (size_t i = 0; i < ncases; i++) {
                CGColorSpaceRef g = CGColorSpaceCreateCalibratedGray(
                    cases[i].wp, cases[i].has_bp ? cases[i].bp : NULL,
                    cases[i].gamma);
                CFDataRef d;

                snprintf(l, sizeof l, "gray/%s/non_null", cases[i].name);
                b(l, g != NULL);
                if (!g) continue;

                snprintf(l, sizeof l, "gray/%s/model", cases[i].name);
                n(l, (long long)CGColorSpaceGetModel(g));
                snprintf(l, sizeof l, "gray/%s/ncomp", cases[i].name);
                n(l, (long long)CGColorSpaceGetNumberOfComponents(g));
                snprintf(l, sizeof l, "gray/%s/base_is_null", cases[i].name);
                b(l, CGColorSpaceGetBaseColorSpace(g) == NULL);

                /* The whole profile. */
                d = CGColorSpaceCopyICCData(g);
                snprintf(l, sizeof l, "gray/%s/profile", cases[i].name);
                icc(l, d);
                if (d) CFRelease(d);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                CFDataRef dp = CGColorSpaceCopyICCProfile(g);
                snprintf(l, sizeof l, "gray/%s/profile_alias", cases[i].name);
                icc(l, dp);
                if (dp) CFRelease(dp);
#pragma clang diagnostic pop

                /* The copy must be the caller's own: writing through it may
                   not disturb the space, which is only true if CopyICCData
                   copied rather than handing back the stored bytes. */
                {
                    CFDataRef c1 = CGColorSpaceCopyICCData(g);
                    CFDataRef c2 = CGColorSpaceCopyICCData(g);
                    snprintf(l, sizeof l, "gray/%s/copy_is_fresh", cases[i].name);
                    b(l, c1 && c2 && CFDataGetBytePtr(c1) != CFDataGetBytePtr(c2));
                    if (c1) CFRelease(c1);
                    if (c2) CFRelease(c2);
                }

                /* Two spaces built from identical arguments are equal, and
                   two built from different ones are not -- the profile is
                   not baked into identity, so two equal profiles still have
                   to compare equal. */
                {
                    CGColorSpaceRef g2 = CGColorSpaceCreateCalibratedGray(
                        cases[i].wp, cases[i].has_bp ? cases[i].bp : NULL,
                        cases[i].gamma);
                    snprintf(l, sizeof l, "gray/%s/self_eq", cases[i].name);
                    b(l, CGColorSpaceEqualToColorSpace(g, g2));
                    snprintf(l, sizeof l, "gray/%s/clone_eq", cases[i].name);
                    b(l, CGColorSpaceEqualToColorSpace(g, g2));
                    if (g2) CGColorSpaceRelease(g2);
                }

                snprintf(l, sizeof l, "gray/%s/uncalibrated", cases[i].name);
                b(l, CGColorSpaceIsUncalibrated(g));
                snprintf(l, sizeof l, "gray/%s/iccCompatible", cases[i].name);
                b(l, CGColorSpaceIsICCCompatible(g));
                snprintf(l, sizeof l, "gray/%s/renderingIntent", cases[i].name);
                n(l, (long long)CGColorSpaceGetRenderingIntent(g));
                snprintf(l, sizeof l, "gray/%s/psLevel2", cases[i].name);
                b(l, CGColorSpaceIsPSLevel2Compatible(g));
                CGColorSpaceRelease(g);
            }

            /* Distinct arguments must not collapse to one profile. */
            for (size_t i = 0; i + 1 < ncases; i++) {
                for (size_t k = i + 1; k < ncases; k++) {
                    CGColorSpaceRef gi = CGColorSpaceCreateCalibratedGray(
                        cases[i].wp, cases[i].has_bp ? cases[i].bp : NULL,
                        cases[i].gamma);
                    CGColorSpaceRef gk = CGColorSpaceCreateCalibratedGray(
                        cases[k].wp, cases[k].has_bp ? cases[k].bp : NULL,
                        cases[k].gamma);
                    snprintf(l, sizeof l, "gray/distinct/%zu/%zu", i, k);
                    b(l, CGColorSpaceEqualToColorSpace(gi, gk));
                    if (gi) CGColorSpaceRelease(gi);
                    if (gk) CGColorSpaceRelease(gk);
                }
            }

            /* A calibrated space is not a device space, and the two must not
               be confusable in either direction. */
            {
                CGColorSpaceRef dev = CGColorSpaceCreateDeviceGray();
                CGColorSpaceRef cal = CGColorSpaceCreateCalibratedGray(
                    (CGFloat[]){ 0.9505, 1.0, 1.0890 }, NULL, 2.2);
                snprintf(l, sizeof l, "gray/vs_device_eq");
                b(l, CGColorSpaceEqualToColorSpace(cal, dev));
                snprintf(l, sizeof l, "gray/vs_device_rev_eq");
                b(l, CGColorSpaceEqualToColorSpace(dev, cal));
                /* The device gray has no profile and the calibrated one has
                   380 bytes, so the two differ in the one way that matters
                   to a caller who never asks for the profile. */
                snprintf(l, sizeof l, "gray/dev_icc_is_null");
                b(l, CGColorSpaceCopyICCData(dev) == NULL);
                snprintf(l, sizeof l, "gray/cal_model");
                n(l, (long long)CGColorSpaceGetModel(cal));
                snprintf(l, sizeof l, "gray/dev_model");
                n(l, (long long)CGColorSpaceGetModel(dev));
                if (cal) CGColorSpaceRelease(cal);
                CGColorSpaceRelease(dev);
            }
        }

        /* Calibrated RGB.  The cases are chosen to hit the four ways the
           profile length and the tag table can differ: three distinct tone
           curves, two curves sharing one block, one curve shared three ways,
           and the white point quantising to zero so that every colorant
           aliases it. */
        {
            static const struct {
                const char *name;
                CGFloat wp[3], bp[3], gamma[3], m[9];
                int has_bp;
            } cases[] = {
            /* The identity matrix on (1,1,1): the profile the reference
               template above was taken from, and all three curves equal. */
            { "unity", { 1.0, 1.0, 1.0 }, { 0, 0, 0 }, { 2.2, 2.2, 2.2 },
              { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 0 },
            /* D50, three distinct curves: the longest profile, 528 bytes. */
            { "d50_distinct", { 0.9505, 1.0, 1.0890 }, { 0, 0, 0 },
              { 2.2, 1.0, 0.5 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 0 },
            /* D50, two equal curves: 512 bytes. */
            { "d50_partial", { 0.9505, 1.0, 1.0890 }, { 0, 0, 0 },
              { 2.2, 2.2, 1.0 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 0 },
            /* An explicit black point, which must alias wtpt when it is zero
               and must not when it is not. */
            { "d50_bp_zero", { 0.9505, 1.0, 1.0890 }, { 0.0, 0.0, 0.0 },
              { 2.2, 2.2, 2.2 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 1 },
            { "d50_bp", { 0.9505, 1.0, 1.0890 }, { 0.1, 0.2, 0.3 },
              { 2.2, 2.2, 2.2 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 1 },
            /* A zero white point: the colorants divide by zero and land on
               zero, collapsing five XYZ tags onto one block. */
            { "wp_zero", { 0.0, 0.0, 0.0 }, { 0, 0, 0 }, { 2.2, 2.2, 2.2 },
              { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 0 },
            /* A non-identity matrix, so the colorant dot products are doing
               real work rather than passing an identity through. */
            { "d50_nonsym", { 0.9505, 1.0, 1.0890 }, { 0, 0, 0 },
              { 1.8, 1.8, 1.8 },
              { 0.4360, 0.3851, 0.1431, 0.2225, 0.7169, 0.0606,
                0.0139, 0.0971, 0.7141 }, 0 },
            /* A matrix with negative and off-diagonal terms, which pushes the
               adapted matrix outside the gamut and exercises the Y clamp
               against the saturated X and Z. */
            { "d50_signed", { 0.9505, 1.0, 1.0890 }, { 0, 0, 0 },
              { 2.2, 2.2, 2.2 },
              { -0.4, 1.2, 0.2, 0.1, -0.3, 1.2, 0.3, 0.4, 0.1 }, 0 },
            /* D65, which is the common real case. */
            { "d65", { 0.95047, 1.0, 1.08883 }, { 0, 0, 0 },
              { 2.2, 2.2, 2.2 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 0 },
            /* The standard-library D50, whose 0.9642 spelling differs from the
               0.9505 one above and therefore gives different colorants. */
            { "d50std", { 0.9642, 1.0, 0.8249 }, { 0, 0, 0 },
              { 1.0, 1.0, 1.0 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 0 },
            /* Gamma 0 quantises to zero and must agree with the clamp in the
               tone curve, not saturate. */
            { "gamma_zero", { 0.9505, 1.0, 1.0890 }, { 0, 0, 0 },
              { 0.0, 0.0, 2.2 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 0 },
            /* Two cases whose colorants differ in their last quantisation unit
               depending only on whether the caller's white point and matrix
               are narrowed to float before the adaptation.  Both were found by
               search, not by inspection: `f32_wp' ends one unit higher on bXYZ
               Z with a double pipeline, `f32_mat' one unit lower on bXYZ Y.
               Every other case above is round-numbered enough that both
               pipelines agree on it, so without these two the harness would
               not notice the narrowing being dropped at all. */
            { "f32_wp", { 0.5881576452780182, 0.9470020567646694,
                0.7229488766692795 }, { 0, 0, 0 }, { 2.2, 2.2, 2.2 },
              { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 0 },
            { "f32_mat", { 0.7870202081887864, 0.6530591360169214,
                0.9163712302723298 }, { 0, 0, 0 }, { 2.2, 2.2, 2.2 },
              { 0.43490125721413203, 1.1113834036643453, 0.6716735016645925,
                0.9337600377690622, 0.7232386798674622, 0.8723825768682059,
                0.39371172726300574, 0.25797279625686687, 1.496284064957866 },
              0 },
            };
            size_t ncases = sizeof cases / sizeof cases[0];

            for (size_t i = 0; i < ncases; i++) {
                CGColorSpaceRef c = CGColorSpaceCreateCalibratedRGB(
                    cases[i].wp, cases[i].has_bp ? cases[i].bp : NULL,
                    cases[i].gamma, cases[i].m);
                CFDataRef d;

                snprintf(l, sizeof l, "rgb/%s/non_null", cases[i].name);
                b(l, c != NULL);
                if (!c) continue;

                snprintf(l, sizeof l, "rgb/%s/model", cases[i].name);
                n(l, (long long)CGColorSpaceGetModel(c));
                snprintf(l, sizeof l, "rgb/%s/ncomp", cases[i].name);
                n(l, (long long)CGColorSpaceGetNumberOfComponents(c));
                snprintf(l, sizeof l, "rgb/%s/base_is_null", cases[i].name);
                b(l, CGColorSpaceGetBaseColorSpace(c) == NULL);

                /* The whole profile, which is what pins the layout, the
                   sharing and the ID at once. */
                d = CGColorSpaceCopyICCData(c);
                snprintf(l, sizeof l, "rgb/%s/profile", cases[i].name);
                icc(l, d);
                if (d) CFRelease(d);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                CFDataRef dp = CGColorSpaceCopyICCProfile(c);
                snprintf(l, sizeof l, "rgb/%s/profile_alias", cases[i].name);
                icc(l, dp);
                if (dp) CFRelease(dp);
#pragma clang diagnostic pop

                {
                    CFDataRef c1 = CGColorSpaceCopyICCData(c);
                    CFDataRef c2 = CGColorSpaceCopyICCData(c);
                    snprintf(l, sizeof l, "rgb/%s/copy_is_fresh",
                        cases[i].name);
                    b(l, c1 && c2 && CFDataGetBytePtr(c1) != CFDataGetBytePtr(c2));
                    if (c1) CFRelease(c1);
                    if (c2) CFRelease(c2);
                }

                {
                    CGColorSpaceRef c2 = CGColorSpaceCreateCalibratedRGB(
                        cases[i].wp, cases[i].has_bp ? cases[i].bp : NULL,
                        cases[i].gamma, cases[i].m);
                    snprintf(l, sizeof l, "rgb/%s/self_eq", cases[i].name);
                    b(l, CGColorSpaceEqualToColorSpace(c, c2));
                    if (c2) CGColorSpaceRelease(c2);
                }

                snprintf(l, sizeof l, "rgb/%s/uncalibrated", cases[i].name);
                b(l, CGColorSpaceIsUncalibrated(c));
                snprintf(l, sizeof l, "rgb/%s/iccCompatible", cases[i].name);
                b(l, CGColorSpaceIsICCCompatible(c));
                snprintf(l, sizeof l, "rgb/%s/renderingIntent",
                    cases[i].name);
                n(l, (long long)CGColorSpaceGetRenderingIntent(c));
                snprintf(l, sizeof l, "rgb/%s/psLevel2", cases[i].name);
                b(l, CGColorSpaceIsPSLevel2Compatible(c));
                CGColorSpaceRelease(c);
            }

            /* Distinct arguments must not collapse to one profile. */
            for (size_t i = 0; i + 1 < ncases; i++) {
                for (size_t k = i + 1; k < ncases; k++) {
                    CGColorSpaceRef ci = CGColorSpaceCreateCalibratedRGB(
                        cases[i].wp, cases[i].has_bp ? cases[i].bp : NULL,
                        cases[i].gamma, cases[i].m);
                    CGColorSpaceRef ck = CGColorSpaceCreateCalibratedRGB(
                        cases[k].wp, cases[k].has_bp ? cases[k].bp : NULL,
                        cases[k].gamma, cases[k].m);
                    snprintf(l, sizeof l, "rgb/distinct/%zu/%zu", i, k);
                    b(l, CGColorSpaceEqualToColorSpace(ci, ck));
                    if (ci) CGColorSpaceRelease(ci);
                    if (ck) CGColorSpaceRelease(ck);
                }
            }

            /* The defaulted arguments: a NULL black point is zero, NULL
               gamma is 1.0 and NULL matrix is the identity, and each default
               has to agree with passing that value explicitly.  This is
               worth testing on its own because it is the only thing that
               distinguishes the caller's three pointers from the profile. */
            {
                static const CGFloat unity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
                CGFloat unit_g[3] = { 1.0, 1.0, 1.0 };
                CGFloat zero_b[3] = { 0.0, 0.0, 0.0 };
                CGFloat wp[3] = { 0.9505, 1.0, 1.0890 };
                CGColorSpaceRef dflt = CGColorSpaceCreateCalibratedRGB(
                    wp, NULL, NULL, NULL);
                CGColorSpaceRef expl = CGColorSpaceCreateCalibratedRGB(
                    wp, zero_b, unit_g, unity);

                snprintf(l, sizeof l, "rgb/defaults/non_null");
                b(l, dflt != NULL);
                snprintf(l, sizeof l, "rgb/defaults/agree");
                b(l, dflt && CGColorSpaceEqualToColorSpace(dflt, expl));
                if (dflt) CGColorSpaceRelease(dflt);
                if (expl) CGColorSpaceRelease(expl);
            }

            /* A calibrated RGB space is not a device RGB space, and the two
               must not be confusable in either direction. */
            {
                CGColorSpaceRef dev = CGColorSpaceCreateDeviceRGB();
                CGColorSpaceRef cal = CGColorSpaceCreateCalibratedRGB(
                    (CGFloat[]){ 0.9505, 1.0, 1.0890 }, NULL,
                    (CGFloat[]){ 2.2, 2.2, 2.2 },
                    (CGFloat[]){ 1, 0, 0, 0, 1, 0, 0, 0, 1 });
                snprintf(l, sizeof l, "rgb/vs_device_eq");
                b(l, CGColorSpaceEqualToColorSpace(cal, dev));
                snprintf(l, sizeof l, "rgb/vs_device_rev_eq");
                b(l, CGColorSpaceEqualToColorSpace(dev, cal));
                snprintf(l, sizeof l, "rgb/dev_icc_is_null");
                b(l, CGColorSpaceCopyICCData(dev) == NULL);
                if (cal) CGColorSpaceRelease(cal);
                CGColorSpaceRelease(dev);
            }
        }

        /* Lab.  This one has three rules the calibrated families above do
           not have, and each has to be pinned by the whole profile:

             - a white or black point survives only if a float can hold each
               of its three coordinates exactly, and the two tags are gated
               independently, so a rejected tag is stored as three zeros
               while the other is still kept;
             - because a rejected tag is indistinguishable from a tag the
               caller passed as zero, the profile is 496 bytes when both
               points end up zero -- which is also exactly the generic Lab
               profile -- and 516 when they differ, since the two XYZ tags
               can no longer share one block;
             - the creation date is live, so these profiles are compared with
               the date masked.

           The cases are chosen so each rule has at least one case that would
           fail if the rule were dropped. */
        {
            static const struct {
                const char *name;
                CGFloat wp[3], bp[3], range[4];
                int has_bp, has_range;
            } cases[] = {
            /* (1,1,1) on both: every coordinate is float-exact, so the two
               tags differ and the profile is the long one. */
            { "unity", { 1.0, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            /* A NULL black point is zero, which is what the case above has to
               agree with, since that is the one thing that distinguishes the
               caller's three pointers from the profile. */
            { "unity_no_bp", { 1.0, 1.0, 1.0 }, { 0, 0, 0 },
              { 0, 0, 0, 0 }, 0, 0 },
            /* D65 and D50 both fail the float test, so each collapses onto
               the generic Lab profile -- the common real-world case, and the
               one a caller is most likely to be surprised by. */
            { "d65", { 0.95047, 1.0, 1.08883 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            { "d50std", { 0.9642, 1.0, 0.8249 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            /* A white point of literal zeros PASSES the float test -- zero is
               exactly representable -- and so does not collapse the profile:
               it stores zeros and shares a block with the default black
               point, which is the same 496 bytes as the generic profile for
               a different reason.  This is the case that separates "fails the
               test" from "is zero". */
            { "wp_zero", { 0.0, 0.0, 0.0 }, { 0, 0, 0 },
              { 0, 0, 0, 0 }, 0, 0 },
            /* The independent gates: white point rejected, black point kept,
               and the reverse.  Each is 516 bytes and neither is the generic
               profile, so dropping the "independently" would show up here. */
            { "wp_bad_bp_ok", { 0.5, 0.3, 0.125 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            { "wp_ok_bp_bad", { 1.0, 1.0, 1.0 }, { 0.5, 0.3, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            { "both_bad", { 0.5, 0.3, 0.125 }, { 0.5, 0.7, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            /* (float)0.3 survives while a double 0.3 does not, because the
               narrow value is exactly representable.  Without this pair the
               harness could not tell "compares against float" from "compares
               against double". */
            { "f32_wp", { (CGFloat)(float)0.3, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            { "f64_wp", { 0.3, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            /* NaN fails the gate.  Infinities pass it, because a float holds
               infinity exactly, and then saturate in the 16.16 encoding --
               so this case pins the clamp from both sides. */
            { "wp_nan", { 0.0 / 0.0, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            { "wp_inf", { 1.0, 1.0 / 0.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            { "wp_neginf", { 1.0, 1.0, -1.0 / 0.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            { "wp_big", { 40000.0, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            { "wp_negbig", { -40000.0, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            /* The rounding asymmetry: +0.5 and -0.5 quantise to different
               magnitudes, because the scale rounds before it truncates. */
            { "wp_half", { 0.5, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            { "wp_neghalf", { -0.5, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            { "wp_lsb", { 1.0 / 65536.0, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0, 0, 0, 0 }, 1, 0 },
            /* The range argument is accepted and ignored outright, so all
               four of these have to produce the same profile as unity. */
            { "range_ones", { 1.0, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 1.0, 1.0, 1.0, 1.0 }, 1, 1 },
            { "range_signed", { 1.0, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { -0.5, 0.5, -0.5, 0.5 }, 1, 1 },
            { "range_huge", { 1.0, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 1e30, -1e30, 1e30, -1e30 }, 1, 1 },
            { "range_nan", { 1.0, 1.0, 1.0 }, { 0.5, 0.25, 0.125 },
              { 0.0 / 0.0, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0 }, 1, 1 },
            };
            size_t ncases = sizeof cases / sizeof cases[0];
            static const CGFloat unity_wp[3] = { 1.0, 1.0, 1.0 };
            static const CGFloat unity_bp[3] = { 0.5, 0.25, 0.125 };
            static const CGFloat zero4[4] = { 0.0, 0.0, 0.0, 0.0 };

            for (size_t i = 0; i < ncases; i++) {
                CGColorSpaceRef c = CGColorSpaceCreateLab(
                    cases[i].wp, cases[i].has_bp ? cases[i].bp : NULL,
                    cases[i].has_range ? cases[i].range : NULL);
                CFDataRef d;

                snprintf(l, sizeof l, "lab/%s/non_null", cases[i].name);
                b(l, c != NULL);
                if (!c) continue;

                snprintf(l, sizeof l, "lab/%s/model", cases[i].name);
                n(l, (long long)CGColorSpaceGetModel(c));
                snprintf(l, sizeof l, "lab/%s/ncomp", cases[i].name);
                n(l, (long long)CGColorSpaceGetNumberOfComponents(c));
                snprintf(l, sizeof l, "lab/%s/base_is_null", cases[i].name);
                b(l, CGColorSpaceGetBaseColorSpace(c) == NULL);
                snprintf(l, sizeof l, "lab/%s/name_is_null", cases[i].name);
                b(l, CGColorSpaceCopyName(c) == NULL);
                snprintf(l, sizeof l, "lab/%s/uncalibrated", cases[i].name);
                b(l, CGColorSpaceIsUncalibrated(c));
                snprintf(l, sizeof l, "lab/%s/iccCompatible", cases[i].name);
                b(l, CGColorSpaceIsICCCompatible(c));
                snprintf(l, sizeof l, "lab/%s/renderingIntent", cases[i].name);
                n(l, (long long)CGColorSpaceGetRenderingIntent(c));

                /* The whole profile with the live date masked.  This is the
                   check that pins the tag table, the sharing and the
                   quantisation at once. */
                d = CGColorSpaceCopyICCData(c);
                snprintf(l, sizeof l, "lab/%s/profile", cases[i].name);
                icc_live_date(l, d);
                if (d) CFRelease(d);

                {
                    CFDataRef c1 = CGColorSpaceCopyICCData(c);
                    CFDataRef c2 = CGColorSpaceCopyICCData(c);
                    snprintf(l, sizeof l, "lab/%s/copy_is_fresh", cases[i].name);
                    b(l, c1 && c2 && CFDataGetBytePtr(c1) != CFDataGetBytePtr(c2));
                    if (c1) CFRelease(c1);
                    if (c2) CFRelease(c2);
                }

                /* Equality is on the profile, not on the arguments, so two
                   calls that agree byte for byte have to compare equal even
                   though the arguments differ. */
                {
                    CGColorSpaceRef c2 = CGColorSpaceCreateLab(
                        cases[i].wp, cases[i].has_bp ? cases[i].bp : NULL,
                        cases[i].has_range ? cases[i].range : NULL);
                    snprintf(l, sizeof l, "lab/%s/self_eq", cases[i].name);
                    b(l, CGColorSpaceEqualToColorSpace(c, c2));
                    if (c2) CGColorSpaceRelease(c2);
                }

                CGColorSpaceRelease(c);
            }

            /* The collapsed cases are all the same 496 bytes: a rejected tag
               is indistinguishable from a tag the caller passed as zero, so
               every case whose two points both end up zero has to be equal to
               every other one, whatever its arguments were.  This is checked
               through the public surface -- equality plus the digest -- rather
               than against CGColorSpaceCreateWithName(kCGColorSpaceGenericLab),
               which Apple resolves to those same bytes but which is outside
               this project's implemented surface. */
            {
                static const CGFloat d65[3] = { 0.95047, 1.0, 1.08883 };
                static const CGFloat d50[3] = { 0.9642, 1.0, 0.8249 };
                static const CGFloat wp_zero[3] = { 0.0, 0.0, 0.0 };
                static const CGFloat wp_bad[3] = { 0.5, 0.3, 0.125 };
                static const CGFloat wp_inf[3] = { 1.0, 1.0 / 0.0, 1.0 };
                static const CGFloat bp_bad[3] = { 0.5, 0.7, 0.125 };
                static const CGFloat *const collapse[][3] = {
                    { unity_wp, unity_bp, NULL },
                    { d65, unity_bp, NULL },
                    { d50, unity_bp, NULL },
                    { wp_zero, unity_bp, NULL },
                    { wp_bad, bp_bad, NULL },
                    { wp_inf, unity_bp, NULL },
                };
                static const char *const cnames[] = {
                    "unity_wp", "d65", "d50", "wp_zero", "wp_bad", "wp_inf",
                };
                const size_t nc = sizeof collapse / sizeof collapse[0];
                CGColorSpaceRef ref = CGColorSpaceCreateLab(collapse[0][0],
                    collapse[0][1], collapse[0][2]);

                for (size_t i = 0; i < nc; i++) {
                    CGColorSpaceRef c = CGColorSpaceCreateLab(collapse[i][0],
                        collapse[i][1], collapse[i][2]);
                    CFDataRef dr = ref ? CGColorSpaceCopyICCData(ref) : NULL;
                    CFDataRef dc = c ? CGColorSpaceCopyICCData(c) : NULL;
                    int same = 0;

                    if (dr && dc) {
                        CFIndex m = CFDataGetLength(dr);

                        same = 1;
                        if (m != CFDataGetLength(dc)) {
                            same = 0;
                        } else {
                            const unsigned char *a = CFDataGetBytePtr(dr);
                            const unsigned char *bb = CFDataGetBytePtr(dc);

                            /* Bytes 24-35 are the creation date, which is
                               live here; everything else must match. */
                            for (CFIndex k = 0; k < m; k++) {
                                if (k >= 24 && k < 36) continue;
                                if (a[k] != bb[k]) { same = 0; break; }
                            }
                        }
                    }

                    snprintf(l, sizeof l, "lab/collapse/%s/eq_ref", cnames[i]);
                    b(l, c && CGColorSpaceEqualToColorSpace(c, ref));
                    /* And the same bytes, not merely an equal space:
                       equality could be decided on the arguments while the
                       profiles differed, and these cases disagree about the
                       arguments on purpose. */
                    snprintf(l, sizeof l, "lab/collapse/%s/same_bytes", cnames[i]);
                    b(l, same);
                    if (dr) CFRelease(dr);
                    if (dc) CFRelease(dc);
                    if (c) CGColorSpaceRelease(c);
                }
                if (ref) CGColorSpaceRelease(ref);
            }

            /* The defaulting rules: NULL black point is zero, and a range of
               all zeros is the same as no range at all. */
            {
                CGColorSpaceRef dflt = CGColorSpaceCreateLab(unity_wp, NULL, NULL);
                CGColorSpaceRef expl = CGColorSpaceCreateLab(unity_wp, unity_bp,
                    NULL);

                snprintf(l, sizeof l, "lab/defaults/non_null");
                b(l, dflt != NULL);
                /* The explicit black point above is not zero, so these two
                   must differ; the zero-range half is checked by comparing
                   two spaces built the same way with and without it. */
                snprintf(l, sizeof l, "lab/defaults/bp_matters");
                b(l, !(dflt && CGColorSpaceEqualToColorSpace(dflt, expl)));
                if (dflt) CGColorSpaceRelease(dflt);
                if (expl) CGColorSpaceRelease(expl);
            }
            {
                CGColorSpaceRef nr = CGColorSpaceCreateLab(unity_wp, unity_bp, NULL);
                CGColorSpaceRef zr = CGColorSpaceCreateLab(unity_wp, unity_bp, zero4);

                snprintf(l, sizeof l, "lab/defaults/range_ignored");
                b(l, nr && CGColorSpaceEqualToColorSpace(nr, zr));
                if (nr) CGColorSpaceRelease(nr);
                if (zr) CGColorSpaceRelease(zr);
            }

            /* A NULL white point is deliberately NOT exercised here.  Apple
               faults on it, so the oracle would die before printing anything
               and there is no answer to compare against; the header marks the
               argument non-null and our implementation returns NULL instead. */

            /* A sweep over dyadic white points, which are all float-exact, so
               every case here exercises the 16.16 encoding across its range
               rather than the float gate.  Anything the quantiser rounds,
               truncates or saturates differently shows up as a length or a
               digest mismatch rather than as a plausible-looking profile. */
            for (int i = -80; i <= 80; i++) {
                CGFloat wp[3] = { (CGFloat)i / 64.0, 1.0, 1.0 };
                CGColorSpaceRef c = CGColorSpaceCreateLab(wp, unity_bp, NULL);
                CFDataRef d;

                snprintf(l, sizeof l, "lab/sweep/dyadic_%d", i);
                d = c ? CGColorSpaceCopyICCData(c) : NULL;
                icc_live_date(l, d);
                if (d) CFRelease(d);
                if (c) CGColorSpaceRelease(c);
            }

            /* And a sweep over points that are mostly NOT float-exact, so the
               gate is exercised across the whole set rather than at a handful
               of hand-picked values. */
            for (int i = 0; i < 64; i++) {
                CGFloat wp[3], bp[3];
                CGColorSpaceRef c;
                CFDataRef d;

                for (int k = 0; k < 3; k++) {
                    wp[k] = (CGFloat)(0.1 * (double)(i + k * 17));
                    bp[k] = (CGFloat)(0.5 * (double)(i - k * 7));
                }
                c = CGColorSpaceCreateLab(wp, bp, NULL);
                snprintf(l, sizeof l, "lab/sweep/inexact_%d", i);
                d = c ? CGColorSpaceCopyICCData(c) : NULL;
                icc_live_date(l, d);
                if (d) CFRelease(d);
                if (c) CGColorSpaceRelease(c);
            }
        }

        /* Linearized and extended spaces.

           These are a different kind of step from the calibrated ones.  The
           profile is not a template with a few fields filled in; it is
           assembled out of the base profile's own tags, so the cases that
           matter are the ones that decide which tags are carried over and
           which are dropped, plus the two places where the result stops
           being a function of its arguments at all.

           The first is the white point and the colorants, which are copied
           verbatim rather than recomputed, and the tone curve and black point
           and copyright, which are discarded.  The second is the
           description, which is built by appending to the base's, so that
           linearizing an already-linearized space is not the identity.

           No case here masks the creation date: unlike Lab, every profile in
           this family stamps a constant one.  That is worth pinning down, so
           the profile cases use the unmasked comparison. */
        {
            static const CGFloat d50[3] = { 0.9505, 1.0, 1.0890 };
            static const CGFloat d65[3] = { 0.95047, 1.0, 1.08883 };
            static const CGFloat unity[3] = { 1.0, 1.0, 1.0 };
            static const CGFloat zero[3] = { 0.0, 0.0, 0.0 };
            static const CGFloat odd[3] = { 0.2, 0.4, 0.6 };
            static const CGFloat g222[3] = { 2.2, 2.2, 2.2 };
            static const CGFloat g321[3] = { 1.8, 2.2, 1.0 };
            static const CGFloat idm[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
            static const CGFloat p3m[9] = {
                0.4866, 0.2657, 0.1982,
                0.2289, 0.6917, 0.0794,
                0.0000, 0.0451, 1.0439,
            };
            /* Bases to build from.  Two sets of five: one with a default
               black point and one with an explicit one, because the linear
               profile is supposed to drop the black point either way, and an
               implementation that carried it over would only show up on the
               second set.  `gscal' is the per-channel gamma for the RGB
               bases; gray takes a single gamma. */
            static const CGFloat *const wps[] = { d50, d65, unity, zero, odd };
            static const CGFloat *const matrices[] = { idm, p3m };
            static const CGFloat gsc[2] = { 2.2, 1.8 };
            static const CGFloat *const rgammas[] = { g222, g321 };
            CGColorSpaceRef graybase[5], rgbbase[5], graybp[5], rgbbp[5];
            size_t i, k;

            for (i = 0; i < 5; i++) {
                graybase[i] = CGColorSpaceCreateCalibratedGray(wps[i], NULL,
                    gsc[i % 2]);
                rgbbase[i] = CGColorSpaceCreateCalibratedRGB(wps[i], NULL,
                    rgammas[i % 2], matrices[i % 2]);
                graybp[i] = CGColorSpaceCreateCalibratedGray(wps[i], odd,
                    gsc[i % 2]);
                rgbbp[i] = CGColorSpaceCreateCalibratedRGB(wps[i], odd,
                    rgammas[i % 2], matrices[i % 2]);
            }

            /* Every base, through all three entry points. */
            for (i = 0; i < 5; i++) {
                CGColorSpaceRef base = graybase[i];
                CGColorSpaceRef rgbb = rgbbase[i];

                for (int which = 0; which < 2; which++) {
                    const char *kind = which ? "rgb" : "gray";
                    CGColorSpaceRef src = which ? rgbb : base;
                    CGColorSpaceRef lin = CGColorSpaceCreateLinearized(src);
                    CGColorSpaceRef ext = CGColorSpaceCreateExtended(src);
                    CGColorSpaceRef xlin =
                        CGColorSpaceCreateExtendedLinearized(src);
                    CFDataRef d;

                    snprintf(l, sizeof l, "lin/%s/%zu/non_null", kind, i);
                    b(l, lin != NULL);
                    if (lin) {
                        snprintf(l, sizeof l, "lin/%s/%zu/model", kind, i);
                        n(l, (long long)CGColorSpaceGetModel(lin));
                        snprintf(l, sizeof l, "lin/%s/%zu/ncomp", kind, i);
                        n(l, (long long)CGColorSpaceGetNumberOfComponents(lin));
                        snprintf(l, sizeof l, "lin/%s/%zu/base_is_null", kind, i);
                        b(l, CGColorSpaceGetBaseColorSpace(lin) == NULL);
                        snprintf(l, sizeof l, "lin/%s/%zu/name_is_null", kind, i);
                        b(l, CGColorSpaceCopyName(lin) == NULL);
                        /* The whole profile, unmasked: the date is constant
                           in this family, so nothing needs masking and a
                           live date would be caught. */
                        d = CGColorSpaceCopyICCData(lin);
                        snprintf(l, sizeof l, "lin/%s/%zu/profile", kind, i);
                        icc(l, d);
                        if (d) CFRelease(d);
                        /* Not extended, but wide if it has three
                           components. */
                        snprintf(l, sizeof l, "lin/%s/%zu/ext_range", kind, i);
                        b(l, CGColorSpaceUsesExtendedRange(lin));
                        snprintf(l, sizeof l, "lin/%s/%zu/wide", kind, i);
                        b(l, CGColorSpaceIsWideGamutRGB(lin));
                        snprintf(l, sizeof l, "lin/%s/%zu/ps2", kind, i);
                        b(l, CGColorSpaceIsPSLevel2Compatible(lin));
                        snprintf(l, sizeof l, "lin/%s/%zu/icc", kind, i);
                        b(l, CGColorSpaceIsICCCompatible(lin));
                    }
                    if (ext) {
                        d = CGColorSpaceCopyICCData(ext);
                        snprintf(l, sizeof l, "ext/%s/%zu/profile", kind, i);
                        icc(l, d);
                        if (d) CFRelease(d);
                        /* Extended, and wide only for RGB. */
                        snprintf(l, sizeof l, "ext/%s/%zu/ext_range", kind, i);
                        b(l, CGColorSpaceUsesExtendedRange(ext));
                        snprintf(l, sizeof l, "ext/%s/%zu/wide", kind, i);
                        b(l, CGColorSpaceIsWideGamutRGB(ext));
                        /* An extended space's profile is its base's byte for
                           byte.  Equality has to disagree even so. */
                        snprintf(l, sizeof l, "ext/%s/%zu/eq_base", kind, i);
                        b(l, CGColorSpaceEqualToColorSpace(ext, src));
                    }
                    if (xlin) {
                        d = CGColorSpaceCopyICCData(xlin);
                        snprintf(l, sizeof l, "xlin/%s/%zu/profile", kind, i);
                        icc(l, d);
                        if (d) CFRelease(d);
                        snprintf(l, sizeof l, "xlin/%s/%zu/ext_range", kind, i);
                        b(l, CGColorSpaceUsesExtendedRange(xlin));
                        /* Identical profile to the linearized one, and still
                           a different space. */
                        snprintf(l, sizeof l, "xlin/%s/%zu/eq_lin", kind, i);
                        b(l, CGColorSpaceEqualToColorSpace(xlin, lin));
                    }
                    if (lin) CGColorSpaceRelease(lin);
                    if (ext) CGColorSpaceRelease(ext);
                    if (xlin) CGColorSpaceRelease(xlin);
                }
            }

            /* What is rejected: anything without a gray or RGB profile. */
            {
                static const char *const what[] = {
                    "null", "device_rgb", "device_gray", "device_cmyk",
                    "pattern", "pattern_colored", "lab",
                };
                CGColorSpaceRef rej[7];

                rej[0] = NULL;
                rej[1] = CGColorSpaceCreateDeviceRGB();
                rej[2] = CGColorSpaceCreateDeviceGray();
                rej[3] = CGColorSpaceCreateDeviceCMYK();
                rej[4] = CGColorSpaceCreatePattern(CGColorSpaceCreateDeviceRGB());
                rej[5] = CGColorSpaceCreatePattern(NULL);
                rej[6] = CGColorSpaceCreateLab(d50, NULL, NULL);
                for (i = 0; i < 7; i++) {
                    snprintf(l, sizeof l, "lin/reject/%s/lin", what[i]);
                    b(l, CGColorSpaceCreateLinearized(rej[i]) == NULL);
                    snprintf(l, sizeof l, "lin/reject/%s/ext", what[i]);
                    b(l, CGColorSpaceCreateExtended(rej[i]) == NULL);
                    snprintf(l, sizeof l, "lin/reject/%s/xlin", what[i]);
                    b(l, CGColorSpaceCreateExtendedLinearized(rej[i]) == NULL);
                }
                for (i = 0; i < 7; i++) {
                    if (rej[i]) CGColorSpaceRelease(rej[i]);
                }
            }

            /* The black point is dropped.  A base built with an explicit
               black point and one built without have to linearize to the
               same bytes: the tag is not in the linear profile at all, so
               equality here is a statement about the profile, not about the
               arguments. */
            for (i = 0; i < 5; i++) {
                for (int which = 0; which < 2; which++) {
                    const char *kind = which ? "rgb" : "gray";
                    CGColorSpaceRef def = which ? rgbbase[i] : graybase[i];
                    CGColorSpaceRef wit = which ? rgbbp[i] : graybp[i];
                    CGColorSpaceRef a = CGColorSpaceCreateLinearized(def);
                    CGColorSpaceRef b2 = CGColorSpaceCreateLinearized(wit);
                    CFDataRef da = a ? CGColorSpaceCopyICCData(a) : NULL;
                    CFDataRef db = b2 ? CGColorSpaceCopyICCData(b2) : NULL;
                    int same = 0;

                    if (da && db &&
                        CFDataGetLength(da) == CFDataGetLength(db) &&
                        memcmp(CFDataGetBytePtr(da), CFDataGetBytePtr(db),
                            (size_t)CFDataGetLength(da)) == 0)
                        same = 1;
                    snprintf(l, sizeof l, "lin/blackpoint/%s/%zu/eq", kind, i);
                    b(l, a && CGColorSpaceEqualToColorSpace(a, b2));
                    snprintf(l, sizeof l, "lin/blackpoint/%s/%zu/icc", kind, i);
                    icc(l, da);
                    snprintf(l, sizeof l, "lin/blackpoint/%s/%zu/same_bytes", kind, i);
                    b(l, same);
                    if (da) CFRelease(da);
                    if (db) CFRelease(db);
                    if (a) CGColorSpaceRelease(a);
                    if (b2) CGColorSpaceRelease(b2);
                }
            }

            /* Chaining.  Linearizing a linearized space appends the word to
               the description again and grows the profile by the 22 bytes
               the text occupies, so this is where the description handling is
               actually pinned down; a builder that recomputed the
               description from the model would report 276 bytes here and be
               wrong.  Extending a linearized space keeps its profile. */
            for (i = 0; i < 5; i++) {
                for (int which = 0; which < 2; which++) {
                    const char *kind = which ? "rgb" : "gray";
                    CGColorSpaceRef src = which ? rgbbase[i] : graybase[i];
                    CGColorSpaceRef l1 = CGColorSpaceCreateLinearized(src);
                    CGColorSpaceRef l2 = CGColorSpaceCreateLinearized(l1);
                    CGColorSpaceRef l3 = CGColorSpaceCreateLinearized(l2);
                    CGColorSpaceRef e1 = CGColorSpaceCreateExtended(l1);
                    CFDataRef d;

                    if (l2) {
                        d = CGColorSpaceCopyICCData(l2);
                        snprintf(l, sizeof l, "lin/chain/%s/%zu/twice", kind, i);
                        icc(l, d);
                        if (d) CFRelease(d);
                    }
                    if (l3) {
                        d = CGColorSpaceCopyICCData(l3);
                        snprintf(l, sizeof l, "lin/chain/%s/%zu/thrice", kind, i);
                        icc(l, d);
                        if (d) CFRelease(d);
                    }
                    /* Two routes to the same place: linearizing the base
                       twice, and linearizing the once-linearized space.  The
                       results have to be equal *and* the same bytes. */
                    if (l2 && l3) {
                        CGColorSpaceRef direct =
                            CGColorSpaceCreateLinearized(
                                CGColorSpaceCreateLinearized(src));

                        snprintf(l, sizeof l, "lin/chain/%s/%zu/route_eq", kind, i);
                        b(l, CGColorSpaceEqualToColorSpace(l2, direct));
                        if (direct) CGColorSpaceRelease(direct);
                    }
                    if (e1) {
                        d = CGColorSpaceCopyICCData(e1);
                        snprintf(l, sizeof l, "lin/chain/%s/%zu/ext_of_lin", kind, i);
                        icc(l, d);
                        if (d) CFRelease(d);
                        /* The profile is the linearized one; the flags
                           differ. */
                        snprintf(l, sizeof l, "lin/chain/%s/%zu/ext_lin_eq", kind, i);
                        b(l, CGColorSpaceEqualToColorSpace(e1,
                            CGColorSpaceCreateExtendedLinearized(l1)));
                    }
                    /* A linearized space does not inherit its base's range:
                       the flag is not carried over. */
                    {
                        static const CGFloat r4[4] = { 0.25, 0.5, 0.75, 1 };
                        CGColorSpaceRef withr = which
                            ? CGColorSpaceCreateCalibratedRGB(d50, r4, g222, idm)
                            : CGColorSpaceCreateCalibratedGray(d50, NULL, 2.2);
                        CGColorSpaceRef lr = CGColorSpaceCreateLinearized(withr);

                        snprintf(l, sizeof l, "lin/chain/%s/%zu/no_range", kind, i);
                        b(l, lr && CGColorSpaceEqualToColorSpace(lr,
                            CGColorSpaceCreateLinearized(src)));
                        if (lr) CGColorSpaceRelease(lr);
                        if (withr) CGColorSpaceRelease(withr);
                    }
                    if (l1) CGColorSpaceRelease(l1);
                    if (l2) CGColorSpaceRelease(l2);
                    if (l3) CGColorSpaceRelease(l3);
                    if (e1) CGColorSpaceRelease(e1);
                }
            }

            /* A sweep over white points, so the copied colorants are pinned
               down across the quantisation rather than at two values. */
            for (i = 0; i < 24; i++) {
                CGFloat wp[3];

                for (k = 0; k < 3; k++)
                    wp[k] = (CGFloat)(0.05 * (double)(i * 3 + k));
                {
                    CGColorSpaceRef src = CGColorSpaceCreateCalibratedRGB(wp,
                        NULL, g222, p3m);
                    CGColorSpaceRef lin = CGColorSpaceCreateLinearized(src);
                    CFDataRef d = lin ? CGColorSpaceCopyICCData(lin) : NULL;

                    snprintf(l, sizeof l, "lin/sweep/wp_%zu", i);
                    icc(l, d);
                    if (d) CFRelease(d);
                    if (lin) CGColorSpaceRelease(lin);
                    if (src) CGColorSpaceRelease(src);
                }
            }

            /* And a sweep over gamma, which the linearized profile must not
               depend on at all: every one of these is the same 396 bytes as
               the gamma 2.2 case.  If the tone curve leaked through, these
               would differ. */
            for (i = 0; i < 12; i++) {
                CGFloat g[3];

                for (k = 0; k < 3; k++)
                    g[k] = (CGFloat)(0.25 * (double)(i + 1 + k));
                {
                    CGColorSpaceRef src = CGColorSpaceCreateCalibratedRGB(d50,
                        NULL, g, idm);
                    CGColorSpaceRef lin = CGColorSpaceCreateLinearized(src);
                    CGColorSpaceRef ref = CGColorSpaceCreateLinearized(
                        CGColorSpaceCreateCalibratedRGB(d50, NULL, g222, idm));
                    CFDataRef d = lin ? CGColorSpaceCopyICCData(lin) : NULL;

                    snprintf(l, sizeof l, "lin/sweep/gamma_%zu/icc", i);
                    icc(l, d);
                    /* And equal to the reference, which is what says the
                       gamma is gone rather than merely rounded. */
                    snprintf(l, sizeof l, "lin/sweep/gamma_%zu/eq_ref", i);
                    b(l, lin && CGColorSpaceEqualToColorSpace(lin, ref));
                    if (d) CFRelease(d);
                    if (lin) CGColorSpaceRelease(lin);
                    if (ref) CGColorSpaceRelease(ref);
                    if (src) CGColorSpaceRelease(src);
                }
            }

            for (i = 0; i < 5; i++) {
                CGColorSpaceRelease(graybase[i]);
                CGColorSpaceRelease(rgbbase[i]);
                CGColorSpaceRelease(graybp[i]);
                CGColorSpaceRelease(rgbbp[i]);
            }
        }

        /* CGColorSpaceCreateWithICCData.

           The inputs are profiles this step already produces byte for byte --
           the calibrated gray and RGB spaces -- so a difference here is about
           the parsing and not about the profile bytes underneath.  Each is
           round-tripped and the model, component count and profile handed back
           compared; a profile Apple accepts has to come back unchanged.

           Then the validation rules, each pinned by mutating a single field
           of a good RGB profile and reading the accept/reject answer back. */
        {
            static const CGFloat icc_d50[3] = { 0.9505, 1.0, 1.0890 };
            static const CGFloat icc_g222[3] = { 2.2, 2.2, 2.2 };
            static const CGFloat icc_idm[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
            CGColorSpaceRef srcspace[2];
            static const char *const nlab[] = { "gray", "rgb" };
            /* The signed tag this fixture keys each mutation off, and the
               value it is replaced with. */
            static const struct {
                const char *what;
                size_t off;
                const char *val;
            } muts[] = {
                /* Not the signature Apple wants. */
                { "acsp_upper", 36, "ACSP" },
                { "acsp_zero", 36, "\0\0\0\0" },
                /* Only mntr and scnr are display and input profiles; the
                   rest of the ICC device classes are refused. */
                { "class_prtr", 12, "prtr" },
                { "class_spac", 12, "spac" },
                { "class_abst", 12, "abst" },
                { "class_zero", 12, "\0\0\0\0" },
                { "class_upper", 12, "MNTR" },
                /* A colour space signature with no model. */
                { "cs_hsv", 16, "HSV " },
                { "cs_2clr", 16, "2CLR" },
                { "cs_zero", 16, "\0\0\0\0" },
            };
            const size_t nn = sizeof srcspace / sizeof srcspace[0];
            const size_t nm = sizeof muts / sizeof muts[0];
            size_t i2, m;
            unsigned char *buf;
            size_t blen;

            srcspace[0] = CGColorSpaceCreateCalibratedGray(icc_d50, NULL, 2.2);
            srcspace[1] = CGColorSpaceCreateCalibratedRGB(icc_d50, NULL,
                icc_g222, icc_idm);

            /* Round-trip a gray and an RGB profile. */
            for (i2 = 0; i2 < nn; i2++) {
                CFDataRef src = srcspace[i2]
                    ? CGColorSpaceCopyICCData(srcspace[i2]) : NULL;
                CGColorSpaceRef s = src ? CGColorSpaceCreateWithICCData(src) : NULL;
                CFDataRef back = s ? CGColorSpaceCopyICCData(s) : NULL;

                snprintf(l, sizeof l, "icc/%s/non_null", nlab[i2]);
                b(l, s != NULL);
                snprintf(l, sizeof l, "icc/%s/model", nlab[i2]);
                n(l, s ? (long long)CGColorSpaceGetModel(s) : -1);
                snprintf(l, sizeof l, "icc/%s/ncomp", nlab[i2]);
                n(l, s ? (long long)CGColorSpaceGetNumberOfComponents(s) : -1);
                snprintf(l, sizeof l, "icc/%s/name_is_null", nlab[i2]);
                b(l, s && CGColorSpaceCopyName(s) == NULL);
                snprintf(l, sizeof l, "icc/%s/base_is_null", nlab[i2]);
                b(l, s && CGColorSpaceGetBaseColorSpace(s) == NULL);
                snprintf(l, sizeof l, "icc/%s/ext_range", nlab[i2]);
                b(l, s && CGColorSpaceUsesExtendedRange(s));
                snprintf(l, sizeof l, "icc/%s/wide", nlab[i2]);
                b(l, s && CGColorSpaceIsWideGamutRGB(s));
                snprintf(l, sizeof l, "icc/%s/ps2", nlab[i2]);
                b(l, s && CGColorSpaceIsPSLevel2Compatible(s));
                snprintf(l, sizeof l, "icc/%s/is_icc", nlab[i2]);
                b(l, s && CGColorSpaceIsICCCompatible(s));
                /* The whole profile, unmasked: these are fixed templates, so
                   there is no creation date to hide. */
                snprintf(l, sizeof l, "icc/%s/profile", nlab[i2]);
                icc(l, back);
                /* And it is the input, not merely an equal profile. */
                snprintf(l, sizeof l, "icc/%s/roundtrip", nlab[i2]);
                b(l, back && src &&
                    CFDataGetLength(back) == CFDataGetLength(src) &&
                    memcmp(CFDataGetBytePtr(back), CFDataGetBytePtr(src),
                        (size_t)CFDataGetLength(src)) == 0);
                if (back) CFRelease(back);
                if (s) CGColorSpaceRelease(s);
                if (src) CFRelease(src);
            }

            /* What is refused outright. */
            b("icc/reject/null", CGColorSpaceCreateWithICCData(NULL) == NULL);
            {
                unsigned char junk[64];
                CFDataRef d;

                memset(junk, 0xAB, sizeof junk);
                d = CFDataCreate(kCFAllocatorDefault, junk, (CFIndex)sizeof junk);
                b("icc/reject/garbage",
                    CGColorSpaceCreateWithICCData(d) == NULL);
                if (d) CFRelease(d);
                d = CFDataCreate(kCFAllocatorDefault, junk, 8);
                b("icc/reject/short", CGColorSpaceCreateWithICCData(d) == NULL);
                if (d) CFRelease(d);
                d = CFDataCreate(kCFAllocatorDefault, junk, 0);
                b("icc/reject/empty", CGColorSpaceCreateWithICCData(d) == NULL);
                if (d) CFRelease(d);
            }

            /* Take an RGB profile as the base for the field mutations. */
            {
                CFDataRef src = CGColorSpaceCopyICCData(srcspace[1]);

                blen = (size_t)CFDataGetLength(src);
                buf = malloc(blen + 64);
                memcpy(buf, CFDataGetBytePtr(src), blen);

                /* Truncation at every interesting boundary.  Cutting one byte
                   short drops the last tag's final byte, which Apple rejects;
                   so does anything shorter. */
                {
                    size_t cuts[] = {
                        128, 132, 143, 144, blen - 1,
                    };
                    size_t c;

                    for (c = 0; c < sizeof cuts / sizeof cuts[0]; c++) {
                        CFDataRef d = CFDataCreate(kCFAllocatorDefault, buf,
                            (CFIndex)cuts[c]);
                        snprintf(l, sizeof l, "icc/trunc/%zu", cuts[c]);
                        b(l, CGColorSpaceCreateWithICCData(d) == NULL);
                        if (d) CFRelease(d);
                    }
                    /* The full profile is accepted, which is what makes the
                       truncation results above meaningful. */
                    {
                        CFDataRef d = CFDataCreate(kCFAllocatorDefault, buf,
                            (CFIndex)blen);
                        snprintf(l, sizeof l, "icc/trunc/full");
                        b(l, CGColorSpaceCreateWithICCData(d) != NULL);
                        if (d) CFRelease(d);
                    }
                }

                /* Trailing bytes past the end of the profile are dropped, so
                   the answer is still a space and its profile is the original
                   length rather than what went in. */
                {
                    static const size_t extra[] = { 1, 8, 64 };
                    size_t e;

                    memset(buf + blen, 0, 64);
                    for (e = 0; e < sizeof extra / sizeof extra[0]; e++) {
                        CFDataRef d = CFDataCreate(kCFAllocatorDefault, buf,
                            (CFIndex)(blen + extra[e]));
                        CGColorSpaceRef s =
                            CGColorSpaceCreateWithICCData(d);
                        CFDataRef back = s ? CGColorSpaceCopyICCData(s) : NULL;

                        snprintf(l, sizeof l, "icc/extra/%zu/non_null", extra[e]);
                        b(l, s != NULL);
                        snprintf(l, sizeof l, "icc/extra/%zu/len", extra[e]);
                        n(l, back ? (long long)CFDataGetLength(back) : -1);
                        snprintf(l, sizeof l, "icc/extra/%zu/roundtrip", extra[e]);
                        b(l, back &&
                            CFDataGetLength(back) == (CFIndex)blen &&
                            memcmp(CFDataGetBytePtr(back), buf, blen) == 0);
                        if (back) CFRelease(back);
                        if (s) CGColorSpaceRelease(s);
                        if (d) CFRelease(d);
                    }
                }

                /* The declared profile size is not what decides anything: a
                   lie in either direction, including zero and the maximum, is
                   still accepted and still yields the original bytes. */
                {
                    static const unsigned sizes[] = {
                        0u, 128u, 0xFFFFFFFFu,
                    };
                    size_t e;

                    for (e = 0; e < sizeof sizes / sizeof sizes[0]; e++) {
                        CFDataRef d;
                        CGColorSpaceRef s;
                        CFDataRef back;
                        unsigned char hdr[4];

                        hdr[0] = (unsigned char)(sizes[e] >> 24);
                        hdr[1] = (unsigned char)(sizes[e] >> 16);
                        hdr[2] = (unsigned char)(sizes[e] >> 8);
                        hdr[3] = (unsigned char)sizes[e];
                        memcpy(buf, hdr, 4);
                        d = CFDataCreate(kCFAllocatorDefault, buf,
                            (CFIndex)blen);
                        s = CGColorSpaceCreateWithICCData(d);
                        back = s ? CGColorSpaceCopyICCData(s) : NULL;
                        snprintf(l, sizeof l, "icc/declared/%zu/non_null", e);
                        b(l, s != NULL);
                        snprintf(l, sizeof l, "icc/declared/%zu/roundtrip", e);
                        b(l, back &&
                            CFDataGetLength(back) == (CFIndex)blen &&
                            memcmp(CFDataGetBytePtr(back), buf, blen) == 0);
                        if (back) CFRelease(back);
                        if (s) CGColorSpaceRelease(s);
                        if (d) CFRelease(d);
                    }
                    memcpy(buf, CFDataGetBytePtr(src), blen);
                }

                /* Version zero is refused; 2.1 and 4.0 are not. */
                {
                        static const unsigned vers[] = {
                            0u, 0x02100000u, 0x04000000u,
                        };
                        size_t e;

                        for (e = 0; e < sizeof vers / sizeof vers[0]; e++) {
                            unsigned char hdr[4];
                            CFDataRef d;

                            hdr[0] = (unsigned char)(vers[e] >> 24);
                            hdr[1] = (unsigned char)(vers[e] >> 16);
                            hdr[2] = (unsigned char)(vers[e] >> 8);
                            hdr[3] = (unsigned char)vers[e];
                            memcpy(buf + 8, hdr, 4);
                            d = CFDataCreate(kCFAllocatorDefault, buf,
                                (CFIndex)blen);
                            snprintf(l, sizeof l, "icc/version/%zu", e);
                            b(l, (CGColorSpaceCreateWithICCData(d) != NULL)
                                == (vers[e] != 0));
                            if (d) CFRelease(d);
                        }
                        memcpy(buf, CFDataGetBytePtr(src), blen);
                    }

                /* The remaining single-field mutations: signature, device
                   class and colour space. */
                for (m = 0; m < nm; m++) {
                    CFDataRef d;

                    memcpy(buf, CFDataGetBytePtr(src), blen);
                    memcpy(buf + muts[m].off, muts[m].val, 4);
                    d = CFDataCreate(kCFAllocatorDefault, buf, (CFIndex)blen);
                    snprintf(l, sizeof l, "icc/mut/%s", muts[m].what);
                    b(l, CGColorSpaceCreateWithICCData(d) == NULL);
                    if (d) CFRelease(d);
                }
                memcpy(buf, CFDataGetBytePtr(src), blen);

                /* Tag count and tag bounds.  A count that puts the table past
                   the end of the data, a tag whose offset and length run past
                   it, and a count of zero are all refused. */
                {
                    static const unsigned counts[] = {
                        0u, 1u, 9u, 10000u, 0xFFFFFFFFu,
                    };
                    size_t e;

                    for (e = 0; e < sizeof counts / sizeof counts[0]; e++) {
                        unsigned char v[4];
                        CFDataRef d;

                        v[0] = (unsigned char)(counts[e] >> 24);
                        v[1] = (unsigned char)(counts[e] >> 16);
                        v[2] = (unsigned char)(counts[e] >> 8);
                        v[3] = (unsigned char)counts[e];
                        memcpy(buf + 128, v, 4);
                        d = CFDataCreate(kCFAllocatorDefault, buf,
                            (CFIndex)blen);
                        snprintf(l, sizeof l, "icc/tagcount/%u", counts[e]);
                        b(l, CGColorSpaceCreateWithICCData(d) == NULL);
                        if (d) CFRelease(d);
                    }
                    memcpy(buf, CFDataGetBytePtr(src), blen);

                    /* The first tag entry points past the end, three ways. */
                    {
                        static const unsigned bad[] = {
                            0xFFFFFF00u, 0xFFFFFFFFu,
                        };
                        size_t e2;

                        for (e2 = 0; e2 < sizeof bad / sizeof bad[0]; e2++) {
                            unsigned char v[4];
                            CFDataRef d;

                            /* Offset. */
                            v[0] = (unsigned char)(bad[e2] >> 24);
                            v[1] = (unsigned char)(bad[e2] >> 16);
                            v[2] = (unsigned char)(bad[e2] >> 8);
                            v[3] = (unsigned char)bad[e2];
                            memcpy(buf + 136, v, 4);
                            d = CFDataCreate(kCFAllocatorDefault, buf,
                                (CFIndex)blen);
                            snprintf(l, sizeof l,
                                "icc/tag/off/%zu", e2);
                            b(l, CGColorSpaceCreateWithICCData(d) == NULL);
                            if (d) CFRelease(d);
                            /* Length. */
                            memcpy(buf, CFDataGetBytePtr(src), blen);
                            memcpy(buf + 140, v, 4);
                            d = CFDataCreate(kCFAllocatorDefault, buf,
                                (CFIndex)blen);
                            snprintf(l, sizeof l,
                                "icc/tag/len/%zu", e2);
                            b(l, CGColorSpaceCreateWithICCData(d) == NULL);
                            if (d) CFRelease(d);
                            memcpy(buf, CFDataGetBytePtr(src), blen);
                        }
                        /* An offset just short of the end, so offset+length
                           overshoots by the length. */
                        {
                            unsigned char v[4];
                            CFDataRef d;

                            v[0] = (unsigned char)((blen - 4) >> 24);
                            v[1] = (unsigned char)((blen - 4) >> 16);
                            v[2] = (unsigned char)((blen - 4) >> 8);
                            v[3] = (unsigned char)(blen - 4);
                            memcpy(buf + 136, v, 4);
                            d = CFDataCreate(kCFAllocatorDefault, buf,
                                (CFIndex)blen);
                            b("icc/tag/off/end",
                                CGColorSpaceCreateWithICCData(d) == NULL);
                            if (d) CFRelease(d);
                            memcpy(buf, CFDataGetBytePtr(src), blen);
                        }
                    }
                }

                free(buf);
                if (src) CFRelease(src);
            }

            /* Wide gamut for a profile-backed space is decided from the
               colorants, so it is swept over real spaces rather than one
               fixture.  Each matrix is the primaries of a space Apple ships,
               read out of its own profile and laid out for
               CreateCalibratedRGB: rows are X, Y and Z of red, green and blue.
               The profile that comes back is then fed to CreateWithICCData,
               because a calibrated space answers from a flag and says false
               for all of these. */
            {
                static const struct {
                    const char *name;
                    CGFloat m[9];
                } gamuts[] = {
                    { "srgb", { 0.43607, 0.38515, 0.14307,
                                0.22249, 0.71687, 0.06061,
                                0.01392, 0.09708, 0.71410 } },
                    { "genericrgb", { 0.45430, 0.35335, 0.15665,
                                      0.24191, 0.67363, 0.08446,
                                      0.01489, 0.09064, 0.71957 } },
                    { "genericrgblin", { 0.45430, 0.35330, 0.15660,
                                        0.24260, 0.67439, 0.08340,
                                        0.01480, 0.09039, 0.71950 } },
                    { "adobergb", { 0.60974, 0.20528, 0.14919,
                                    0.31111, 0.62567, 0.06322,
                                    0.01947, 0.06087, 0.74457 } },
                    { "dcip3", { 0.48616, 0.32385, 0.15419,
                                 0.22668, 0.71033, 0.06299,
                                 -0.00081, 0.04323, 0.78247 } },
                    { "displayp3", { 0.51512, 0.29198, 0.15710,
                                     0.24120, 0.69225, 0.06657,
                                     -0.00105, 0.04189, 0.78407 } },
                    { "rec2020", { 0.67348, 0.16566, 0.12505,
                                   0.27904, 0.67534, 0.04561,
                                   -0.00194, 0.02998, 0.79684 } },
                    { "acescg", { 0.68988, 0.14977, 0.12456,
                                  0.28452, 0.67169, 0.04379,
                                  -0.00604, 0.01001, 0.82094 } },
                    /* The extremes: the identity matrix is the widest
                       triangle a profile can describe. */
                    { "identity", { 1, 0, 0, 0, 1, 0, 0, 0, 1 } },
                    /* All three primaries pulled in towards the white point,
                       which shrinks the triangle without changing its shape
                       much. */
                    { "collapsed", { 0.05, 0.03, 0.02,
                                     0.03, 0.05, 0.02,
                                     0.02, 0.02, 0.05 } },
                };
                size_t gi;

                for (gi = 0; gi < sizeof gamuts / sizeof gamuts[0]; gi++) {
                    CGColorSpaceRef c = CGColorSpaceCreateCalibratedRGB(
                        icc_d50, NULL, icc_g222, gamuts[gi].m);
                    CFDataRef d = c ? CGColorSpaceCopyICCData(c) : NULL;
                    CGColorSpaceRef s = d
                        ? CGColorSpaceCreateWithICCData(d) : NULL;

                    snprintf(l, sizeof l, "icc/wide/%s", gamuts[gi].name);
                    b(l, s && CGColorSpaceIsWideGamutRGB(s));
                    if (s) CGColorSpaceRelease(s);
                    if (d) CFRelease(d);
                    if (c) CGColorSpaceRelease(c);
                }

                /* The boundary itself.  Interpolating every primary from sRGB
                   toward Display P3 crosses from false to true, and doing it
                   on one primary at a time does not -- the whole triangle has
                   to be bigger, not just a vertex. */
                for (gi = 0; gi <= 10; gi++) {
                    static const CGFloat srgb[9] = {
                        0.43607, 0.38515, 0.14307,
                        0.22249, 0.71687, 0.06061,
                        0.01392, 0.09708, 0.71410
                    };
                    static const CGFloat p3[9] = {
                        0.51512, 0.29198, 0.15710,
                        0.24120, 0.69225, 0.06657,
                        -0.00105, 0.04189, 0.78407
                    };
                    CGFloat t = (CGFloat)gi / 10.0f;
                    CGFloat m[9];
                    int k;

                    for (k = 0; k < 9; k++)
                        m[k] = (CGFloat)(float)(srgb[k]
                            + t * (p3[k] - srgb[k]));
                    {
                        CGColorSpaceRef c = CGColorSpaceCreateCalibratedRGB(
                            icc_d50, NULL, icc_g222, m);
                        CFDataRef d = c ? CGColorSpaceCopyICCData(c) : NULL;
                        CGColorSpaceRef s = d
                            ? CGColorSpaceCreateWithICCData(d) : NULL;

                        snprintf(l, sizeof l, "icc/wideblend/%zu", gi);
                        b(l, s && CGColorSpaceIsWideGamutRGB(s));
                        if (s) CGColorSpaceRelease(s);
                        if (d) CFRelease(d);
                        if (c) CGColorSpaceRelease(c);
                    }
                }
            }

            /* A DeviceN space takes its channel count and its device-class set
               from the body its tags describe, not from its own signature, so
               the signature is swept against three bodies: a gray one, an RGB
               one and a Lab one.  The Lab body matters because it is the only
               one here carrying the 'A2B0'/'B2A0' pair, which is the branch
               that reads a LUT.

               Two of the results are not obvious and both cost a probe to
               find.  A gray body is never a valid DeviceN body -- every
               'nCLR' is refused on it including '1CLR', which matters because
               a gray profile does have exactly one channel, so this is not
               the channel-count agreement doing the rejecting.  And the class
               set follows the body: an RGB body relabelled 'HSV ' accepts
               only 'mntr' and 'scnr', while a Lab body accepts all five. */
            {
                static const struct {
                    const char *tag;
                    const char *sig;
                } dsigs[] = {
                    { "hsv", "HSV " }, { "cmy", "CMY " }, { "yxy", "Yxy " },
                    { "luv", "Luv " }, { "hls", "HLS " },
                    { "1clr", "1CLR" }, { "2clr", "2CLR" }, { "3clr", "3CLR" },
                    { "4clr", "4CLR" }, { "5clr", "5CLR" }, { "6clr", "6CLR" },
                    { "9clr", "9CLR" }, { "0clr", "0CLR" }, { "aclr", "ACLR" },
                    { "clr", "CLR " }, { "ycbr", "YCbr" }, { "4clr-lower", "4cLR" },
                };
                static const char *const classes[] = {
                    "mntr", "scnr", "prtr", "spac", "abst",
                };
                struct { const char *n; CGColorSpaceRef ref; } bodies[3];
                size_t bi, si, ci;

                bodies[0].n = "gray";
                bodies[0].ref = CGColorSpaceCreateCalibratedGray(icc_d50, NULL, 2.2);
                bodies[1].n = "rgb";
                bodies[1].ref = CGColorSpaceCreateCalibratedRGB(icc_d50, NULL,
                    icc_g222, icc_idm);
                bodies[2].n = "lab";
                bodies[2].ref = CGColorSpaceCreateLab(icc_d50, NULL, NULL);

                for (bi = 0; bi < 3; bi++) {
                    CFDataRef bd = bodies[bi].ref
                        ? CGColorSpaceCopyICCData(bodies[bi].ref) : NULL;
                    size_t bl = bd ? (size_t)CFDataGetLength(bd) : 0;

                    for (si = 0; si < sizeof dsigs / sizeof dsigs[0]; si++) {
                        unsigned char *bb = (unsigned char *)malloc(bl);
                        CFDataRef dd;
                        CGColorSpaceRef s;

                        memcpy(bb, CFDataGetBytePtr(bd), bl);
                        memcpy(bb + 16, dsigs[si].sig, 4);
                        dd = CFDataCreate(kCFAllocatorDefault, bb, (CFIndex)bl);
                        s = CGColorSpaceCreateWithICCData(dd);
                        snprintf(l, sizeof l, "icc/devicen/%s/%s/ok",
                            bodies[bi].n, dsigs[si].tag);
                        b(l, s != NULL);
                        if (s) {
                            snprintf(l, sizeof l,
                                "icc/devicen/%s/%s/model", bodies[bi].n,
                                dsigs[si].tag);
                            n(l, (long long)CGColorSpaceGetModel(s));
                            snprintf(l, sizeof l,
                                "icc/devicen/%s/%s/ncomp", bodies[bi].n,
                                dsigs[si].tag);
                            n(l, (long long)CGColorSpaceGetNumberOfComponents(s));
                        }
                        CGColorSpaceRelease(s);
                        CFRelease(dd);

                        /* And the class byte, over the two bodies whose
                           class sets differ. */
                        for (ci = 0; bi != 0 && ci < 5; ci++) {
                            memcpy(bb, CFDataGetBytePtr(bd), bl);
                            memcpy(bb + 16, dsigs[si].sig, 4);
                            memcpy(bb + 12, classes[ci], 4);
                            dd = CFDataCreate(kCFAllocatorDefault, bb,
                                (CFIndex)bl);
                            s = CGColorSpaceCreateWithICCData(dd);
                            snprintf(l, sizeof l, "icc/devcls/%s/%s/%s",
                                bodies[bi].n, dsigs[si].tag, classes[ci]);
                            b(l, s != NULL);
                            CGColorSpaceRelease(s);
                            CFRelease(dd);
                        }
                        free(bb);
                    }
                    if (bd) CFRelease(bd);
                }
                for (bi = 0; bi < 3; bi++)
                    if (bodies[bi].ref) CGColorSpaceRelease(bodies[bi].ref);
            }

            /* Where the width of such a body comes from.  The LUT's own
               inputChannels byte is the only thing that scales with the
               answer -- 4 on the shipped CMYK profile, 3 on the shipped Lab
               one -- and it is what the implementation reads.

               It cannot be varied independently to confirm that, and the
               reason is the point: writing 4 into the Lab LUT does not produce
               a four-channel body, it produces a refusal.  Apple checks the
               LUT against the table it describes (the CLUT begins at offset 48
               in both 'mft1' and 'mft2' payloads, and the cell count implied by
               inputChannels, outputChannels and clutPoints has to fit the
               declared length), and 9^3 cells do not fit a 124-byte tag where
               9^2 ones did.  The formula is Apple's private one and is not
               recoverable from the two shipped profiles, so those cases are
               recorded as a documented divergence under icc/lutgeom/ rather
               than silently dropped: we accept a hand-corrupted LUT that Apple
               refuses.  No well-formed profile reaches this -- every shipped
               profile has a self-consistent LUT -- so the divergence is only
               reachable by corrupting a profile by hand.

               The declared length itself is not validated, which is checkable
               and is checked: growing 'A2B0' by a byte is still accepted. */
            {
                CGColorSpaceRef lab = CGColorSpaceCreateLab(icc_d50, NULL, NULL);
                CFDataRef bd = lab ? CGColorSpaceCopyICCData(lab) : NULL;
                size_t bl = bd ? (size_t)CFDataGetLength(bd) : 0;
                unsigned char *bb = (unsigned char *)malloc(bl);
                static const struct { const char *tag; const char *sig; } s2[] = {
                    { "3clr", "3CLR" }, { "4clr", "4CLR" },
                };
                size_t si;

                for (si = 0; si < 2; si++) {
                    unsigned char orig_in;
                    CFDataRef dd;
                    CGColorSpaceRef s;
                    uint32_t k, ae = 0, ao = 0, ntags;

                    memcpy(bb, CFDataGetBytePtr(bd), bl);
                    ntags = ((uint32_t)bb[128] << 24) | ((uint32_t)bb[129] << 16)
                        | ((uint32_t)bb[130] << 8) | bb[131];
                    for (k = 0; k < ntags; k++) {
                        const unsigned char *e = bb + 132 + k * 12;
                        if (memcmp(e, "A2B0", 4) == 0) {
                            ae = 132 + k * 12;
                            ao = ((uint32_t)e[4] << 24) | ((uint32_t)e[5] << 16)
                                | ((uint32_t)e[6] << 8) | e[7];
                            break;
                        }
                    }
                    if (!ao) { free(bb); break; }
                    orig_in = bb[ao + 8];

                    /* the byte on its own */
                    memcpy(bb + 16, s2[si].sig, 4);
                    bb[ao + 8] = 4;
                    dd = CFDataCreate(kCFAllocatorDefault, bb, (CFIndex)bl);
                    s = CGColorSpaceCreateWithICCData(dd);
                    snprintf(l, sizeof l, "icc/lutgeom/%s/in4", s2[si].tag);
                    b(l, s != NULL);
                    if (s) {
                        snprintf(l, sizeof l, "icc/lutgeom/%s/in4/ncomp", s2[si].tag);
                        n(l, (long long)CGColorSpaceGetNumberOfComponents(s));
                    }
                    CGColorSpaceRelease(s);
                    CFRelease(dd);

                    /* and the declared length grown by one, in the tag table */
                    bb[ao + 8] = orig_in;
                    bb[ae + 11]++;
                    dd = CFDataCreate(kCFAllocatorDefault, bb, (CFIndex)bl);
                    s = CGColorSpaceCreateWithICCData(dd);
                    snprintf(l, sizeof l, "icc/lutlen/%s", s2[si].tag);
                    b(l, s != NULL);
                    CGColorSpaceRelease(s);
                    CFRelease(dd);
                    bb[ae + 11]--;
                }
                free(bb);
                if (bd) CFRelease(bd);
                if (lab) CGColorSpaceRelease(lab);
            }

            for (i2 = 0; i2 < nn; i2++)
                if (srcspace[i2]) CGColorSpaceRelease(srcspace[i2]);
        }

        /* CGColorSpaceCreateIndexed.  The table is (last + 1) * the base's
           component count bytes, with no pad byte in front of an entry, and
           the 256-entry ceiling is a property of the table rather than of the
           base -- all of which the guard-page probe in the report confirmed by
           reading exactly three bytes per entry on an RGB base and one on a
           gray one before faulting. */
        {
            static const size_t lasts[] = { 0, 1, 2, 15, 16, 254, 255, 256, 257 };
            static const char *const ishort[] = { "gray", "rgb", "cmyk", "lab" };
            CGColorSpaceRef named[4];
            size_t ni;

            /* The four base kinds Apple was measured on.  The named spaces
               would be a fifth, but CGColorSpaceCreateWithName is a later
               milestone, and GenericRGB turned out to behave exactly like
               DeviceRGB here anyway -- the 256-entry ceiling is on the table,
               not on the base. */
            named[0] = CGColorSpaceCreateDeviceGray();
            named[1] = CGColorSpaceCreateDeviceRGB();
            named[2] = CGColorSpaceCreateDeviceCMYK();
            /* A Lab base, which is one component, so it exercises the same
               "one byte per entry" path as gray but from a calibrated space. */
            { static const CGFloat lab_wp[3] = { 0.95047, 1.0, 1.08883 };
              named[3] = CGColorSpaceCreateLab(lab_wp, NULL, NULL); }
            for (ni = 0; ni < 4; ni++) {
                size_t nc = named[ni]
                    ? CGColorSpaceGetNumberOfComponents(named[ni]) : 0;
                size_t li;

                for (li = 0; li < sizeof lasts / sizeof lasts[0]; li++) {
                    size_t last = lasts[li];
                    size_t bytes = (last + 2) * (nc + 1) + 64;
                    unsigned char *t = malloc(bytes);
                    CGColorSpaceRef s;

                    for (size_t k = 0; k < bytes; k++)
                        t[k] = (unsigned char)(k * 7u + 1u);
                    s = CGColorSpaceCreateIndexed(named[ni], last, t);
                    snprintf(l, sizeof l, "idx/%s/last%zu", ishort[ni], last);
                    b(l, s != NULL);
                    if (s) {
                        snprintf(l, sizeof l, "idx/%s/last%zu/model",
                            ishort[ni], last);
                        b(l, CGColorSpaceGetModel(s) == kCGColorSpaceModelIndexed);
                        snprintf(l, sizeof l, "idx/%s/last%zu/ncomp",
                            ishort[ni], last);
                        b(l, CGColorSpaceGetNumberOfComponents(s) == 1);
                        snprintf(l, sizeof l, "idx/%s/last%zu/name",
                            ishort[ni], last);
                        cfstr(l, CGColorSpaceCopyName(s));
                        snprintf(l, sizeof l, "idx/%s/last%zu/icc",
                            ishort[ni], last);
                        b(l, CGColorSpaceCopyICCData(s) != NULL);
                        snprintf(l, sizeof l, "idx/%s/last%zu/out",
                            ishort[ni], last);
                        b(l, CGColorSpaceSupportsOutput(s));
                        snprintf(l, sizeof l, "idx/%s/last%zu/base",
                            ishort[ni], last);
                        b(l, CGColorSpaceEqualToColorSpace(
                                CGColorSpaceGetBaseColorSpace(s), named[ni]));
                        CGColorSpaceRelease(s);
                    }
                    free(t);
                }
                if (named[ni]) CGColorSpaceRelease(named[ni]);
            }

            /* A base that cannot be expanded into: NULL, a pattern space, an
               indexed space.  The pattern space reports three components on an
               RGB base, so its count looks perfectly ordinary and only the
               model gives it away. */
            {
                unsigned char t[16] = { 0, 1, 2, 3, 4, 5, 6, 7,
                                        8, 9, 10, 11, 12, 13, 14, 15 };
                CGColorSpaceRef pat = CGColorSpaceCreatePattern(
                    CGColorSpaceCreateDeviceRGB());
                CGColorSpaceRef inner = CGColorSpaceCreateIndexed(
                    CGColorSpaceCreateDeviceRGB(), 3, t);

                for (size_t which = 0; which < 3; which++) {
                    CGColorSpaceRef base = which == 0 ? NULL
                        : which == 1 ? pat : inner;
                    snprintf(l, sizeof l, "idx/base/%zu", which);
                    b(l, CGColorSpaceCreateIndexed(base, 3, t) != NULL);
                }
                if (pat) CGColorSpaceRelease(pat);
                if (inner) CGColorSpaceRelease(inner);
            }

            /* A NULL table is refused, and the contents are never inspected:
               every fill, including all-zero and all-0xff, is accepted. */
            {
                unsigned char z[12] = { 0 }, f[12] = {
                    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                    0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
                CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
                CGColorSpaceRef s;

                b("idx/nulltable", CGColorSpaceCreateIndexed(rgb, 3, NULL) != NULL);
                s = CGColorSpaceCreateIndexed(rgb, 3, z);
                b("idx/content/zeros", s != NULL);
                if (s) CGColorSpaceRelease(s);
                s = CGColorSpaceCreateIndexed(rgb, 3, f);
                b("idx/content/ff", s != NULL);
                if (s) CGColorSpaceRelease(s);
                CGColorSpaceRelease(rgb);
            }

            /* Identity: same base and same table are equal; a differing
               table, a differing base or a differing `last' are not.  All
               three would compare equal if only the shape were compared, since
               every indexed space reports model 5 and one component. */
            {
                unsigned char t1[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
                unsigned char t2[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12 };
                CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
                CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
                CGColorSpaceRef a = CGColorSpaceCreateIndexed(rgb, 3, t1);
                CGColorSpaceRef a2 = CGColorSpaceCreateIndexed(rgb, 3, t1);
                CGColorSpaceRef d = CGColorSpaceCreateIndexed(rgb, 3, t2);
                CGColorSpaceRef g = CGColorSpaceCreateIndexed(gray, 3, t1);
                CGColorSpaceRef e = CGColorSpaceCreateIndexed(rgb, 2, t1);

                b("idx/eq/self", CGColorSpaceEqualToColorSpace(a, a2));
                b("idx/eq/table", CGColorSpaceEqualToColorSpace(a, d));
                b("idx/eq/base", CGColorSpaceEqualToColorSpace(a, g));
                b("idx/eq/last", CGColorSpaceEqualToColorSpace(a, e));
                b("idx/eq/vsbase", CGColorSpaceEqualToColorSpace(a, rgb));
                CGColorSpaceRelease(a);
                CGColorSpaceRelease(a2);
                CGColorSpaceRelease(d);
                CGColorSpaceRelease(g);
                CGColorSpaceRelease(e);
                CGColorSpaceRelease(rgb);
                CGColorSpaceRelease(gray);
            }
        }

        /* Pairwise equality, including pattern spaces with equal, different
           and absent bases. */
        for (size_t i = 0; i < nspaces; i++) {
            for (size_t k = 0; k < nspaces; k++) {
                snprintf(l, sizeof l, "cs/eq/%zu/%zu", i, k);
                b(l, CGColorSpaceEqualToColorSpace(sp[i], sp[k]));
                snprintf(l, sizeof l, "cs/eqrange/%zu/%zu", i, k);
                b(l, CGColorSpaceEqualToColorSpaceIgnoringRange(sp[i], sp[k]));
            }
        }

        {
            /* The built-in table, ID 1 through 32, in Apple's order. */
            static const char *const names[] = {
                "kCGColorSpaceGenericGrayGamma2_2",
                "kCGColorSpaceExtendedGray",
                "kCGColorSpaceLinearGray",
                "kCGColorSpaceExtendedLinearGray",
                "kCGColorSpaceGenericLab",
                "kCGColorSpaceGenericXYZ",
                "kCGColorSpaceDisplayP3",
                "kCGColorSpaceExtendedDisplayP3",
                "kCGColorSpaceLinearDisplayP3",
                "kCGColorSpaceExtendedLinearDisplayP3",
                "kCGColorSpaceDisplayP3_PQ",
                "kCGColorSpaceDisplayP3_HLG",
                "kCGColorSpaceDisplayP3_709OETF",
                "kCGColorSpaceAdobeRGB1998",
                "kCGColorSpaceSRGB",
                "kCGColorSpaceExtendedSRGB",
                "kCGColorSpaceLinearSRGB",
                "kCGColorSpaceExtendedLinearSRGB",
                "kCGColorSpaceACESCGLinear",
                "kCGColorSpaceITUR_709",
                "kCGColorSpaceITUR_709_PQ",
                "kCGColorSpaceITUR_709_HLG",
                "kCGColorSpaceITUR_2020",
                "kCGColorSpaceLinearITUR_2020",
                "kCGColorSpaceExtendedITUR_2020",
                "kCGColorSpaceExtendedLinearITUR_2020",
                "kCGColorSpaceITUR_2020_sRGBGamma",
                "kCGColorSpaceITUR_2100_PQ",
                "kCGColorSpaceITUR_2100_HLG",
                "kCGColorSpaceROMMRGB",
                "kCGColorSpaceDCIP3",
                "kCGColorSpaceCoreMedia709",
                /* The four second spellings, which answer with an identifier
                   they are not filed under: the 2020 PQ and HLG names reach
                   the 2100 identifiers, and the two EOTF names reach the PQ
                   identifiers they are spellings of.  None of them round-trips,
                   since CGColorSpaceNameFromID hands back the sibling name. */
                "kCGColorSpaceITUR_2020_PQ",
                "kCGColorSpaceITUR_2020_HLG",
                "kCGColorSpaceDisplayP3_PQ_EOTF",
                "kCGColorSpaceITUR_2020_PQ_EOTF",
            };
            const size_t nn = sizeof names / sizeof names[0];

            for (size_t i = 0; i < nn; i++) {
                CFStringRef s = CFStringCreateWithCString(NULL, names[i],
                    kCFStringEncodingUTF8);
                int id = CGColorSpaceIDFromName(s);

                snprintf(l, sizeof l, "cs/table/id/%s", names[i]);
                n(l, id);
                snprintf(l, sizeof l, "cs/table/roundtrip/%s", names[i]);
                cfstr(l, CGColorSpaceNameFromID(id));
                CFRelease(s);
            }
            for (int id = -2; id <= 40; id++) {
                snprintf(l, sizeof l, "cs/table/name/%d", id);
                cfstr(l, CGColorSpaceNameFromID(id));
            }
        }

        /* Names that are not in the table, to pin down whether the lookup
           is case sensitive, trims whitespace, or accepts a partial match. */
        {
            static const char *const junk[] = {
                "", " ", "srgb", "SRGB", "kCGColorSpaceSRGB ",
                " kCGColorSpaceSRGB", "kCGColorSpaceSRGBX",
                "kCGColorSpacesRGB", "Generic Gray", "GenericGray",
                "kCGColorSpaceDeviceRGB", "kCGColorSpacePattern",
                "kCGColorSpaceNonexistent",
            };
            const size_t nj = sizeof junk / sizeof junk[0];

            for (size_t i = 0; i < nj; i++) {
                CFStringRef s = CFStringCreateWithCString(NULL, junk[i],
                    kCFStringEncodingUTF8);
                snprintf(l, sizeof l, "cs/junk/%zu", i);
                n(l, CGColorSpaceIDFromName(s));
                CFRelease(s);
            }
        }

        /* CreateWithName.  Only names whose answers agree are recorded here.
           Apple resolves 40 of the 45 name constants and this step resolves
           thirty-one, so the other nine would be a mismatch rather than a test
           and each one joins this family as its profile template lands.  What
           is recorded is the part that is not obvious from the name: that the
           device names answer the existing singletons, that a pattern name
           builds something fresh every call, and that the generic Lab name
           resolves to precisely the profile CreateLab already emits. */
        {
            /* The names that resolve in both. */
            static const char *const resolves[] = {
                "kCGColorSpaceDeviceGray",
                "kCGColorSpaceDeviceRGB",
                "kCGColorSpaceDeviceCMYK",
                "kCGColorSpaceColoredPattern",
                "kCGColorSpaceGenericLab",
                /* The one v2.1 profile, carrying two names: it is a gray
                   space and so reports one component and no primaries, and
                   the extended name takes the same 356 bytes as its base. */
                "kCGColorSpaceLinearGray",
                "kCGColorSpaceExtendedLinearGray",
                /* The twelve v4-template profiles, which between them carry seventeen
                   names: the three extended-range aliases take the same
                   profile bytes as their base and differ only in the name and
                   the extended flag. */
                "kCGColorSpaceDisplayP3",
                "kCGColorSpaceExtendedDisplayP3",
                "kCGColorSpaceITUR_709",
                "kCGColorSpaceITUR_2020",
                "kCGColorSpaceExtendedITUR_2020",
                "kCGColorSpaceITUR_2020_sRGBGamma",
                "kCGColorSpaceDisplayP3_709OETF",
                "kCGColorSpaceROMMRGB",
                "kCGColorSpaceDCIP3",
                "kCGColorSpaceACESCGLinear",
                /* The seventeenth v4-template profile, and the only one of
                   them that is not a v4 profile: its header declares v2.1, so
                   it carries the legacy 'desc' and 'text' strings, two tags of
                   its own in the middle of the table, and a white point that is
                   not the quantised one the other sixteen share.  It reports
                   the same type as they do, which is worth having in the table
                   since nothing about the header would suggest it. */
                "kCGColorSpaceCoreMedia709",
                /* The linearized three, which differ from their gamma
                   counterparts in carrying a 'cicp' tag and a profile ID
                   where the others carry neither. */
                "kCGColorSpaceLinearSRGB",
                "kCGColorSpaceExtendedLinearSRGB",
                "kCGColorSpaceLinearDisplayP3",
                "kCGColorSpaceExtendedLinearDisplayP3",
                "kCGColorSpaceLinearITUR_2020",
                "kCGColorSpaceExtendedLinearITUR_2020",
                /* The six HDR profiles, which between them carry eight names:
                   the PQ and HLG variants of the Display P3, 709 and 2020
                   primaries.  None of the eight carries the extended flag,
                   since an HDR space is not a range variant of a base one.
                   The two 2100 names are not separate spaces at all: each is
                   a second spelling of the 2020 one, answering the same
                   pointer and reporting the 2100 name, which is what the name
                   check below is there to pin down. */
                "kCGColorSpaceITUR_2020_PQ",
                "kCGColorSpaceITUR_2100_PQ",
                "kCGColorSpaceITUR_2020_HLG",
                "kCGColorSpaceITUR_2100_HLG",
                "kCGColorSpaceITUR_709_PQ",
                "kCGColorSpaceITUR_709_HLG",
                "kCGColorSpaceDisplayP3_PQ",
                "kCGColorSpaceDisplayP3_HLG",
                /* And two more second spellings, which behave the same way:
                   each EOTF name answers the very pointer its non-EOTF sibling
                   does and reports the sibling's name, so these ten names
                   build six spaces. */
                "kCGColorSpaceDisplayP3_PQ_EOTF",
                "kCGColorSpaceITUR_2020_PQ_EOTF",
            };
            /* Names Apple itself refuses, so the NULL is a shared answer
               rather than this step's gap.  Unnamed and Invalid are the two
               that look like they should work and do not, and the three
               Generic spellings are the interesting ones: they read like
               names in the table and are not. */
            static const char *const refuses[] = {
                /* A real constant rather than a misspelling, and so listed
                   here rather than in the near misses below: Apple has the
                   name, the identifier is 0, and the lookup still refuses it.
                   That makes it the one name a lookup built off the
                   identifier table gets wrong for a different reason than the
                   other seven -- it looks like a known id rather than an
                   unknown one. */
                "kCGColorSpacePattern",
                "kCGColorSpaceUnnamed",
                "kCGColorSpaceInvalid",
                "kCGColorSpaceGenericCMYKGamma2_2",
                "kCGColorSpaceGenericRGBGamma2_2",
                "kCGColorSpaceGenericCMYKLinear",
            };
            /* Near misses around the five that work, since an exact byte
               match has to fail all of these: wrong case, a space on either
               side, an extra character at either end, the bare human-readable
               name, and the empty and blank strings. */
            static const char *const near[] = {
                "", " ", "sRGB", "SRGB", "P3", "GenericGray", "Generic Gray",
                "kCGColorSpaceGenericLab ", " kCGColorSpaceGenericLab",
                "kCGColorSpaceGenericLabX", "kCGColorSpaceGenericLabs",
                "kCGColorSpacegenericlab",
                "kCGColorSpaceDeviceRGB ", "kCGColorSpaceDeviceGrayX",
                "kCGColorSpacePattern", "kCGColorSpaceNonexistent",
                "kCGColorSpacesRGB",
            };
            const size_t nres = sizeof resolves / sizeof resolves[0];
            const size_t nref = sizeof refuses / sizeof refuses[0];
            const size_t nnear = sizeof near / sizeof near[0];
            /* What each resolving name answers, so the properties are
               asserted rather than inferred from the name's spelling. */
            static const struct {
                CGColorSpaceModel model;
                size_t ncomp;
                int type;
                int out;
                /* Whether the name reports an extended range, and whether it
                   reports a wide gamut.  Neither follows from the spelling, so
                   both are asserted: an extended-range alias takes the same
                   profile bytes as its base and is told apart only by the
                   first, and Rec. ITU-R BT.709-5 has primaries wider than
                   sRGB's yet is not called wide gamut.  Linear sRGB is the
                   sharper case of that: the same profile bytes answer false
                   on their own and true on the extended-range alias.

                   Extended linear gray is the case that fixes the rule in the
                   other direction: it reports an extended range and is still
                   not wide gamut, so the override that makes the linearized
                   RGB aliases wide cannot stand on its own and has to be
                   confined to spaces that have primaries at all. */
                int extended;
                int wide;
            } expect[] = {
                { kCGColorSpaceModelMonochrome, 1, 0, 1, 0, 0 }, /* DeviceGray */
                { kCGColorSpaceModelRGB, 3, 1, 1, 0, 0 },        /* DeviceRGB */
                { kCGColorSpaceModelCMYK, 4, 2, 1, 0, 0 },       /* DeviceCMYK */
                { kCGColorSpaceModelPattern, 0, 9, 0, 0, 0 },    /* ColoredPattern */
                { kCGColorSpaceModelLab, 3, 5, 1, 0, 0 },        /* GenericLab */
                { kCGColorSpaceModelMonochrome, 1, 6, 1, 0, 0 }, /* LinearGray */
                { kCGColorSpaceModelMonochrome, 1, 6, 1, 1, 0 }, /* Extended LinearGray */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* DisplayP3 */
                { kCGColorSpaceModelRGB, 3, 6, 1, 1, 1 },        /* Extended DisplayP3 */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 0 },        /* ITUR_709 */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* ITUR_2020 */
                { kCGColorSpaceModelRGB, 3, 6, 1, 1, 1 },        /* Extended ITUR_2020 */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* ITUR_2020 sRGBGamma */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* DisplayP3 709OETF */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* ROMM RGB */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* DCI P3 */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* ACES CG Linear */
                /* CoreMedia709 reports the same type as the sixteen v4
                   profiles around it even though its header is v2.1, and is
                   not wide gamut because its primaries are BT.709's, exactly
                   as the gamma space of those primaries is not. */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 0 },        /* CoreMedia709 */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 0 },        /* Linear sRGB */
                { kCGColorSpaceModelRGB, 3, 6, 1, 1, 1 },        /* Extended Linear sRGB */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* Linear Display P3 */
                { kCGColorSpaceModelRGB, 3, 6, 1, 1, 1 },        /* Extended Linear Display P3 */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* Linear ITUR_2020 */
                { kCGColorSpaceModelRGB, 3, 6, 1, 1, 1 },        /* Extended Linear ITUR_2020 */
                /* The six HDR spaces, one row per profile.  Each is a three-
                   component ICC space of the same type as the v4 ones, none is
                   extended, and the wide answer follows the primaries rather
                   than the spelling: the 2020 and P3 HDR spaces are wide gamut
                   and the 709 ones are not, exactly as their gamma
                   counterparts are.

                   The two 2100 names take the row of the 2020 space they
                   resolve to, since asking for either spelling answers the
                   same space and reports the 2100 name. */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* ITUR_2020_PQ */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* ITUR_2100_PQ */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* ITUR_2020_HLG */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* ITUR_2100_HLG */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 0 },        /* ITUR_709_PQ */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 0 },        /* ITUR_709_HLG */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* DisplayP3_PQ */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* DisplayP3_HLG */
                /* The EOTF spellings take their sibling's row for the same
                   reason the 2100 spellings do. */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* DisplayP3_PQ_EOTF */
                { kCGColorSpaceModelRGB, 3, 6, 1, 0, 1 },        /* ITUR_2020_PQ_EOTF */
            };
            size_t i;

            /* A NULL name is refused rather than faulted. */
            b("nm/null_name", CGColorSpaceCreateWithName(NULL) == NULL);

            for (i = 0; i < nres; i++) {
                CFStringRef s = CFStringCreateWithCString(NULL, resolves[i],
                    kCFStringEncodingUTF8);
                CGColorSpaceRef r = CGColorSpaceCreateWithName(s);

                snprintf(l, sizeof l, "nm/%s/non_null", resolves[i]);
                b(l, r != NULL);
                if (r) {
                    snprintf(l, sizeof l, "nm/%s/model", resolves[i]);
                    b(l, CGColorSpaceGetModel(r) == expect[i].model);
                    snprintf(l, sizeof l, "nm/%s/ncomp", resolves[i]);
                    n(l, (long long)CGColorSpaceGetNumberOfComponents(r));
                    snprintf(l, sizeof l, "nm/%s/type", resolves[i]);
                    n(l, CGColorSpaceGetType(r));
                    snprintf(l, sizeof l, "nm/%s/name", resolves[i]);
                    cfstr(l, CGColorSpaceCopyName(r));
                    snprintf(l, sizeof l, "nm/%s/icc", resolves[i]);
                    /* Masked rather than compared whole, because one name in
                       this list carries a live creation date: the generic Lab
                       profile is built by CreateLab, and CreateLab is the one
                       builder here that stamps the current time rather than a
                       constant.  Every other named profile in the list is
                       byte-identical run to run, and a masked date cannot hide
                       a difference in any of those -- the bytes being masked are
                       the same bytes on both sides. */
                    icc_live_date(l, CGColorSpaceCopyICCData(r));
                    snprintf(l, sizeof l, "nm/%s/base_is_null", resolves[i]);
                    b(l, CGColorSpaceGetBaseColorSpace(r) == NULL);
                    snprintf(l, sizeof l, "nm/%s/out", resolves[i]);
                    b(l, CGColorSpaceSupportsOutput(r));
                    /* The three device names are the singletons themselves,
                       so a named lookup and the constructor are one object.
                       Apple agrees: both return the same address. */
                    snprintf(l, sizeof l, "nm/%s/stable", resolves[i]);
                    b(l, CGColorSpaceCreateWithName(s) == r);
                    snprintf(l, sizeof l, "nm/%s/extended", resolves[i]);
                    b(l, CGColorSpaceUsesExtendedRange(r) == expect[i].extended);
                    snprintf(l, sizeof l, "nm/%s/wide", resolves[i]);
                    b(l, CGColorSpaceIsWideGamutRGB(r) == expect[i].wide);
                    /* Every one of these is a singleton, so the second
                       call answers the same pointer rather than a copy -- the
                       `stable' line above.  Apple goes further and makes them
                       immortal, but that shows as a retain count read through
                       the CF runtime rather than as anything a caller can
                       compare, so the identity is the part recorded here. */
                    CGColorSpaceRelease(r);
                }
                CFRelease(s);
            }
            /* These four pairs are one space under two names, which
               the per-name lines above cannot show: each answers its own
               stability check, so both spellings are stable without either
               revealing that they are stable as the same object.  Asking for
               both and comparing the pointers is what distinguishes a second
               name from a second space. */
            {
                static const char *const pairs[][2] = {
                    { "kCGColorSpaceITUR_2020_PQ", "kCGColorSpaceITUR_2100_PQ" },
                    { "kCGColorSpaceITUR_2020_HLG", "kCGColorSpaceITUR_2100_HLG" },
                    { "kCGColorSpaceDisplayP3_PQ_EOTF", "kCGColorSpaceDisplayP3_PQ" },
                    { "kCGColorSpaceITUR_2020_PQ_EOTF", "kCGColorSpaceITUR_2020_PQ" },
                };
                static const char *const reported[4] = {
                    "kCGColorSpaceITUR_2100_PQ", "kCGColorSpaceITUR_2100_HLG",
                    "kCGColorSpaceDisplayP3_PQ", "kCGColorSpaceITUR_2100_PQ",
                };
                const size_t np = sizeof pairs / sizeof pairs[0];

                for (i = 0; i < np; i++) {
                    CFStringRef a = CFStringCreateWithCString(NULL,
                        pairs[i][0], kCFStringEncodingUTF8);
                    CFStringRef bstr = CFStringCreateWithCString(NULL,
                        pairs[i][1], kCFStringEncodingUTF8);
                    CGColorSpaceRef ra = CGColorSpaceCreateWithName(a);
                    CGColorSpaceRef rb = CGColorSpaceCreateWithName(bstr);

/* Both spellings in one label would run to
                       "nm/kCGColorSpaceITUR_2020_HLG/kCGColorSpaceITUR_2100_HLG",
                       which does not fit the 64 bytes every label here has,
                       so the pair is named by its first half. */
                    snprintf(l, sizeof l, "nm/%s/pair_same", pairs[i][0]);
                    b(l, ra == rb);
                    /* And the pair reports the sibling's spelling under both
                       names, so the name is a property of the space rather
                       than of the lookup. */
                    snprintf(l, sizeof l, "nm/%s/pair_name", pairs[i][0]);
                    cfstr(l, CGColorSpaceCopyName(ra));
                    snprintf(l, sizeof l, "nm/%s/pair_reported_is",
                        pairs[i][0]);
                    {
                        /* Compared as ASCII: cfstr above already records the
                           spelling itself, and this only has to agree with
                           it, which the same helper cannot say. */
                        CFStringRef got = CGColorSpaceCopyName(ra);
                        char buf[64] = "";
                        int match;

                        match = got && CFStringGetCString(got, buf,
                            sizeof buf, kCFStringEncodingUTF8);
                        b(l, match && strcmp(buf, reported[i]) == 0);
                        if (got)
                            CFRelease(got);
                    }
                    CGColorSpaceRelease(ra);
                    CGColorSpaceRelease(rb);
                    CFRelease(a);
                    CFRelease(bstr);
                }
            }
            for (i = 0; i < nref; i++) {
                CFStringRef s = CFStringCreateWithCString(NULL, refuses[i],
                    kCFStringEncodingUTF8);
                snprintf(l, sizeof l, "nm/refused/%zu", i);
                b(l, CGColorSpaceCreateWithName(s) == NULL);
                CFRelease(s);
            }
            for (i = 0; i < nnear; i++) {
                CFStringRef s = CFStringCreateWithCString(NULL, near[i],
                    kCFStringEncodingUTF8);
                snprintf(l, sizeof l, "nm/near/%zu", i);
                b(l, CGColorSpaceCreateWithName(s) == NULL);
                CFRelease(s);
            }

            /* The extended-range alias and the space it is named after, one
               pair at a time.  Every pair shares its profile byte for byte and
               differs only in the flag, so the two functions answer opposite
               ways: unequal, and then equal once the range is ignored.  The
               equality matrix above cannot reach this, since it covers only
               the device and pattern spaces and none of those has a range.

               Linear gray is here for the same reason as the rest and with
               nothing extra to say: it is an alias like the others, and the
               fact that it is not wide gamut is asserted in the table above. */
            {
                static const char *const alias[][2] = {
                    { "kCGColorSpaceDisplayP3", "kCGColorSpaceExtendedDisplayP3" },
                    { "kCGColorSpaceLinearSRGB", "kCGColorSpaceExtendedLinearSRGB" },
                    { "kCGColorSpaceLinearDisplayP3", "kCGColorSpaceExtendedLinearDisplayP3" },
                    { "kCGColorSpaceLinearITUR_2020", "kCGColorSpaceExtendedLinearITUR_2020" },
                    { "kCGColorSpaceITUR_2020", "kCGColorSpaceExtendedITUR_2020" },
                    { "kCGColorSpaceLinearGray", "kCGColorSpaceExtendedLinearGray" },
                };
                const size_t nal = sizeof alias / sizeof alias[0];

                for (i = 0; i < nal; i++) {
                    CFStringRef bs = CFStringCreateWithCString(NULL,
                        alias[i][0], kCFStringEncodingUTF8);
                    CFStringRef es = CFStringCreateWithCString(NULL,
                        alias[i][1], kCFStringEncodingUTF8);
                    CGColorSpaceRef bsp = CGColorSpaceCreateWithName(bs);
                    CGColorSpaceRef esp = CGColorSpaceCreateWithName(es);

                    snprintf(l, sizeof l, "nm/alias/%zu/eq", i);
                    b(l, CGColorSpaceEqualToColorSpace(bsp, esp));
                    snprintf(l, sizeof l, "nm/alias/%zu/eqrange", i);
                    b(l, CGColorSpaceEqualToColorSpaceIgnoringRange(bsp, esp));
                    CFRelease(bs);
                    CFRelease(es);
                    CGColorSpaceRelease(bsp);
                    CGColorSpaceRelease(esp);
                }
            }

            /* Identity with the device constructors, which is a stronger
               statement than "the properties match". */
            {
                CFStringRef g = CFStringCreateWithCString(NULL,
                    "kCGColorSpaceDeviceGray", kCFStringEncodingUTF8);
                CFStringRef r = CFStringCreateWithCString(NULL,
                    "kCGColorSpaceDeviceRGB", kCFStringEncodingUTF8);
                CFStringRef c = CFStringCreateWithCString(NULL,
                    "kCGColorSpaceDeviceCMYK", kCFStringEncodingUTF8);

                b("nm/identity/gray",
                    CGColorSpaceCreateWithName(g) == CGColorSpaceCreateDeviceGray());
                b("nm/identity/rgb",
                    CGColorSpaceCreateWithName(r) == CGColorSpaceCreateDeviceRGB());
                b("nm/identity/cmyk",
                    CGColorSpaceCreateWithName(c) == CGColorSpaceCreateDeviceCMYK());
                CFRelease(g);
                CFRelease(r);
                CFRelease(c);
            }

            /* A pattern name builds a new object on every call rather than
               answering a singleton, so the caller owns each result and two
               calls are not the same space. */
            {
                CFStringRef p = CFStringCreateWithCString(NULL,
                    "kCGColorSpaceColoredPattern", kCFStringEncodingUTF8);
                CGColorSpaceRef p1 = CGColorSpaceCreateWithName(p);
                CGColorSpaceRef p2 = CGColorSpaceCreateWithName(p);

                b("nm/pattern/fresh", p1 != p2);
                b("nm/pattern/eq_across_calls",
                    CGColorSpaceEqualToColorSpace(p1, p2));
                if (p1)
                    CGColorSpaceRelease(p1);
                if (p2)
                    CGColorSpaceRelease(p2);
                CFRelease(p);
            }

            /* The generic Lab name resolves to the 496-byte profile
               CreateLab already emits, and to nothing else: D65 cannot be held
               exactly by a float, so the white point is dropped whole and the
               profile is the same one D50 produces.  Both comparisons mask the
               creation date, which is the only live field. */
            {
                static const CGFloat d65[3] = { 0.9505, 1.0, 1.089 };
                static const CGFloat d50[3] = { 0.9642, 1.0, 0.8249 };
                CFStringRef s = CFStringCreateWithCString(NULL,
                    "kCGColorSpaceGenericLab", kCFStringEncodingUTF8);
                CGColorSpaceRef named = CGColorSpaceCreateWithName(s);
                CGColorSpaceRef l65 = CGColorSpaceCreateLab(d65, NULL, NULL);
                CGColorSpaceRef l50 = CGColorSpaceCreateLab(d50, NULL, NULL);
                CFDataRef a = named ? CGColorSpaceCopyICCData(named) : NULL;
                CFDataRef b65 = l65 ? CGColorSpaceCopyICCData(l65) : NULL;
                CFDataRef b50 = l50 ? CGColorSpaceCopyICCData(l50) : NULL;

                icc_live_date("nm/lab/icc", a);
                icc_live_date("nm/lab/icc_d65", b65);
                icc_live_date("nm/lab/icc_d50", b50);
                /* The named profile matches the D65-built one and not the 516 bytes
                   a white point a float can hold produces; the two digests
                   above are recorded separately so that shows up as data. */
                /* Apple reports the named space and a space built from D65
                   unequal even though the two carry the same 496 bytes: the
                   profile cannot be what decides it, because it cannot tell
                   them apart.  The white point can. */
                b("nm/lab/neq_createlab_d65",
                    named && l65
                        && !CGColorSpaceEqualToColorSpace(named, l65));
                if (a) CFRelease(a);
                if (b65) CFRelease(b65);
                if (b50) CFRelease(b50);
                if (l65) CGColorSpaceRelease(l65);
                if (l50) CGColorSpaceRelease(l50);
                if (named) CGColorSpaceRelease(named);
                CFRelease(s);
            }

            /* The lookup reads contents rather than identity, so a mutable
               string built at runtime resolves like a constant does. */
            {
                CFMutableStringRef m = CFStringCreateMutable(NULL, 0);

                CFStringAppend(m, CFSTR("kCGColorSpace"));
                CFStringAppend(m, CFSTR("DeviceRGB"));
                b("nm/mutable_resolves",
                    CGColorSpaceCreateWithName(m) == CGColorSpaceCreateDeviceRGB());
                CFRelease(m);
            }
        }

        /* A retained device space survives the release, because it is a
           singleton and not something the caller owns. */
        CGColorSpaceRetain(sp[0]);
        CGColorSpaceRelease(sp[0]);
        b("cs/singleton/survives_release", CGColorSpaceGetName(sp[0]) != NULL);
        n("cs/pattern_ncomp", CGColorSpaceGetNumberOfComponents(sp[3]));

        for (size_t i = 0; i < nspaces; i++) CGColorSpaceRelease(sp[i]);
    }

    return 0;
}
