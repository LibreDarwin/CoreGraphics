/* CoreGraphics - CGSPI.h
   Copyright (C) 2026, LibreDarwin
   SPDX-License-Identifier: BSD-3-Clause

   Declarations for CoreGraphics SPI that is present in the framework but
   is not part of the public SDK headers.  Most of it is exported and so
   part of the ABI we reproduce; the few private externals are marked
   CG_PRIVATE and are deliberately kept out of our export list.

   These symbols are exported by Apple's CoreGraphics and are therefore
   part of the ABI surface we reproduce, but they have no public
   declaration.  They are collected here rather than mixed into the public
   headers.

   Note on calling convention: both functions below take the
   CGAffineTransform by POINTER, not by value.  This is visible in the
   disassembly, where the transform is loaded from [x0] while the
   geometry argument arrives in vector registers.  The signatures here
   match that. */

#ifndef CGSPI_H_
#define CGSPI_H_

#include "CGAffineTransform.h"
#include "CGInternal.h"

CG_BEGIN_DECLS

/* The *NearlyEqual* family.  Each compares components with
   fabs(a - b) <= tolerance; the no-tolerance forms use CoreGraphics'
   default tolerance of 2^-26. */

CG_EXTERN bool CGPointNearlyEqualToPoint(CGPoint point1, CGPoint point2);

CG_EXTERN bool CGPointNearlyEqualToPointWithTolerance(CGPoint point1,
    CGPoint point2, CGFloat tolerance);

CG_EXTERN bool CGSizeNearlyEqualToSize(CGSize size1, CGSize size2);

CG_EXTERN bool CGSizeNearlyEqualToSizeWithTolerance(CGSize size1,
    CGSize size2, CGFloat tolerance);

CG_EXTERN bool CGVectorNearlyEqualToVector(CGVector vector1,
    CGVector vector2);

CG_EXTERN bool CGVectorNearlyEqualToVectorWithTolerance(CGVector vector1,
    CGVector vector2, CGFloat tolerance);

CG_EXTERN bool CGRectNearlyEqualToRect(CGRect rect1, CGRect rect2);

CG_EXTERN bool CGRectNearlyEqualToRectWithTolerance(CGRect rect1,
    CGRect rect2, CGFloat tolerance);

/* Return true if all of `rect's' coordinates and lengths are integers.
   Each component is round-tripped through a signed 32-bit conversion. */

CG_EXTERN bool CGRectIsIntegral(CGRect rect);

/* Given a rect and one of its four corners, return the minimal rect that
   contains the original rect rotated about that corner.

   This one is a private external in Apple's framework: it has an address
   and is used internally, but it is absent from the SDK's .tbd, so it must
   not appear in our dynamic symbol table either.  That also means it
   cannot be exercised by a test linked against the real framework. */

CG_EXTERN_PRIVATE CGRect CGRectUprightBoundsForRotation(CGRect rect,
    CGRectEdge edge);

/* True if the 2x2 part of *t has a zero determinant.  Like the inverse
   helpers below, this takes the transform by POINTER: the disassembly
   loads a and d from [x0] and [x0, #0x18], which is also what the arm64
   ABI does for a 48-byte struct passed by value, so the two forms are
   indistinguishable at the ABI level and the pointer form is used here
   for consistency with the rest of this header. */
CG_EXTERN bool CGAffineTransformIsSingular(const CGAffineTransform *t);

/* True if the linear part is a pure scale, i.e. one of the two diagonals
   of the 2x2 block is entirely zero.  Rectilinear transforms are the ones
   whose inverse maps rectangles to rectangles rather than to parallelograms. */
CG_EXTERN bool CGAffineTransformIsRectilinear(const CGAffineTransform *t);

/* Build the transform mapping the unit square onto `rect'.  The rect is
   passed in the four vector registers as a homogeneous float aggregate,
   and the result comes back through the hidden return pointer, so the
   by-value signature below is the one that matches. */
CG_EXTERN CGAffineTransform CGAffineTransformMakeWithRect(CGRect rect);

/* Decompose `t' through the out parameters instead of returning a
   CGAffineTransformComponents by value.  Every out parameter may be NULL.
   The return value reports whether the shear is negligible, i.e. whether
   fabs(horizontalShear) < 2^-46. */
CG_EXTERN bool CGAffineTransformDecompose_SPI(CGAffineTransform t,
    CGSize *outScale, CGFloat *outRotation, bool *outScaleIsNegative,
    CGVector *outTranslation);

/* Apply the inverse of *t to point.

   Unlike CGAffineTransformInvert, which returns the identity transform on
   a singular matrix, this reports the error and returns point unchanged
   (the disassembly leaves the saved input registers in place on the
   singular path). */
CG_EXTERN CGPoint CGPointApplyInverseAffineTransform(CGPoint point,
    const CGAffineTransform *t);

/* Apply the inverse of *t to rect and return the bounding box of the
   result. */
CG_EXTERN CGRect CGRectApplyInverseAffineTransform(CGRect rect,
    const CGAffineTransform *t);

CG_END_DECLS

#endif /* CGSPI_H_ */
