/* CoreGraphics - CGGeometry.c
   Copyright (C) 2026, LibreDarwin

   Implementation of the geometric value types and the CGRect primitives.

   Behaviour notes, established from the arm64e disassembly in
   local/disasm:

     - The default tolerance used by the *NearlyEqual* family is 2^-26
       (0x1p-26).  It is materialised at runtime into a lazily
       initialised global, and the comparisons are `fabs(a - b) <= tol`.

     - CGRectIsNull is a single 4-double equality test against a constant
       whose sentinel is +infinity.

     - CGRectIsEmpty tests a pair of components for equality with zero and
       ORs the result with CGRectIsNull.

     - CGRectIsIntegral round-trips each component through a signed
       32-bit integer conversion and compares.

   See the comments on the CGRectNull and CGRectInfinite definitions below
   for how their values were established. */

#include "CGGeometry.h"
#include "CGSPI.h"
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFString.h>
#include <math.h>
#include <string.h>

#include "CGInternal.h"

const CGPoint CGPointZero = { 0, 0 };
const CGSize CGSizeZero = { 0, 0 };
const CGRect CGRectZero = { { 0, 0 }, { 0, 0 } };

/* The two sentinels could not be read out of the original binary: the
   extraction contains only __TEXT, and these globals live in a data
   segment.  Their values below were settled differentially instead, by
   calling the real CGRectNull and CGRectInfinite and printing the raw
   doubles.  CGRectNull is an infinite origin with zero size -- not
   CGFLOAT_MAX, which was the natural guess and is wrong.  CGRectInfinite
   is CGFLOAT_MAX in size but only half of it in origin: -CGFLOAT_MAX/2,
   not -CGFLOAT_MAX, which was the other natural guess and is also wrong. */
const CGRect CGRectNull = { { INFINITY, INFINITY }, { 0, 0 } };
const CGRect CGRectInfinite = {
    { -CGFLOAT_MAX / 2, -CGFLOAT_MAX / 2 },
    { CGFLOAT_MAX, CGFLOAT_MAX }
};

/* The default tolerance for the NearlyEqual family: 2^-26. */
#define CG_DEFAULT_TOLERANCE 0x1p-26

/* No geometry accessor ever hands back a negative zero.  The original
   arrives at each result through an operation that collapses -0 to +0,
   and the sign of zero is part of the observable ABI, so a rect built
   from -0 origins (CGRectMake(-0.0, ...)) must not round-trip it.  Adding
   +0.0 is that collapse: it is the identity on every other value. */
static inline CGFloat
__CGFloatNoNegZero(CGFloat value)
{
    return value + 0.0;
}

static bool
__CGFloatNearlyEqual(CGFloat a, CGFloat b, CGFloat tolerance)
{
    return fabs(a - b) <= tolerance;
}

/* The rect accessors report the standardized extent, so a rect with a
   negative size answers as if it had been passed through
   CGRectStandardize: CGRectGetMinX/CGRectMake(5, 5, -4, -4) is 1, not 5,
   and CGRectGetWidth is 4, not -4.  The originals branch on the sign of
   the size rather than normalizing up front, which also avoids the
   overflow that x + width would hit on the largest finite rect. */

CGFloat
CGRectGetMinX(CGRect rect)
{
    return __CGFloatNoNegZero(rect.size.width < 0 ? rect.origin.x + rect.size.width
        : rect.origin.x);
}

CGFloat
CGRectGetMidX(CGRect rect)
{
    return CGRectGetMinX(rect) + CGRectGetWidth(rect) / 2;
}

CGFloat
CGRectGetMaxX(CGRect rect)
{
    return __CGFloatNoNegZero(rect.size.width < 0 ? rect.origin.x
        : rect.origin.x + rect.size.width);
}

CGFloat
CGRectGetMinY(CGRect rect)
{
    return __CGFloatNoNegZero(rect.size.height < 0 ? rect.origin.y + rect.size.height
        : rect.origin.y);
}

CGFloat
CGRectGetMidY(CGRect rect)
{
    return CGRectGetMinY(rect) + CGRectGetHeight(rect) / 2;
}

CGFloat
CGRectGetMaxY(CGRect rect)
{
    return __CGFloatNoNegZero(rect.size.height < 0 ? rect.origin.y
        : rect.origin.y + rect.size.height);
}

