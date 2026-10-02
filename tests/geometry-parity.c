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
   digest was computed over the same bytes rather than a stale copy. */
static void icc(const char *label, CFDataRef d)
{
    unsigned char md[CC_MD5_DIGEST_LENGTH];
    const unsigned char *p;
    CFIndex len;

    if (!d) {
        printf("%s (null)\n", label);
        return;
    }
    p = CFDataGetBytePtr(d);
    len = CFDataGetLength(d);
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
       everywhere else. */
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
