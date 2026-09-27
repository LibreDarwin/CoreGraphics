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

static void b(const char *label, int v)
{
    printf("%s %d\n", label, v != 0);
}

static void tr(const char *label, CGAffineTransform t)
{
    printf("%s %a %a %a %a %a %a\n", label, (double)t.a, (double)t.b,
        (double)t.c, (double)t.d, (double)t.tx, (double)t.ty);
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

    return 0;
}