CGFloat
CGRectGetWidth(CGRect rect)
{
    /* fabs folds -0 to +0 and leaves every other magnitude alone. */
    return fabs(rect.size.width);
}

CGFloat
CGRectGetHeight(CGRect rect)
{
    return fabs(rect.size.height);
}

bool
CGPointEqualToPoint(CGPoint point1, CGPoint point2)
{
    return point1.x == point2.x && point1.y == point2.y;
}

bool
CGPointNearlyEqualToPointWithTolerance(CGPoint point1, CGPoint point2,
    CGFloat tolerance)
{
    return __CGFloatNearlyEqual(point1.x, point2.x, tolerance) &&
        __CGFloatNearlyEqual(point1.y, point2.y, tolerance);
}

bool
CGPointNearlyEqualToPoint(CGPoint point1, CGPoint point2)
{
    return CGPointNearlyEqualToPointWithTolerance(point1, point2,
        CG_DEFAULT_TOLERANCE);
}

bool
CGSizeEqualToSize(CGSize size1, CGSize size2)
{
    return size1.width == size2.width && size1.height == size2.height;
}

bool
CGSizeNearlyEqualToSizeWithTolerance(CGSize size1, CGSize size2,
    CGFloat tolerance)
{
    return __CGFloatNearlyEqual(size1.width, size2.width, tolerance) &&
        __CGFloatNearlyEqual(size1.height, size2.height, tolerance);
}

bool
CGSizeNearlyEqualToSize(CGSize size1, CGSize size2)
{
    return CGSizeNearlyEqualToSizeWithTolerance(size1, size2,
        CG_DEFAULT_TOLERANCE);
}

bool
CGVectorNearlyEqualToVectorWithTolerance(CGVector vector1, CGVector vector2,
    CGFloat tolerance)
{
    return __CGFloatNearlyEqual(vector1.dx, vector2.dx, tolerance) &&
        __CGFloatNearlyEqual(vector1.dy, vector2.dy, tolerance);
}

bool
CGVectorNearlyEqualToVector(CGVector vector1, CGVector vector2)
{
    return CGVectorNearlyEqualToVectorWithTolerance(vector1, vector2,
        CG_DEFAULT_TOLERANCE);
}

bool
CGRectEqualToRect(CGRect rect1, CGRect rect2)
{
    /* Null is a single distinguished value rather than a set of bit patterns,
       so any two null rects are equal however their other components are
       spelled.  {{inf,inf},{1,1}} and {{inf,0},{0,0}} are both null, hence
       equal to each other and to CGRectNull, even though comparing all four
       components would call them different.  Once neither side is null the
       comparison is exact. */
    if (CGRectIsNull(rect1) || CGRectIsNull(rect2)) {
        return CGRectIsNull(rect1) && CGRectIsNull(rect2);
    }

    return rect1.origin.x == rect2.origin.x &&
           rect1.origin.y == rect2.origin.y &&
           rect1.size.width == rect2.size.width &&
           rect1.size.height == rect2.size.height;
}

bool
CGRectNearlyEqualToRectWithTolerance(CGRect rect1, CGRect rect2,
    CGFloat tolerance)
{
    return __CGFloatNearlyEqual(rect1.origin.x, rect2.origin.x, tolerance) &&
           __CGFloatNearlyEqual(rect1.origin.y, rect2.origin.y, tolerance) &&
           __CGFloatNearlyEqual(rect1.size.width, rect2.size.width, tolerance) &&
           __CGFloatNearlyEqual(rect1.size.height, rect2.size.height, tolerance);
}

bool
CGRectNearlyEqualToRect(CGRect rect1, CGRect rect2)
{
    return CGRectNearlyEqualToRectWithTolerance(rect1, rect2,
        CG_DEFAULT_TOLERANCE);
}

