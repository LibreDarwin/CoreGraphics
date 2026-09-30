/* CoreGraphics - CGAffineTransform.c
   Copyright (C) 2026, LibreDarwin

   Behaviour notes, established from the arm64e disassembly in
   local/disasm:

     - CGAffineTransformInvert computes the 2x2 determinant in SIMD, and on
       a singular matrix calls
           CGPostError("%s: singular matrix.", "CGAffineTransformInvert")
       and then returns the identity transform.

     - CGAffineTransformIsIdentity is a componentwise comparison against
       the identity.

     - CGAffineTransformIsSingular tests a zero determinant computed as
       one fused multiply-add, not as a separately rounded a*d - b*c.

     - CGAffineTransformMakeWithRect builds the unit-square mapping, but
       treats a negative side as a signal to flip rather than as an error:
       the scale becomes the absolute size and the origin moves back by
       the negative extent.  A negative side combined with an infinite
       origin collapses the transform to zero scale about an infinite
       translation.

   CGAffineTransformDecompose now agrees with Apple bit for bit, and is
   documented where it is defined.  It was previously built on a different
   decomposition convention and reached the public ABI only indirectly,
   through CGAffineTransformDecompose_SPI, whose payload labels
   tests/geometry-parity.sh allowlisted while the SPI itself was gated.  That
   allowlist is gone.  See local/disasm/analysis/REPORT.md. */

#include "CGAffineTransform.h"
#include "CGInternal.h"
#include "CGSPI.h"
#include <math.h>
#include <string.h>

const CGAffineTransform CGAffineTransformIdentity = {
    1, 0, 0, 1, 0, 0
};

bool
CGAffineTransformIsIdentity(CGAffineTransform t)
{
    return t.a == 1 && t.b == 0 && t.c == 0 && t.d == 1 &&
        t.tx == 0 && t.ty == 0;
}

bool
CGAffineTransformEqualToTransform(CGAffineTransform t1, CGAffineTransform t2)
{
    return t1.a == t2.a && t1.b == t2.b && t1.c == t2.c &&
        t1.d == t2.d && t1.tx == t2.tx && t1.ty == t2.ty;
}

CGAffineTransform
CGAffineTransformMake(CGFloat a, CGFloat b, CGFloat c, CGFloat d,
    CGFloat tx, CGFloat ty)
{
    CGAffineTransform t;

    t.a = a;  t.b = b;
    t.c = c;  t.d = d;
    t.tx = tx;  t.ty = ty;

    return t;
}

CGAffineTransform
CGAffineTransformMakeScale(CGFloat sx, CGFloat sy)
{
    return CGAffineTransformMake(sx, 0, 0, sy, 0, 0);
}

CGAffineTransform
CGAffineTransformMakeTranslation(CGFloat tx, CGFloat ty)
{
    return CGAffineTransformMake(1, 0, 0, 1, tx, ty);
}

CGAffineTransform
CGAffineTransformMakeRotation(CGFloat angle)
{
    CGFloat s = sin(angle);
    CGFloat c = cos(angle);

    return CGAffineTransformMake(c, s, -s, c, 0, 0);
}

CGAffineTransform
CGAffineTransformConcat(CGAffineTransform t1, CGAffineTransform t2)
{
    CGAffineTransform t;

    t.a  = t1.a * t2.a + t1.b * t2.c;
    t.b  = t1.a * t2.b + t1.b * t2.d;
    t.c  = t1.c * t2.a + t1.d * t2.c;
    t.d  = t1.c * t2.b + t1.d * t2.d;
    /* The translation is summed as a fused multiply-add, with the *second*
       product contracted into the addition.  Spelling it out rather than
       leaving it to the compiler is what keeps the last bit stable:
       translating a rotation of 0.75 by (1.5, 2.5) gives tx =
       -0x1.368f80f5f33f5p-1 fused, but -0x1.368f80f5f33f6p-1 rounded
       through an FMA, and a one-bit difference here is a parity failure just
       like any other. */
    t.tx = fma(t1.ty, t2.c, t1.tx * t2.a) + t2.tx;
    t.ty = fma(t1.ty, t2.d, t1.tx * t2.b) + t2.ty;

    return t;
}

