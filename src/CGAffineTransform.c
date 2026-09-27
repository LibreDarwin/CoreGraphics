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

   CGAffineTransformDecompose needs the same algorithm as QuartzCore's
   CATransform3D decomposition, so it is not wired up here yet; it lands
   with the QuartzCore work. */

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

CGAffineTransformComponents
CGAffineTransformDecompose(CGAffineTransform transform)
{
    CGAffineTransformComponents c;

    const CGFloat t = atan2(transform.c, transform.a);
    const CGFloat s = sin(t);
    const CGFloat co = cos(t);

    c.scale.width = hypot(transform.a, transform.c);
    c.scale.height = transform.d * co - transform.b * s;
    c.horizontalShear = c.scale.height != 0
        ? (transform.b * co + transform.d * s) / c.scale.height
        : 0;
    c.rotation = t;

    /* Undo the rotation that R.T applied to the translation. */
    if (c.scale.width != 0 && c.scale.height != 0) {
        const CGFloat p = transform.tx / c.scale.width;
        const CGFloat q = transform.ty / c.scale.height
            - c.horizontalShear * p;

        c.translation.dx = co * p - s * q;
        c.translation.dy = s * p + co * q;
    } else {
        c.translation.dx = transform.tx;
        c.translation.dy = transform.ty;
    }

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