CGRect
CGRectStandardize(CGRect rect)
{
    /* A rect with an infinite origin is null, and standardizing it does not
       repair it: it collapses to CGRectNull outright, with *both* origin
       components becoming infinite even when only one of them was.  So
       CGRectStandardize({{inf, 0}, {0, 0}}) is CGRectNull, not {{inf,0},{0,0}}. */
    if (CGRectIsNull(rect)) {
        return CGRectNull;
    }
    if (rect.size.width < 0) {
        rect.origin.x += rect.size.width;
        rect.size.width = -rect.size.width;
    }
    if (rect.size.height < 0) {
        rect.origin.y += rect.size.height;
        rect.size.height = -rect.size.height;
    }
    /* -0 is not "less than zero", so the branches above leave it alone, but
       the original still reports it as +0 in both the origin and the size. */
    rect.origin.x = __CGFloatNoNegZero(rect.origin.x);
    rect.origin.y = __CGFloatNoNegZero(rect.origin.y);
    rect.size.width = __CGFloatNoNegZero(rect.size.width);
    rect.size.height = __CGFloatNoNegZero(rect.size.height);
    return rect;
}

bool
CGRectIsEmpty(CGRect rect)
{
    /* A negative size is not empty: it is a rect drawn the other way, and
       it standardizes to a non-empty one.  CGRectIsEmpty(CGRectMake(5, 5,
       -4, -4)) is false.  A null rect is always empty, whatever its size. */
    return CGRectIsNull(rect) || rect.size.width == 0 || rect.size.height == 0;
}

bool
CGRectIsNull(CGRect rect)
{
    /* Only the origin matters.  A rect with an infinite origin is null even
       when its size is finite or itself infinite, and the size is never
       consulted: CGRectIsNull({{inf, 0}, {0, 0}}) and
       CGRectIsNull({{inf, inf}, {inf, inf}}) are both true. */
    return rect.origin.x == INFINITY || rect.origin.y == INFINITY;
}

bool
CGRectIsInfinite(CGRect rect)
{
    /* The exact sentinel and nothing else.  A merely enormous rect built from
       +/-CGFLOAT_MAX is not infinite, because the components do not match. */
    return CGRectEqualToRect(rect, CGRectInfinite);
}

bool
CGRectIsIntegral(CGRect rect)
{
    const CGFloat x = rect.origin.x;
    const CGFloat y = rect.origin.y;
    const CGFloat w = rect.size.width;
    const CGFloat h = rect.size.height;

    /* Null is integral, even though its infinities are not integers. */
    if (CGRectIsNull(rect)) {
        return true;
    }

    /* The range is 32-bit, not merely "has no fractional part": 2^30 is
       integral and 2^31 is not, even though both are whole numbers. */
    return x >= -2147483648.0 && x <= 2147483647.0 && x == floor(x) &&
           y >= -2147483648.0 && y <= 2147483647.0 && y == floor(y) &&
           w >= -2147483648.0 && w <= 2147483647.0 && w == floor(w) &&
           h >= -2147483648.0 && h <= 2147483647.0 && h == floor(h);
}

CGRect
CGRectInset(CGRect rect, CGFloat dx, CGFloat dy)
{
    CGRect r;

    /* A null rect is returned untouched, keeping its original origin and
       even a non-zero size: CGRectInset({{inf,inf}, {1,1}}, 1, 1) is
       {{inf,inf}, {1,1}}, not CGRectNull. */
    if (CGRectIsNull(rect)) {
        return rect;
    }

    r = CGRectStandardize(rect);

    r.origin.x += dx;
    r.origin.y += dy;
    r.size.width -= 2 * dx;
    r.size.height -= 2 * dy;

    /* An inset that consumes the whole extent collapses to CGRectNull, but
       one that lands exactly on zero is a legitimate degenerate rect:
       CGRectInset(CGRectMake(0, 0, 1, 1), 0.5, 0) is {(0.5, 0), (0, 1)},
       not CGRectNull. */
    if (r.size.width < 0 || r.size.height < 0) {
        return CGRectNull;
    }

    return r;
}

CGRect
CGRectIntegral(CGRect rect)
{
    CGRect r = CGRectStandardize(rect);

    const CGFloat minx = floor(CGRectGetMinX(r));
    const CGFloat miny = floor(CGRectGetMinY(r));
    const CGFloat maxx = ceil(CGRectGetMaxX(r));
    const CGFloat maxy = ceil(CGRectGetMaxY(r));

    return CGRectMake(minx, miny, maxx - minx, maxy - miny);
}

