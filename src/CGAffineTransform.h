/* CoreGraphics - CGAffineTransform.h
   Copyright (C) 2026, LibreDarwin

   The 2D affine transform type and its constructors, decomposers and
   combinators.

   ABI-compatible with Apple's CGAffineTransform.h. */

#ifndef CGAFFINETRANSFORM_H_
#define CGAFFINETRANSFORM_H_

#include "CGGeometry.h"

CF_ASSUME_NONNULL_BEGIN

#ifndef CF_DEFINES_CG_TYPES
/* Affine transforms. */

struct CGAffineTransform {
    CGFloat a, b, c, d;
    CGFloat tx, ty;
};
typedef struct CGAffineTransform CGAffineTransform;
#endif /* CF_DEFINES_CG_TYPES */

#ifndef CF_DEFINES_CGAFFINETRANSFORMCOMPONENTS
/* A decomposed affine transform.  The parts compose, in this order, into
   the transform they were taken from:

       scale -> horizontalShear -> rotation -> translation

   Negative scale values mean the image is flipped on that axis. */

typedef struct CGAffineTransformComponents {
    CGSize scale;
    CGFloat horizontalShear;
    CGFloat rotation;
    CGVector translation;
} CGAffineTransformComponents;
#endif /* CF_DEFINES_CGAFFINETRANSFORMCOMPONENTS */

/* The identity transform. */

CG_EXTERN const CGAffineTransform CGAffineTransformIdentity;

/* Return true if `t' is the identity transform. */

CG_EXTERN bool CGAffineTransformIsIdentity(CGAffineTransform t) CG_PURE;

/* Return true if the two transforms are equal. */

CG_EXTERN bool CGAffineTransformEqualToTransform(CGAffineTransform t1,
    CGAffineTransform t2) CG_PURE;

/* Return a transform constructed from the six component values. */

CG_EXTERN CGAffineTransform CGAffineTransformMake(CGFloat a, CGFloat b,
    CGFloat c, CGFloat d, CGFloat tx, CGFloat ty);

/* Return a transform that scales by `(sx, sy)'. */

CG_EXTERN CGAffineTransform CGAffineTransformMakeScale(CGFloat sx,
    CGFloat sy);

/* Return a transform that translates by `(tx, ty)'. */

CG_EXTERN CGAffineTransform CGAffineTransformMakeTranslation(CGFloat tx,
    CGFloat ty);

/* Return a transform that rotates by `angle' radians. */

CG_EXTERN CGAffineTransform CGAffineTransformMakeRotation(CGFloat angle);

/* Concat two transforms.  `t1' is applied first, then `t2'. */

CG_EXTERN CGAffineTransform CGAffineTransformConcat(CGAffineTransform t1,
    CGAffineTransform t2);

/* Decompose `transform' into its scale, shear, rotation and translation
   parts, returned by value.  The product of the returned components is the
   input transform. */

CG_EXTERN CGAffineTransformComponents CGAffineTransformDecompose(
    CGAffineTransform transform) CG_PURE;

/* Return the inverse of `t'.  If `t' is singular the identity transform is
   returned and an error is logged. */

CG_EXTERN CGAffineTransform CGAffineTransformInvert(CGAffineTransform t);

/* Build a transform from scale * shear * rotation * translation. */

CG_EXTERN CGAffineTransform CGAffineTransformMakeWithComponents(
    CGAffineTransformComponents components) CG_PURE;

/* Translate an existing transform. */

CG_EXTERN CGAffineTransform CGAffineTransformTranslate(CGAffineTransform t,
    CGFloat tx, CGFloat ty);

/* Scale an existing transform. */

CG_EXTERN CGAffineTransform CGAffineTransformScale(CGAffineTransform t,
    CGFloat sx, CGFloat sy);

/* Rotate an existing transform. */

CG_EXTERN CGAffineTransform CGAffineTransformRotate(CGAffineTransform t,
    CGFloat angle);

/* Apply `t' to `point'. */

CG_EXTERN CGPoint CGPointApplyAffineTransform(CGPoint point,
    CGAffineTransform t) CG_PURE;

/* Apply `t' to `size', ignoring translation. */

CG_EXTERN CGSize CGSizeApplyAffineTransform(CGSize size,
    CGAffineTransform t) CG_PURE;

/* Apply `t' to `rect', returning the bounding box of the transformed
   rectangle. */

CG_EXTERN CGRect CGRectApplyAffineTransform(CGRect rect,
    CGAffineTransform t) CG_PURE;

CF_ASSUME_NONNULL_END

#endif /* CGAFFINETRANSFORM_H_ */
