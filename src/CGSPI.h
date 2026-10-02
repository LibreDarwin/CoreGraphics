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
CG_EXTERN bool CGAffineTransformIsSingular(const CGAffineTransform *_Nonnull t);

/* True if the linear part is a pure scale, i.e. one of the two diagonals
   of the 2x2 block is entirely zero.  Rectilinear transforms are the ones
   whose inverse maps rectangles to rectangles rather than to parallelograms. */
CG_EXTERN bool CGAffineTransformIsRectilinear(const CGAffineTransform *_Nonnull t);

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
    CGSize *cg_nullable outScale, CGFloat *cg_nullable outRotation,
    bool *cg_nullable outScaleIsNegative, CGVector *cg_nullable outTranslation);

/* Apply the inverse of *t to point.

   Unlike CGAffineTransformInvert, which returns the identity transform on
   a singular matrix, this reports the error and returns point unchanged
   (the disassembly leaves the saved input registers in place on the
   singular path). */
CG_EXTERN CGPoint CGPointApplyInverseAffineTransform(CGPoint point,
    const CGAffineTransform *_Nonnull t);

/* Apply the inverse of *t to rect and return the bounding box of the
   result. */
CG_EXTERN CGRect CGRectApplyInverseAffineTransform(CGRect rect,
    const CGAffineTransform *_Nonnull t);

/* The color space SPI.

   These are all exported by Apple's CoreGraphics and are in the SDK's .tbd,
   so a binary linked against the real framework can call them, but the
   public CGColorSpace.h never declares them.  They are collected here.

   Every one of them takes a CGColorSpaceRef that may be NULL, and none of
   them crashes on NULL: Apple returns a zero, NULL or false result. */

#include "CGColorSpace.h"

/* Return the model a space paints in, which is the model of its base
   space.  For the three device spaces this is the space's own model; for a
   pattern space it is the base's model, or kCGColorSpaceModelUnknown when
   the pattern has no base. */
CG_EXTERN CGColorSpaceModel CGColorSpaceGetProcessColorModel(
    CGColorSpaceRef cg_nullable space) CG_PURE;

/* Return the space's "type", a finer-grained tag than the model: 0 for
   monochrome, 1 for RGB, 2 for CMYK, and 9 for a pattern space.  A pattern
   space reports 9 whatever its base says, so this is not the model with a
   different name. */
CG_EXTERN int CGColorSpaceGetType(CGColorSpaceRef cg_nullable space) CG_PURE;

/* Return the identifier of `space' in the built-in name table, or 0 if
   `space' is not one of the built-in named spaces.  The three device spaces
   are not in the table and report 0. */
CG_EXTERN int CGColorSpaceGetID(CGColorSpaceRef cg_nullable space) CG_PURE;

/* Return the built-in identifier for `name', or 0 if `name' is not one of
   the built-in names.  The comparison is exact: "kCGColorSpaceSRGB" has an
   identifier but "sRGB", "CGColorSpace sRGB" and the empty string do not. */
CG_EXTERN int CGColorSpaceIDFromName(CFStringRef cg_nullable name);

/* Return the built-in name for identifier `id', or NULL if there is none.
   The table runs 1 to 32; 0 and anything above 32 have no name. */
CG_EXTERN CFStringRef __nullable CGColorSpaceNameFromID(int id);

/* Return true if `space' carries no calibration.  This is false for the
   three device spaces, which are treated as calibrated. */
CG_EXTERN bool CGColorSpaceIsUncalibrated(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return true if `space' is ICC-compatible.  False for every space this
   step implements. */
CG_EXTERN bool CGColorSpaceIsICCCompatible(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return true if `space' is compatible with PostScript level 2.  False for
   every space this step implements. */
CG_EXTERN bool CGColorSpaceIsPSLevel2Compatible(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return true if the two spaces are the same space.  For a pattern space
   this compares the base, not the wrapper. */
CG_EXTERN bool CGColorSpaceEqualToColorSpace(CGColorSpaceRef cg_nullable space1,
    CGColorSpaceRef cg_nullable space2) CG_PURE;

/* As CGColorSpaceEqualToColorSpace, but ignoring differences in output
   range. */
CG_EXTERN bool CGColorSpaceEqualToColorSpaceIgnoringRange(
    CGColorSpaceRef cg_nullable space1, CGColorSpaceRef cg_nullable space2)
    CG_PURE;

/* Return the rendering intent of `space', which is 0 for the device and
   pattern spaces. */
CG_EXTERN int CGColorSpaceGetRenderingIntent(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return true if `space' ignores the rendering intent, i.e. the intent has
   no effect on it.  True for every space this step implements. */
CG_EXTERN bool CGColorSpaceIgnoresIntent(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return true if `space' uses the ITU-R BT.2100 transfer functions.  False
   for every space this step implements. */
CG_EXTERN bool CGColorSpaceUsesITUR_2100TF(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return the space to use in place of `space' for output, or NULL if there
   is none. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceGetAlternateColorSpace(
    CGColorSpaceRef cg_nullable space) CG_PURE;

/* Return the list of names `space' is known by, or NULL.  Apple only
   supports this for DeviceN spaces and aborts the process on anything
   else, so this is NULL everywhere it can be called safely. */
CG_EXTERN CFArrayRef __nullable CGColorSpaceGetNames(
    CGColorSpaceRef cg_nullable space);

/* Return `space's' identifier string, or NULL if it has none. */
CG_EXTERN const char * __nullable CGColorSpaceGetIdentifier(
    CGColorSpaceRef cg_nullable space);

/* Return `space's' ICC profile MD5 digest, or NULL if it has no profile.
   The device and pattern spaces have no profile. */
CG_EXTERN CFDataRef __nullable CGColorSpaceGetMD5Digest(
    CGColorSpaceRef cg_nullable space);

/* Not declared here, because none of them can be given a signature that is
   both right and checkable against Apple for the spaces this step builds:
   CGColorSpaceGetDescriptor faults on a pattern space,
   CGColorSpaceGetConversionMatrix faults on every space,
   CGColorSpaceGetTintTransform aborts, and the argument lists of
   CGColorSpaceGetCICPInfo and CGColorSpaceGetHeadroomInfo could not be
   confirmed from the disassembly.  They stay unexported until a later step
   has an indexed, DeviceN or ICC-backed space to answer for. */

CG_END_DECLS

#endif /* CGSPI_H_ */
