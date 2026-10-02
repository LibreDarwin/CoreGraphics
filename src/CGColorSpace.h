/* CoreGraphics - CGColorSpace.h
   Copyright (C) 2026, LibreDarwin
   SPDX-License-Identifier: BSD-3-Clause

   Color spaces: the device spaces, pattern spaces, and the accessors that
   report a space's shape without reference to an ICC profile.

   ABI-compatible with Apple's CGColorSpace.h.

   Scope of this header.  Apple's CGColorSpace.h declares 35 functions and
   the framework exports 100 CGColorSpace* symbols in total (the extra 65
   have no public declaration and are declared in CGSPI.h).  This header
   covers the part of the surface that needs no ICC profile:

     - the three device spaces, which are immortal singletons,
     - pattern spaces, which are reference-counted and own their base,
     - the calibrated spaces -- gray, RGB and Lab -- which synthesise their
       ICC profile,
     - the accessors over those, and
     - the built-in name/ID table.

   Deliberately absent, because each of these either embeds a profile this
   step cannot reproduce or needs a byte-exact ICC encoder:

     - CreateLinearized, CreateExtended and their Extended variants,
     - CreateICCBased, CreateWithICCData, CreateWithICCProfile,
       CreateWithColorSyncProfile, CreateWithURL, CreatePlatformProfile,
     - CreateWithName and CreateWithID for the 32 built-in non-device names,
       which resolve to the embedded profiles,
     - CreateIndexed, which for a 256-entry table returns NULL in Apple.

   The three calibrated spaces do synthesise a profile, and the size varies
   with the shape of the request: 380 bytes for a calibrated gray, 416 to 528
   for a calibrated RGB depending on how many of its XYZ tags and tone curves
   coincide, and 496 or 516 for a Lab depending on whether the white point
   survives Apple's float check.  1992 is the size of a named space.  The
   model, component count and name are the cheap part of that contract;
   CopyICCData is the rest. */

#ifndef CGCOLORSPACE_H_
#define CGCOLORSPACE_H_

#include "CGBase.h"
#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFString.h>

CF_ASSUME_NONNULL_BEGIN

#ifndef CF_DEFINES_CG_COLORSPACE
/* A color space.  Objects are opaque; the only way to obtain one is
   through the Create functions below. */
typedef struct CGColorSpace *CGColorSpaceRef;
#endif /* CF_DEFINES_CG_COLORSPACE */

/* How a color space describes its components.  These values are part of
   the ABI: they are what CGColorSpaceGetModel reports. */
typedef CF_ENUM(int32_t, CGColorSpaceModel) {
    kCGColorSpaceModelUnknown = -1,
    kCGColorSpaceModelMonochrome,
    kCGColorSpaceModelRGB,
    kCGColorSpaceModelCMYK,
    kCGColorSpaceModelLab,
    kCGColorSpaceModelDeviceN,
    kCGColorSpaceModelIndexed,
    kCGColorSpaceModelPattern,
    kCGColorSpaceModelXYZ
};

/* Return the device RGB space.  The result is a process-lifetime
   singleton: every call returns the same pointer, and releasing it does not
   destroy it. */
CG_EXTERN CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void);

/* Return the device gray space.  As above, a process-lifetime singleton. */

CG_EXTERN CGColorSpaceRef CGColorSpaceCreateDeviceGray(void);

/* Return the device CMYK space.  As above, a process-lifetime singleton. */

CG_EXTERN CGColorSpaceRef CGColorSpaceCreateDeviceCMYK(void);