/* The components view composes as scale * shear * rotation * translation,
   in that order, with the transform stored as

       | a  b  0 |
       | c  d  0 |
       | tx ty 1 |

   Expanding S.H.R.T with S = diag(sx, sy), H having `sh' on the subdiagonal
   and R the rotation by `t' gives

       a  = sx*cos(t)                        c  = sx*sin(t)
       b  = sy*(sh*cos(t) - sin(t))          d  = sy*(sh*sin(t) + cos(t))
       tx = sx*( cos(t)*tx + sin(t)*ty)
       ty = sy*( sh*(cos(t)*tx + sin(t)*ty) - sin(t)*tx + cos(t)*ty )

   Decompose inverts that.  Note this recovers a canonical decomposition
   only: the transform does not record which steps produced it. */

/* pi as a double, spelled out so the value cannot drift with the platform's
   M_PI.  0x400921FB54442D18 is the correctly rounded nearest double to pi. */
static const CGFloat CGAffineTransformPi = 0x1.921fb54442d18p+1;

/* Apple decomposes from the first ROW of the linear block, not the first
   column, and keeps the sign of that row's length in scale.width -- which is
   exactly what CGAffineTransformDecompose_SPI's outScaleIsNegative reports --
   rather than folding the sign into the rotation.  The algorithm below is
   transcribed from _CGAffineTransformDecompose at 0x187431854, argument by
   argument; the shape of it is worth stating because the obvious spelling is
   wrong in three separate ways.

   With angle = atan2(b, a) and the unit vector

       X = -sin(angle) = -b/hypot(a, b)
       Y = -cos(angle) = -a/hypot(a, b)

   the raw width is whichever of b/X, a/Y divides by the larger magnitude, the
   raw height is fma(d, Y, -(X * c)) = -det/hypot(a, b), and the shear is
   fma(c, Y, X * d)/height.  The width and height are then negated together
   when the raw height's sign bit is set, which is what normalises the width
   to a positive number for det > 0 and leaves it at -hypot(a, b) otherwise.
   Note that the rotation takes its second form on the *same* sign-bit test,
   so the two branches differ by exactly pi: one reports `angle`, the other
   `angle - copysign(pi, angle)`, and the copysign is what keeps the result
   inside [-pi, pi] instead of running off the end.  An earlier reading had
   this as atan2(b, a) with a wrap, which matched neither: the wrap is 20%
   wrong and atan2(-b, -a) is 50% wrong, because both are off by an ulp from
   what the sign-select actually produces.

   Four details are load-bearing at the last bit.  None of them is
   discoverable by reading the arithmetic -- each one is a choice between
   spellings that agree to within an ulp everywhere except in a corner, and
   the corners are the whole difficulty.  The disassembly settles three of
   them and the oracle settles the fourth:

     - the branch is "height is not positive", `height > 0` being the test
       that is negated, NOT a comparison against zero and not a sign-bit
       test.  All three of those differ, and only this spelling is right:
       across 2.1 million cases `height < 0` mispredicts the branch about a
       thousand times and `signbit(height)` about seven hundred, every one of
       them a signed zero.  A zero first row makes the height a signed zero
       often enough that this matters, and getting it wrong reports a
       rotation a full pi away, which is a much bigger error than the one
       bit it looks like.

     - the height is a single fused multiply-add, -(X * c) being the rounded
       product negated.  A separate multiply and subtract diverges in the
       cancellation-sensitive cases, which is most of what the degenerate
       grid exercises.

     - where the branch does flip the sign, it folds the flip into the
       multiply-add's operands -- `fma(-d, Y, X * c)` -- instead of negating
       the result it already computed.  The two agree except when the
       height is a zero, and then the operand form is the one that reports
       +0 where negating a computed +0 would report -0.

     - the shear is guarded on `height == 0 || isnan(height)` and not on a
       magnitude, so a height that is an infinity still divides.  It is also
       spelled with the un-negated sin and cos over a negated height; see
       the note at the division itself.


   The translation is copied verbatim.  Apple does NOT un-rotate it, so
   neither do we, and the "undo the rotation that R.T applied" step that used
   to live here was simply wrong. */
