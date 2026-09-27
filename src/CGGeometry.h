/* CoreGraphics - CGGeometry.h
   Copyright (C) 2026, LibreDarwin

   Geometric value types and the CGRect query/manipulation primitives.

   ABI-compatible with Apple's CGGeometry.h.  The CFDictionary-based
   representation functions are deliberately absent here: they need
   CoreFoundation, which is not yet part of this tree.  They belong in
   CGGeometryDictionary.h alongside the rest of the CoreFoundation
   bridges, to be added when CoreFoundation lands. */

#ifndef CGGEOMETRY_H_
#define CGGEOMETRY_H_

#include "CGBase.h"
#include <CoreFoundation/CFDictionary.h>
#include <stdint.h>

CF_ASSUME_NONNULL_BEGIN

#ifndef CF_DEFINES_CG_TYPES
/* Points. */

struct CGPoint {
    CGFloat x;
    CGFloat y;
};
typedef struct CG_BOXABLE CGPoint CGPoint;

/* Sizes. */

struct CGSize {
    CGFloat width;
    CGFloat height;
};
typedef struct CG_BOXABLE CGSize CGSize;

/* Vectors.  A vector is a CGPoint used for displacement rather than
   position. */

#define CGVECTOR_DEFINED 1

struct CGVector {
    CGFloat dx;
    CGFloat dy;
};
typedef struct CG_BOXABLE CGVector CGVector;

/* Rectangles. */

struct CGRect {
    CGPoint origin;
    CGSize size;
};
typedef struct CG_BOXABLE CGRect CGRect;

/* Rectangle edges.

   Apple spells this `typedef CF_CLOSED_ENUM(uint32_t, CGRectEdge)'.  The
   CF_CLOSED_ENUM macro expands, in C, to an unnamed fixed-underlying-type
   enum, which requires reproducing CoreFoundation's macro indirection.
   A plain enum typedef is used instead: it has the same 4-byte
   representation and the same enumerator values, so the ABI is
   unchanged, and it needs no CF dependency. */

typedef enum CGRectEdge {
    CGRectMinXEdge, CGRectMinYEdge, CGRectMaxXEdge, CGRectMaxYEdge
} CGRectEdge;

#endif /* CF_DEFINES_CG_TYPES */

/* The "zero" point -- equivalent to CGPointMake(0, 0). */

CG_EXTERN const CGPoint CGPointZero;

/* The "zero" size -- equivalent to CGSizeMake(0, 0). */

CG_EXTERN const CGSize CGSizeZero;

/* The "zero" rectangle -- equivalent to CGRectMake(0, 0, 0, 0). */

CG_EXTERN const CGRect CGRectZero;

/* The "empty" rect.  This is the rectangle returned when, for example, we
   intersect two disjoint rectangles.  Note that the null rect is not the
   same as the zero rect. */

CG_EXTERN const CGRect CGRectNull;

/* The infinite rectangle. */

CG_EXTERN const CGRect CGRectInfinite;

/* Make a point from `(x, y)'. */

CG_INLINE CGPoint CGPointMake(CGFloat x, CGFloat y);

/* Make a size from `(width, height)'. */

CG_INLINE CGSize CGSizeMake(CGFloat width, CGFloat height);

/* Make a vector from `(dx, dy)'. */

CG_INLINE CGVector CGVectorMake(CGFloat dx, CGFloat dy);

/* Make a rect from `(x, y; width, height)'. */

CG_INLINE CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat width,
  CGFloat height);

/* Return the leftmost x-value of `rect'. */

CG_EXTERN CGFloat CGRectGetMinX(CGRect rect);

/* Return the midpoint x-value of `rect'. */

CG_EXTERN CGFloat CGRectGetMidX(CGRect rect);

/* Return the rightmost x-value of `rect'. */

CG_EXTERN CGFloat CGRectGetMaxX(CGRect rect);

/* Return the smallest y-value of `rect'. */

CG_EXTERN CGFloat CGRectGetMinY(CGRect rect);

/* Return the midpoint y-value of `rect'. */

CG_EXTERN CGFloat CGRectGetMidY(CGRect rect);

/* Return the largest y-value of `rect'. */

CG_EXTERN CGFloat CGRectGetMaxY(CGRect rect);

/* Return the width of `rect'. */

CG_EXTERN CGFloat CGRectGetWidth(CGRect rect);

/* Return the height of `rect'. */

CG_EXTERN CGFloat CGRectGetHeight(CGRect rect);

/* Return true if the two points are equal. */

CG_EXTERN bool CGPointEqualToPoint(CGPoint point1, CGPoint point2);

/* Return true if the two sizes are equal. */

CG_EXTERN bool CGSizeEqualToSize(CGSize size1, CGSize size2);

/* Return true if the two rects are equal. */

CG_EXTERN bool CGRectEqualToRect(CGRect rect1, CGRect rect2);

/* Standardize `rect'; that is, return a rect with a non-negative width and
   height. */

CG_EXTERN CGRect CGRectStandardize(CGRect rect) __attribute__((warn_unused_result));