/* Create a pattern color space.  `baseSpace' is the underlying color space of
   the pattern color space.  For colored patterns, `baseSpace' should be NULL;
   for uncolored patterns, `baseSpace' specifies the color space of colors
   which will be painted through the pattern.

   The result is a new reference-counted object that retains its base, so a
   non-NULL `baseSpace' must be released by the caller. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreatePattern(
    CGColorSpaceRef __nullable baseSpace);

/* Create a calibrated gray color space carrying an ICC profile that this
   framework synthesises.  `whitePoint' is the diffuse white point in CIE
   1931 XYZ, `blackPoint' the diffuse black point, defaulting to zero when
   NULL, and `gamma' the gamma of the gray component.

   The profile is 380 bytes and is byte-for-byte identical to Apple's, which
   includes the profile ID: that field is the MD5 of the finished profile
   with its flags and ID fields zeroed, not an opaque commitment.  The
   gamma is quantised to 16.16 with round-to-nearest, so the caller's
   `gamma' is not recoverable from the profile. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreateCalibratedGray(const CGFloat
    whitePoint[CG_NONNULL_ARRAY 3], const CGFloat blackPoint[__nullable 3],
    CGFloat gamma);

/* Create a calibrated RGB color space carrying a synthesised ICC profile.
   `whitePoint' is the diffuse white point in CIE 1931 XYZ, `blackPoint' the
   diffuse black point, defaulting to zero when NULL, `gamma' the component
   gammas, defaulting to 1.0 when NULL, and `matrix' the 3x3 device matrix in
   row-major order, defaulting to the identity when NULL.

   The profile is byte-for-byte identical to Apple's, including the profile
   ID, which is again an MD5 rather than an opaque commitment.  Its length
   is not fixed: tags whose stored bytes are equal share one block, so the
   profile runs from 416 to 528 bytes.  The colorants are a Bradford
   chromatic adaptation of the matrix onto the white point, and every quantity
   in that computation is quantised, so `whitePoint', `blackPoint', `gamma'
   and `matrix' are not recoverable from the profile.

    A NULL `whitePoint' is rejected and the function returns NULL.  A white
   point of all zeros is accepted, and collapses the five colorant tags onto
   one shared block -- which also reorders the tag table, since Apple lists
   the tags that own a block ahead of the ones that share it. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreateCalibratedRGB(const CGFloat
    whitePoint[CG_NONNULL_ARRAY 3], const CGFloat blackPoint[__nullable 3],
    const CGFloat gamma[__nullable 3], const CGFloat matrix[__nullable 9]);

/* Create a CIE 1931 XYZ-based Lab color space carrying a synthesised ICC
   profile.  `whitePoint' is the diffuse white point, `blackPoint' the diffuse
   black point, defaulting to zero when NULL, and `range' the four
   encoding ranges, defaulting to zero when NULL.

   The profile is byte-for-byte identical to Apple's, with two exceptions a
   caller has to know about.  The profile ID is left all zeros rather than
   being an MD5, and the creation date is the local time of the call rather
   than a constant, so two calls seconds apart differ in one byte and an exact
   comparison has to mask header bytes 24-35.

   A white point is kept only when each of its three coordinates is a value a
   float can hold exactly; otherwise the whole tag is stored as zero.  D65 and
   D50 both fail that test, so a space built from either is byte-for-byte the
   generic Lab profile -- 496 bytes, identical to what
   CGColorSpaceCreateWithName(kCGColorSpaceGenericLab) produces -- while
   (0.5, 1, 1) survives and the profile grows to 516 bytes to give the black
   point a block of its own.  `range' is accepted and has no effect on the
   profile at all.

   A NULL `whitePoint' is rejected and the function returns NULL; Apple faults
   on that instead, so there is no behaviour to copy. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreateLab(const CGFloat
    whitePoint[CG_NONNULL_ARRAY 3], const CGFloat blackPoint[__nullable 3],
    const CGFloat range[__nullable 4]);

/* Return the CoreFoundation type identifier of a color space, which is 73.
   This is a constant in Apple's framework: every color space reports 73
   regardless of its model, because the identifier names the CF class
   rather than the space's shape. */
CG_EXTERN CFTypeID CGColorSpaceGetTypeID(void);

/* Return the space's model: kCGColorSpaceModelMonochrome for device gray,
   kCGColorSpaceModelRGB for device RGB, kCGColorSpaceModelCMYK for device
   CMYK, kCGColorSpaceModelLab for a Lab space, and kCGColorSpaceModelPattern
   for a pattern space. */
