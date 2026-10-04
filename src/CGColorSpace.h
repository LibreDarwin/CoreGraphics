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
     - CreateWithID, and CreateWithName for all but seventeen names, since
       the rest resolve to embedded profiles.

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

/* Create a color space from one of its name constants, such as
   kCGColorSpaceSRGB or kCGColorSpaceDeviceRGB.  A NULL name, and the empty
   string, yield NULL.

   The match is an exact byte comparison of the name's UTF-8 contents: "sRGB",
   "CGColorSpace sRGB", "P3" and "kCGColorSpaceSRGB " are all rejected, as are
   the wrong case and any trailing text.  A mutable CFString resolves on its
   contents exactly as an immutable constant does.

   The names that resolve here are of two kinds.  Five need no profile of
   their own -- the three device names, kCGColorSpaceColoredPattern and
   kCGColorSpaceGenericLab -- and the eighteen described below are assembled
   from recovered constants.  Every other name yields NULL, because it resolves
   to a space carrying an embedded profile this step cannot emit yet.  That set
   is neither the identifier table nor a subset of it: of the 45 name constants
   reachable through the API, 40 resolve and 5 return NULL, and eight of the
   resolving names are absent from the table that CGColorSpaceIDFromName and
   CGColorSpaceNameFromID use.  So this accepts names that have no identifier,
   and refuses names that do.

   The three device names answer the process-lifetime singletons, identical to
   what CGColorSpaceCreateDeviceGray, ...DeviceRGB and ...DeviceCMYK return.
   kCGColorSpaceColoredPattern builds a fresh reference-counted pattern space
   with no base on every call.  kCGColorSpaceGenericLab is also a singleton,
   built on first use and immortal like the device spaces, and its profile is
   byte-for-byte what CGColorSpaceCreateLab with a D65 or D50 white point
   produces.

   Sixteen more names resolve to the eleven profiles that share a v4 shape:
   Display P3, the two ITU-R spaces, BT.2020 with an sRGB gamma, the Display
   P3 space with a 709 OETF, ROMM RGB, DCI P3 and ACES CG Linear, plus the
   three linearized spaces.  Their profiles are synthesised from a table of
   recovered constants rather than computed, and each is byte-for-byte what
   Apple hands back -- 536 to 600 bytes depending on the space.

   Four details of those sixteen are worth knowing.  Each is an immortal
   singleton, like the device spaces.  kCGColorSpaceExtendedDisplayP3,
   kCGColorSpaceExtendedITUR_2020 and the three extended linear names take the
   same profile bytes as their base and are told apart only by the
   extended-range flag, which is also why they report unequal to it.  And
   kCGColorSpaceITUR_709 is not called wide gamut despite primaries wider than
   sRGB's, so that answer comes from the measured triangle area rather than
   from a per-space flag.

   The linearized three are where the profile stops being able to answer.
   kCGColorSpaceLinearSRGB reports *not* wide gamut and
   kCGColorSpaceExtendedLinearSRGB reports wide gamut, from the very same
   bytes, so for an ICC space the extended flag has to be consulted alongside
   the colorants rather than after them.

   That override is confined to RGB, because a gray profile has no primaries
   to be wider than anything: kCGColorSpaceExtendedLinearGray reports an
   extended range and is nevertheless not wide gamut, so letting the flag stand
   on its own would call it one.

   Two more resolve to a profile of quite another shape:
   kCGColorSpaceLinearGray and kCGColorSpaceExtendedLinearGray share a single
   356-byte v2.1 profile, which is a monochrome/TRC one rather than a
   colorimetric RGB: four tags instead of ten or eleven, a legacy 'desc' and
   'text' where the v4 profiles carry 'mluc' records, no colorants at all, and
   one tone curve where they carry three.  The extended name takes the same 356
   bytes as its base and is told apart only by the extended-range flag, exactly
   as the three extended RGB aliases are, and neither is wide gamut -- the
   first of the two things a gray profile settles, since it has no primaries to
   be wider than anything.  It is not the device gray either: the profile is
   what distinguishes them.

   Every other name returns NULL for now; the remaining seventeen resolve to
   embedded profiles, which the rest of this file's constructors do not yet
   assemble. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreateWithName(
    CFStringRef __nullable name);

/* Create an indexed color space from a lookup table.  `lastIndex' is the
   largest valid index and must be at most 255; `colorTable' is an array of
   `(CGColorSpaceGetNumberOfComponents(baseSpace)) * (lastIndex + 1)` bytes,
   one entry per index, and is copied.  Each byte scales to the range of the
   corresponding component of the base.

   The result has one component -- the index -- and reports
   kCGColorSpaceModelIndexed however many the base has; `baseSpace' is
   retained and is what CGColorSpaceGetBaseColorSpace answers.  A NULL base,
   a base that is itself indexed or is a pattern space, and a `lastIndex'
   above 255 all yield NULL.

   The bound is on the table rather than on the base: 256 entries are accepted
   on a one-component gray base just as on a four-component CMYK one, and 257
   are refused on all of them. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreateIndexed(
    CGColorSpaceRef __nullable baseSpace,
    size_t lastIndex,
    const unsigned char * __nullable colorTable);

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

   The white point is kept even when the profile had to collapse it, because
   CGColorSpaceEqualToColorSpace compares it: two Lab spaces built from D65 and
   from D50 share the same 496 bytes and are still unequal.  The space
   CGColorSpaceCreateWithName reports for kCGColorSpaceGenericLab records no
   white point, which is what keeps it unequal to both of them even though it
   carries D65's profile.

   A NULL `whitePoint' is rejected and the function returns NULL; Apple faults
   on that instead, so there is no behaviour to copy. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreateLab(const CGFloat
    whitePoint[CG_NONNULL_ARRAY 3], const CGFloat blackPoint[__nullable 3],
    const CGFloat range[__nullable 4]);

/* Create a linearized version of `baseSpace', whose transfer function is
   replaced by the identity.  `baseSpace' must be a gray or RGB space that
   carries a profile; a device space, a pattern space, a Lab space and NULL
   all yield NULL.

   The profile is byte-for-byte identical to Apple's: 276 bytes for a gray
   space and 396 for RGB, with the base's white point and colorants copied
   verbatim, its black point and copyright dropped, the tone curve replaced by
   a one-entry 'curv', and the profile ID left all zeros as it is for Lab.
   The creation date is a constant, as for the calibrated spaces.

   Linearizing is not idempotent, because the description is built by
   appending " Linearized" to whatever description the base carries: doing it
   twice gives a 300-byte profile whose description says so.  `baseSpace' is
   not retained and the result reports no extended range, but an RGB result
   does report a wide gamut. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreateLinearized(
    CGColorSpaceRef cg_nullable baseSpace);

/* Create an extended-range version of `baseSpace', which must be a gray or
   RGB space that carries a profile, exactly as for
   CGColorSpaceCreateLinearized.

   This synthesises no profile at all: the result hands back its base's
   profile byte for byte, including the profile ID.  What distinguishes it is
   a flag beside the profile, which is why it is observable -- the result
   reports CGColorSpaceUsesExtendedRange, is reported unequal to its base, and
   for RGB also reports a wide gamut -- even though the two profiles are
   identical. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreateExtended(
    CGColorSpaceRef cg_nullable baseSpace);

/* Create an extended-range linearized version of `baseSpace'.  This
   synthesises exactly the profile CGColorSpaceCreateLinearized does and sets
   the extended flag as well, so the profile is identical and only the flags
   differ; the two spaces are reported unequal for that reason. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreateExtendedLinearized(
    CGColorSpaceRef cg_nullable baseSpace);

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

/* Return true if `space' has a gamut wider than sRGB.  True for a three
   component space that was built by CGColorSpaceCreateLinearized,
   CGColorSpaceCreateExtended or CGColorSpaceCreateExtendedLinearized, and
   false for every other space, including a calibrated RGB space. */
CG_EXTERN bool CGColorSpaceIsWideGamutRGB(CGColorSpaceRef cg_nullable space)
    CG_PURE;

/* Return true if `space' uses values outside 0..1.  True only for the spaces
   built by CGColorSpaceCreateExtended and
   CGColorSpaceCreateExtendedLinearized; a linearized space, which is
   extended in the everyday sense, reports false. */
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

/* Build a color space from ICC profile data the caller already holds.  The
   model and component count come from the profile's data colour space
   signature, and the bytes are kept verbatim, so CGColorSpaceCopyICCData
   returns the profile unchanged (short of trailing bytes past the end of the
   tag table and its furthest tag, which are dropped).

   Returns NULL when `data' is NULL, is too short to hold a header and a tag
   table, lacks the 'acsp' signature, declares version zero, carries a device
   class the profile's colour space does not accept, is missing a tag the
   colour space cannot be described without, has a tag pointing past the end of
   the data, or names a colour space Apple does not hand back a model for. */
CG_EXTERN CGColorSpaceRef __nullable CGColorSpaceCreateWithICCData(
    CFDataRef cg_nullable data);

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