CGRect
CGRectUnion(CGRect r1, CGRect r2)
{
    /* Null absorbs, in argument order.  Unioning a null into a rect yields
       the other rect unchanged, so no arithmetic on the infinite origin ever
       happens.  CGRectUnion(CGRectNull, {{inf,0},{0,0}}) is {{inf,0},{0,0}},
       and swapping the operands swaps which one survives. */
    if (CGRectIsNull(r1)) {
        return r2;
    }
    if (CGRectIsNull(r2)) {
        return r1;
    }

    /* The extents come from the accessors, which form maxX as
       origin.x + max(width, 0) rather than as a separately stored corner.
       That is what keeps an enormous union finite: unioning
       {{-DBL_MAX,-DBL_MAX}, {DBL_MAX,DBL_MAX}} with {{-1,-1},{1,1}} has a
       max X of 0, not DBL_MAX, so the width stays DBL_MAX instead of
       overflowing to infinity. */
    const CGFloat minx = fmin(CGRectGetMinX(r1), CGRectGetMinX(r2));
    const CGFloat miny = fmin(CGRectGetMinY(r1), CGRectGetMinY(r2));
    const CGFloat maxx = fmax(CGRectGetMaxX(r1), CGRectGetMaxX(r2));
    const CGFloat maxy = fmax(CGRectGetMaxY(r1), CGRectGetMaxY(r2));

    return CGRectMake(minx, miny, maxx - minx, maxy - miny);
}

CGRect
CGRectIntersection(CGRect r1, CGRect r2)
{
    /* Overlap is computed directly from the extents, independently of
       CGRectIntersectsRect: rects that merely touch still intersect, and the
       overlap itself may be degenerate.  CGRectIntersection(CGRectZero,
       CGRectZero) is CGRectZero, and touching rects give a zero-width
       overlap at the contact point.  Only an extent that comes out negative
       -- genuinely disjoint -- yields CGRectNull. */
    const CGFloat minx = fmax(CGRectGetMinX(r1), CGRectGetMinX(r2));
    const CGFloat miny = fmax(CGRectGetMinY(r1), CGRectGetMinY(r2));
    const CGFloat maxx = fmin(CGRectGetMaxX(r1), CGRectGetMaxX(r2));
    const CGFloat maxy = fmin(CGRectGetMaxY(r1), CGRectGetMaxY(r2));

    if (maxx < minx || maxy < miny) {
        return CGRectNull;
    }

    return CGRectMake(minx, miny, maxx - minx, maxy - miny);
}

CGRect
CGRectOffset(CGRect rect, CGFloat dx, CGFloat dy)
{
    CGRect r;

    /* Null passes through unchanged, keeping its size: offsetting
       {{inf,inf}, {inf,inf}} leaves it as {{inf,inf}, {inf,inf}} rather
       than collapsing it. */
    if (CGRectIsNull(rect)) {
        return rect;
    }

    r = CGRectStandardize(rect);

    r.origin.x += dx;
    r.origin.y += dy;

    return r;
}

void
CGRectDivide(CGRect rect, CGRect *slice, CGRect *remainder, CGFloat amount,
    CGRectEdge edge)
{
    CGRect r = CGRectStandardize(rect);

    const CGFloat minx = r.origin.x;
    const CGFloat miny = r.origin.y;
    const CGFloat w = r.size.width;
    const CGFloat h = r.size.height;

    /* Each edge takes a strip off the side it names, and `amount` is clamped
       into the extent being split so it can never go negative or overhang:
         - min X / max X split the width, min Y / max Y split the height;
         - a strip taken from a far edge is placed at the far end, so
           CGRectDivide({1,2,3,4}, ..., 1.5, CGRectMaxXEdge) cuts a 1.5-wide
           slice off the right, leaving x = 2.5 rather than 1.5.
       A degenerate input divides into two copies of itself. */
    CGFloat a;

    switch (edge) {
    case CGRectMinXEdge:
    case CGRectMaxXEdge:
        a = fmin(fmax(amount, 0), w);
        break;
    case CGRectMinYEdge:
    case CGRectMaxYEdge:
        a = fmin(fmax(amount, 0), h);
        break;
    default:
        *slice = CGRectNull;
        *remainder = CGRectNull;
        return;
    }

    /* The sizes are assigned from `a` and the leftover extent directly.  The
       leftover is never recovered as max - cut, and a far strip's origin is
       placed as min + (extent - a) rather than max - a: for an enormous rect
       the two spellings differ in the last bit, because (extent - a) rounds
       before it is added, so {{-DBL_MAX,-DBL_MAX},{DBL_MAX,DBL_MAX}} divided
       by 0.5 on the max X edge starts its slice at exactly 0 rather than
       -0.5. */
    switch (edge) {
    case CGRectMinXEdge:
        *slice = CGRectMake(minx, miny, a, h);
        *remainder = CGRectMake(minx + a, miny, w - a, h);
        break;
    case CGRectMaxXEdge:
        *slice = CGRectMake(minx + (w - a), miny, a, h);
        *remainder = CGRectMake(minx, miny, w - a, h);
        break;
    case CGRectMinYEdge:
        *slice = CGRectMake(minx, miny, w, a);
        *remainder = CGRectMake(minx, miny + a, w, h - a);
        break;
    case CGRectMaxYEdge:
        *slice = CGRectMake(minx, miny + (h - a), w, a);
        *remainder = CGRectMake(minx, miny, w, h - a);
        break;
    }
}