CG_EXTERN CGColorSpaceModel CGColorSpaceGetModel(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return the number of components a color in `space' has: 1 for device
   gray, 3 for device RGB, 4 for device CMYK, and for a pattern space the
   component count of its base, or 0 when it has no base. */
CG_EXTERN size_t CGColorSpaceGetNumberOfComponents(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return the space's name, or NULL if it has none.  The result is owned by
   the caller and must be released. */
CG_EXTERN CFStringRef __nullable CGColorSpaceCopyName(
    CGColorSpaceRef cg_nullable space);

/* Return the space's name without transferring ownership, or NULL if it has
   none.  The result is borrowed and must not be released; it stays valid
   for as long as the space does. */
CG_EXTERN CFStringRef __nullable CGColorSpaceGetName(
    CGColorSpaceRef cg_nullable space);

/* Return the base of a pattern space without transferring ownership, or
   NULL if `space' has no base.  The three device spaces have no base.
   A pattern space built on a device space reports that device space. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceGetBaseColorSpace(
    CGColorSpaceRef cg_nullable space) CG_PURE;

/* Return a +1 reference to the base of `space', or NULL if `space' has no
   base. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCopyBaseColorSpace(
    CGColorSpaceRef cg_nullable space);

/* Retain and release a color space.  The three device spaces are immortal:
   releasing one does not destroy it and a later Create returns the same
   pointer. */
CG_EXTERN CGColorSpaceRef CGColorSpaceRetain(CGColorSpaceRef space);

CG_EXTERN void CGColorSpaceRelease(CGColorSpaceRef space);

/* Return true if `space' can be used as the destination of a drawing
   operation.  True for the three device spaces; false for a pattern space,
   which describes a paint rather than a destination. */
CG_EXTERN bool CGColorSpaceSupportsOutput(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return true if `space' has a high dynamic range transfer function.  False
   for every space this step implements. */
CG_EXTERN bool CGColorSpaceIsHDR(CGColorSpaceRef cg_nullable space) CG_PURE;

/* Return true if `space' uses the Hybrid Log-Gamma transfer function.  False
   for every space this step implements. */
CG_EXTERN bool CGColorSpaceIsHLGBased(CGColorSpaceRef cg_nullable space) CG_PURE;

/* Return true if `space' uses the Perceptual Quantizer transfer function.
   False for every space this step implements. */
CG_EXTERN bool CGColorSpaceIsPQBased(CGColorSpaceRef cg_nullable space) CG_PURE;

/* Return true if `space' has a gamut wider than sRGB.  False for every
   space this step implements. */
CG_EXTERN bool CGColorSpaceIsWideGamutRGB(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return true if `space' uses values outside 0..1.  False for every space
   this step implements. */
CG_EXTERN bool CGColorSpaceUsesExtendedRange(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return a copy of `space's' ICC profile data, or NULL if `space' has no
   ICC profile.  The three device spaces and pattern spaces have none, so
   this always returns NULL for them. */
CG_EXTERN CFDataRef __nullable CGColorSpaceCopyICCData(
    CGColorSpaceRef cg_nullable space);

/* Deprecated spelling of CGColorSpaceCopyICCData, kept for the ABI. */

CG_EXTERN CFDataRef __nullable CGColorSpaceCopyICCProfile(
    CGColorSpaceRef cg_nullable space)
    __CG_DEPRECATED_WITH_MSG("Use CGColorSpaceCopyICCData");

/* Return the number of entries in the color table of `space'.  This is zero
   unless `space' is an indexed color space. */
CG_EXTERN size_t CGColorSpaceGetColorTableCount(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Copy the entries in the color table of `space' to `table' if `space' is an
   indexed color space; otherwise do nothing.  `table' should be at least as
   large as the number of entries in the color table; the data is in the same
   format as that passed to CGColorSpaceCreateIndexed. */
CG_EXTERN void CGColorSpaceGetColorTable(CGColorSpaceRef cg_nullable space,
    uint8_t * __nullable table);

CF_ASSUME_NONNULL_END

#endif /* CGCOLORSPACE_H_ */