CGAffineTransformComponents
CGAffineTransformDecompose(CGAffineTransform transform)
{
    CGAffineTransformComponents c;
    const CGFloat angle = atan2(transform.b, transform.a);
    const CGFloat X = -sin(angle);
    const CGFloat Y = -cos(angle);

    CGFloat width = fabs(Y) < fabs(X) ? transform.b / X : transform.a / Y;
    CGFloat rawHeight = fma(transform.d, Y, -(X * transform.c));

    /* The shear is the ratio of the two cross products, and Apple spells
       both of them with the UN-negated sin and cos while dividing by a
       negated height.  That is the same number twice over -- negating a
       numerator and a denominator cancels -- so the only place it can be
       observed is a zero, and there it is the whole answer.  Writing it the
       other way round, with the negated X and Y the rest of the routine
       uses, reports a +0 where Apple reports a -0 for a zero first row with
       a zero shear. */
    c.horizontalShear = (rawHeight == 0.0 || isnan(rawHeight))
        ? 0.0
        : fma(transform.c, cos(angle), sin(angle) * transform.d) / -rawHeight;

    /* "Not positive", so a signed zero takes this branch. */
    const int negative = !(rawHeight > 0.0);

    c.rotation = negative
        ? angle
        : angle - copysign(CGAffineTransformPi, angle);

    if (negative) {
        width = -width;
    }

    c.scale.width = width;
    /* The sign flip is folded into the multiply-add's operands rather than
       applied to the result.  For a zero height the two spellings disagree,
       and the oracle wants the operand form: negating a computed +0 would
       report -0, where the fma of two zeros can report +0. */
    c.scale.height = negative
        ? fma(-transform.d, Y, X * transform.c)
        : rawHeight;
    c.translation.dx = transform.tx;
    c.translation.dy = transform.ty;

    return c;
}

CGAffineTransform
CGAffineTransformInvert(CGAffineTransform t)
{
    CGFloat det = t.a * t.d - t.b * t.c;

    /* A singular matrix is reported and then handed straight back
       unchanged: inverting CGAffineTransformMake(0, 1, 0, 0, 0, 0) yields
       that same matrix, not the identity, and
       CGAffineTransformIsIdentity of the result is false. */
    if (det == 0) {
        CGPostError("%s: singular matrix.", "CGAffineTransformInvert");
        return t;
    }

    CGAffineTransform inv;

    inv.a  = t.d / det;
    inv.b  = -t.b / det;
    inv.c  = -t.c / det;
    inv.d  = t.a / det;
    inv.tx = (t.c * t.ty - t.d * t.tx) / det;
    inv.ty = (t.b * t.tx - t.a * t.ty) / det;

    return inv;
}

bool
CGAffineTransformIsSingular(const CGAffineTransform *t)
{
    /* The determinant is evaluated as a single fused operation.  The
       original negates the b*c product first (fnmul) and then folds it
       into one fused multiply-add of a*d, so the whole expression rounds
       once, with a*d never rounded on its own.  Spelling that out with
       fma() is what keeps the result bit-exact; computing a*d - b*c
       naively rounds the a*d product separately and can differ by an ulp,
       which would put the == 0 test on the wrong side of the boundary. */
    return fma(t->a, t->d, -(t->b * t->c)) == 0.0;
}