bool
CGRectContainsPoint(CGRect rect, CGPoint point)
{
    /* Empty rects contain nothing, and since every null rect is empty that
       covers the null case too: no point is inside CGRectNull, even though
       the point is finite.  The interval is half-open, so a point on the
       maximum edge is outside and one on the minimum edge is inside. */
    if (CGRectIsEmpty(rect)) {
        return false;
    }

    return point.x >= CGRectGetMinX(rect) && point.x < CGRectGetMaxX(rect) &&
        point.y >= CGRectGetMinY(rect) && point.y < CGRectGetMaxY(rect);
}

bool
CGRectContainsRect(CGRect rect1, CGRect rect2)
{
    /* No empty-rect shortcut: every rect contains itself, so
       CGRectContainsRect(CGRectZero, CGRectZero) is true, while a non-empty
       rect does not contain an empty one parked outside it.  The extent
       comparison below already produces both answers. */
    return CGRectGetMinX(rect1) <= CGRectGetMinX(rect2) &&
        CGRectGetMinY(rect1) <= CGRectGetMinY(rect2) &&
        CGRectGetMaxX(rect1) >= CGRectGetMaxX(rect2) &&
        CGRectGetMaxY(rect1) >= CGRectGetMaxY(rect2);
}

/* Two spans on a line, [a0,a1] and [b0,b1], overlap when neither is strictly
   past the other.  The degenerate cases are what make this more than a pair of
   strict comparisons: a collapsed span still meets a span it sits inside, but
   it only meets another collapsed span if the two are the same point.  So
   CGRectZero intersects {{0,0},{1,1}}, and two distinct points do not
   intersect. */
static bool
__CGRectSpansIntersect(CGFloat a0, CGFloat a1, CGFloat b0, CGFloat b1)
{
    if (a0 == a1) {
        if (b0 == b1) {
            return a0 == b0;
        }
        return b0 <= a0 && a0 < b1;
    }
    if (b0 == b1) {
        return a0 <= b0 && b0 < a1;
    }
    return a0 < b1 && b0 < a1;
}

bool
CGRectIntersectsRect(CGRect rect1, CGRect rect2)
{
    return __CGRectSpansIntersect(CGRectGetMinX(rect1), CGRectGetMaxX(rect1),
               CGRectGetMinX(rect2), CGRectGetMaxX(rect2)) &&
           __CGRectSpansIntersect(CGRectGetMinY(rect1), CGRectGetMaxY(rect1),
               CGRectGetMinY(rect2), CGRectGetMaxY(rect2));
}

CG_PRIVATE CGRect
CGRectUprightBoundsForRotation(CGRect rect, CGRectEdge edge)
{
    CGRect r = CGRectStandardize(rect);

    CGFloat x = 0, y = 0, w = 0, h = 0;

    switch (edge) {
    case CGRectMinXEdge:
    case CGRectMaxXEdge:
        x = r.origin.x;
        y = r.origin.y;
        w = r.size.width;
        h = r.size.height;
        break;
    case CGRectMinYEdge:
    case CGRectMaxYEdge:
        x = r.origin.x;
        y = r.origin.y;
        w = r.size.height;
        h = r.size.width;
        break;
    }

    return CGRectMake(x, y, w, h);
}