/* Return true if `rect' is a null rectangle. */

CG_EXTERN bool CGRectIsNull(CGRect rect);

/* Return true if `rect' is the infinite rectangle. */

CG_EXTERN bool CGRectIsInfinite(CGRect rect);

/* Return true if `rect' is empty, that is if it has no width or no height. */

CG_EXTERN bool CGRectIsEmpty(CGRect rect);

/* Inset `rect' by `(dx, dy)'. */

CG_EXTERN CGRect CGRectInset(CGRect rect, CGFloat dx, CGFloat dy);

/* Return the smallest integral rectangle contained in `rect'. */

CG_EXTERN CGRect CGRectIntegral(CGRect rect) __attribute__((warn_unused_result));

/* Return the smallest rectangle containing both `r1' and `r2'. */

CG_EXTERN CGRect CGRectUnion(CGRect r1, CGRect r2) __attribute__((warn_unused_result));

/* Return the intersection of `r1' and `r2', or the null rect if they do
   not intersect. */

CG_EXTERN CGRect CGRectIntersection(CGRect r1, CGRect r2) __attribute__((warn_unused_result));

/*** Persistent representations. ***/

/* Return a dictionary representation of `point'.

   The dictionary is keyed by the CFStrings "X" and "Y", each holding a
   CFNumber of type kCFNumberCGFloatType. */

CG_EXTERN CFDictionaryRef CGPointCreateDictionaryRepresentation(CGPoint point);

/* Make a CGPoint from the contents of `dict' (presumably returned earlier
   from `CGPointCreateDictionaryRepresentation') and store the value in
   `point'. Returns true on success; false otherwise.

   Each component is read as a kCFNumberCGFloatType CFNumber, falling back
   to kCFNumberFloatType. A missing first component short-circuits, so the
   out parameter may be partially written. */

CG_EXTERN bool CGPointMakeWithDictionaryRepresentation(
    CFDictionaryRef cg_nullable dict, CGPoint * cg_nullable point);

/* Return a dictionary representation of `size', keyed by "Width" and
   "Height". */

CG_EXTERN CFDictionaryRef CGSizeCreateDictionaryRepresentation(CGSize size);

/* Make a CGSize from the contents of `dict' (presumably returned earlier
   from `CGSizeCreateDictionaryRepresentation') and store the value in
   `size'. Returns true on success; false otherwise. */

CG_EXTERN bool CGSizeMakeWithDictionaryRepresentation(
    CFDictionaryRef cg_nullable dict, CGSize * cg_nullable size);

/* Return a dictionary representation of `rect', keyed by "X", "Y",
   "Width" and "Height" in that order. */

CG_EXTERN CFDictionaryRef CGRectCreateDictionaryRepresentation(CGRect rect);

/* Make a CGRect from the contents of `dict' (presumably returned earlier
   from `CGRectCreateDictionaryRepresentation') and store the value in
   `rect'. Returns true on success; false otherwise. */

CG_EXTERN bool CGRectMakeWithDictionaryRepresentation(
    CFDictionaryRef cg_nullable dict, CGRect * cg_nullable rect);

/* Offset `rect' by `(dx, dy)'. */

CG_EXTERN CGRect CGRectOffset(CGRect rect, CGFloat dx, CGFloat dy) __attribute__((warn_unused_result));

/* Divide `rect' into two parts, separated by a line `amount' units from
   the specified `edge'. */

CG_EXTERN void CGRectDivide(CGRect rect, CGRect *slice,
    CGRect *remainder, CGFloat amount, CGRectEdge edge);

/* Return true if `point' is contained in `rect', false otherwise. */

CG_EXTERN bool CGRectContainsPoint(CGRect rect, CGPoint point);

/* Return true if `rect2' is contained in `rect1', false otherwise. */

CG_EXTERN bool CGRectContainsRect(CGRect rect1, CGRect rect2);

/* Return true if `rect1' and `rect2' intersect, false otherwise. */

CG_EXTERN bool CGRectIntersectsRect(CGRect rect1, CGRect rect2);

CF_ASSUME_NONNULL_END

#ifdef __cplusplus
extern "C" {
#endif

/* Inline implementations. */

CG_INLINE CGPoint
CGPointMake(CGFloat x, CGFloat y)
{
    CGPoint p;
    p.x = x;
    p.y = y;
    return p;
}

CG_INLINE CGSize
CGSizeMake(CGFloat width, CGFloat height)
{
    CGSize s;
    s.width = width;
    s.height = height;
    return s;
}

CG_INLINE CGVector
CGVectorMake(CGFloat dx, CGFloat dy)
{
    CGVector v;
    v.dx = dx;
    v.dy = dy;
    return v;
}

CG_INLINE CGRect
CGRectMake(CGFloat x, CGFloat y, CGFloat width, CGFloat height)
{
    CGRect r;
    r.origin.x = x;
    r.origin.y = y;
    r.size.width = width;
    r.size.height = height;
    return r;
}

#ifdef __cplusplus
}
#endif

#endif /* CGGEOMETRY_H_ */