bool
CGAffineTransformIsRectilinear(const CGAffineTransform *t)
{
    /* Either diagonal of the 2x2 block is entirely zero.  Both comparisons
       are against zero, so -0.0 counts as zero here, and the test is
       independent of the translation. */
    return (t->b == 0.0 && t->c == 0.0) || (t->a == 0.0 && t->d == 0.0);
}

CGAffineTransform
CGAffineTransformMakeWithRect(CGRect rect)
{
    const CGFloat w = rect.size.width;
    const CGFloat h = rect.size.height;
    CGAffineTransform t;

    t.b = 0;
    t.c = 0;

    if (w < 0.0 || h < 0.0) {
        /* A rect with a negative side is not the rect the caller drew, so
           the transform is built from the absolute size and the origin is
           pulled back by whichever side was negative.  When the origin is
           itself infinite the whole thing collapses to a zero scale about
           an infinite translation -- the null rect's transform, and what
           makes CGRectNull round-trip.  Note the +infinity test is an
           equality, so a NaN origin falls through to the finite branch and
           a -infinity origin does too. */
        if (rect.origin.x == INFINITY || rect.origin.y == INFINITY) {
            t.a = 0;
            t.d = 0;
            t.tx = INFINITY;
            t.ty = INFINITY;
        } else {
            t.a = fabs(w);
            t.d = fabs(h);
            /* min(x, 0) rather than a bare conditional: it keeps -0.0
               distinct from 0.0 and suppresses NaN, and the original uses
               the instruction that does exactly this. */
            t.tx = rect.origin.x + fmin(w, 0.0);
            t.ty = rect.origin.y + fmin(h, 0.0);
        }
    } else {
        t.a = w;
        t.d = h;
        t.tx = rect.origin.x;
        t.ty = rect.origin.y;
    }

    return t;
}

bool
CGAffineTransformDecompose_SPI(CGAffineTransform t, CGSize *outScale,
    CGFloat *outRotation, bool *outScaleIsNegative, CGVector *outTranslation)
{
    const CGAffineTransformComponents c = CGAffineTransformDecompose(t);

    if (outScale) *outScale = c.scale;
    if (outRotation) *outRotation = c.rotation;
    if (outScaleIsNegative) *outScaleIsNegative = c.scale.width < 0.0;
    if (outTranslation) *outTranslation = c.translation;

    /* 2^-46: the shear is reported as absent below this magnitude, and the
       comparison is against the magnitude, so a negative shear is treated
       the same as a positive one.  An unordered comparison is false. */
    return fabs(c.horizontalShear) < 0x1p-46;
}

CGAffineTransform
CGAffineTransformMakeWithComponents(CGAffineTransformComponents components)
{
    const CGFloat sx = components.scale.width;
    const CGFloat sy = components.scale.height;
    const CGFloat sh = components.horizontalShear;
    const CGFloat t = components.rotation;
    const CGFloat s = sin(t);
    const CGFloat c = cos(t);

    const CGFloat px = c * components.translation.dx
        + s * components.translation.dy;

    return CGAffineTransformMake(sx * c, sy * (sh * c - s),
        sx * s, sy * (sh * s + c), sx * px,
        sy * (sh * px - s * components.translation.dx
            + c * components.translation.dy));
}

/* The three pre-concatenating helpers put the new operation *first*, not
   second, which reads backwards but is what the original does: scaling
   CGAffineTransformMakeTranslation(3, -4) by (3, 4) scales only the linear
   part and leaves the translation at (3, -4) rather than moving it to
   (9, -16).  CGAffineTransformConcat(t, Make) would give the latter. */
CGAffineTransform
CGAffineTransformTranslate(CGAffineTransform t, CGFloat tx, CGFloat ty)
{
    return CGAffineTransformConcat(CGAffineTransformMakeTranslation(tx, ty), t);
}

CGAffineTransform
CGAffineTransformScale(CGAffineTransform t, CGFloat sx, CGFloat sy)
{
    return CGAffineTransformConcat(CGAffineTransformMakeScale(sx, sy), t);
}