/*** Persistent representations. ***

   Keyed by the CFStrings "X", "Y", "Width" and "Height".  The key strings
   sit 32 bytes apart in the string table, which is how the four names were
   attributed from the disassembly; Point and CGRect share "X"/"Y", CGSize
   uses "Width"/"Height", and CGRect uses all four in that order.

   Values are CFNumbers of type kCFNumberCGFloatType (13).  Reading accepts
   that type first and falls back to kCFNumberFloatType (12), widening the
   single to a double.  The composite readers short-circuit, so a missing
   first component leaves the out-parameter partially written.

   The keys are constant CFStrings built with CFSTR.  In the binary they sit
   32 bytes apart, which is the size of a __kCFConstantStringClassReference,
   and the dictionary really does hold CFStrings rather than C strings --
   a C string used as a key with the kCFType callbacks does not work. */

static void
_add_number_to_dict(CFMutableDictionaryRef dict, CFStringRef key,
    CGFloat value)
{
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberCGFloatType, &value);

    CFDictionarySetValue(dict, key, num);
    CFRelease(num);
}

static bool
_get_number_from_dict(CFDictionaryRef dict, CFStringRef key, CGFloat *out)
{
    CFNumberRef num = (CFNumberRef)CFDictionaryGetValue(dict, key);

    if (num == NULL) {
        return false;
    }

    CGFloat d = 0;

    if (CFNumberGetValue(num, kCFNumberCGFloatType, &d)) {
        *out = d;
        return true;
    }

    float f = 0;

    if (CFNumberGetValue(num, kCFNumberFloatType, &f)) {
        *out = (CGFloat)f;
        return true;
    }

    return false;
}

CFDictionaryRef
CGPointCreateDictionaryRepresentation(CGPoint point)
{
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    _add_number_to_dict(dict, CFSTR("X"), point.x);
    _add_number_to_dict(dict, CFSTR("Y"), point.y);

    return dict;
}

bool
CGPointMakeWithDictionaryRepresentation(CFDictionaryRef cg_nullable dict,
    CGPoint * cg_nullable point)
{
    if (dict == NULL || point == NULL) {
        return false;
    }

    if (!_get_number_from_dict(dict, CFSTR("X"), &point->x)) {
        return false;
    }

    return _get_number_from_dict(dict, CFSTR("Y"), &point->y);
}

CFDictionaryRef
CGSizeCreateDictionaryRepresentation(CGSize size)
{
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    _add_number_to_dict(dict, CFSTR("Width"), size.width);
    _add_number_to_dict(dict, CFSTR("Height"), size.height);

    return dict;
}

bool
CGSizeMakeWithDictionaryRepresentation(CFDictionaryRef cg_nullable dict,
    CGSize * cg_nullable size)
{
    if (dict == NULL || size == NULL) {
        return false;
    }

    if (!_get_number_from_dict(dict, CFSTR("Width"), &size->width)) {
        return false;
    }

    return _get_number_from_dict(dict, CFSTR("Height"), &size->height);
}

CFDictionaryRef
CGRectCreateDictionaryRepresentation(CGRect rect)
{
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    _add_number_to_dict(dict, CFSTR("X"), rect.origin.x);
    _add_number_to_dict(dict, CFSTR("Y"), rect.origin.y);
    _add_number_to_dict(dict, CFSTR("Width"), rect.size.width);
    _add_number_to_dict(dict, CFSTR("Height"), rect.size.height);

    return dict;
}

bool
CGRectMakeWithDictionaryRepresentation(CFDictionaryRef cg_nullable dict,
    CGRect * cg_nullable rect)
{
    if (dict == NULL || rect == NULL) {
        return false;
    }

    if (!_get_number_from_dict(dict, CFSTR("X"), &rect->origin.x)) {
        return false;
    }

    if (!_get_number_from_dict(dict, CFSTR("Y"), &rect->origin.y)) {
        return false;
    }

    if (!_get_number_from_dict(dict, CFSTR("Width"), &rect->size.width)) {
        return false;
    }

    return _get_number_from_dict(dict, CFSTR("Height"), &rect->size.height);
}