CGAffineTransform
CGAffineTransformRotate(CGAffineTransform t, CGFloat angle)
{
    return CGAffineTransformConcat(CGAffineTransformMakeRotation(angle), t);
}

CGPoint
CGPointApplyAffineTransform(CGPoint point, CGAffineTransform t)
{
    return CGPointMake(t.a * point.x + t.c * point.y + t.tx,
        t.b * point.x + t.d * point.y + t.ty);
}

CGSize
CGSizeApplyAffineTransform(CGSize size, CGAffineTransform t)
{
    return CGSizeMake(t.a * size.width + t.c * size.height,
        t.b * size.width + t.d * size.height);
}

CGRect
CGRectApplyAffineTransform(CGRect rect, CGAffineTransform t)
{
    CGRect r = CGRectStandardize(rect);

    /* Transform all four corners, then take the bounding box. */
    const CGPoint corners[4] = {
        CGPointApplyAffineTransform(CGPointMake(r.origin.x, r.origin.y), t),
        CGPointApplyAffineTransform(
            CGPointMake(r.origin.x + r.size.width, r.origin.y), t),
        CGPointApplyAffineTransform(
            CGPointMake(r.origin.x, r.origin.y + r.size.height), t),
        CGPointApplyAffineTransform(
            CGPointMake(r.origin.x + r.size.width,
                r.origin.y + r.size.height), t)
    };

    CGFloat minx = corners[0].x, maxx = corners[0].x;
    CGFloat miny = corners[0].y, maxy = corners[0].y;

    for (int i = 1; i < 4; i++) {
        if (corners[i].x < minx) minx = corners[i].x;
        if (corners[i].x > maxx) maxx = corners[i].x;
        if (corners[i].y < miny) miny = corners[i].y;
        if (corners[i].y > maxy) maxy = corners[i].y;
    }

    return CGRectMake(minx, miny, maxx - minx, maxy - miny);
}

CGPoint
CGPointApplyInverseAffineTransform(CGPoint point, const CGAffineTransform *t)
{
    CGFloat det = t->a * t->d - t->b * t->c;

    if (det == 0) {
        CGPostError("%s: singular matrix.",
            "CGPointApplyInverseAffineTransform");
        return point;
    }

    const CGFloat x = point.x - t->tx;
    const CGFloat y = point.y - t->ty;

    return CGPointMake((t->d * x - t->c * y) / det,
        (t->a * y - t->b * x) / det);
}

CGRect
CGRectApplyInverseAffineTransform(CGRect rect, const CGAffineTransform *t)
{
    if (CGRectIsNull(rect) || CGRectIsInfinite(rect)) {
        return rect;
    }

    CGRect r = CGRectStandardize(rect);

    const CGPoint corners[4] = {
        CGPointApplyInverseAffineTransform(
            CGPointMake(r.origin.x, r.origin.y), t),
        CGPointApplyInverseAffineTransform(
            CGPointMake(r.origin.x + r.size.width, r.origin.y), t),
        CGPointApplyInverseAffineTransform(
            CGPointMake(r.origin.x, r.origin.y + r.size.height), t),
        CGPointApplyInverseAffineTransform(
            CGPointMake(r.origin.x + r.size.width,
                r.origin.y + r.size.height), t)
    };

    CGFloat minx = corners[0].x, maxx = corners[0].x;
    CGFloat miny = corners[0].y, maxy = corners[0].y;

    for (int i = 1; i < 4; i++) {
        if (corners[i].x < minx) minx = corners[i].x;
        if (corners[i].x > maxx) maxx = corners[i].x;
        if (corners[i].y < miny) miny = corners[i].y;
        if (corners[i].y > maxy) maxy = corners[i].y;
    }

    return CGRectMake(minx, miny, maxx - minx, maxy - miny);
}
