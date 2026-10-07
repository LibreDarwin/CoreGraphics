/* CoreGraphics - CGDataProvider.h
   Copyright (C) 2026, LibreDarwin
   SPDX-License-Identifier: BSD-3-Clause

   Data providers: the objects behind CGColorSpaceCreateICCBased and its
   siblings that hand raw color-profile bytes to a caller-supplied consumer.

   ABI-compatible with Apple's CGDataProvider.h for the surface below.

   Scope of this header.  Apple's CGDataProvider.h declares 16 functions and
   the framework exports more than 30 CGDataProvider* symbols in total.  This
   header covers only the CFData-backed provider, which is the one the
   profile-construction APIs in this tree consume:

     - the type identifier and the retain/release pair,
     - CGDataProviderCreateWithCFData, which wraps an existing CFData,
     - CGDataProviderCopyData, which hands the bytes back.

   Deliberately absent: the callbacks-based providers
   (CGDataProviderCreateWithData, ...WithCallbacks), the sequential-IO
   providers (...CreateSequentialData) and the direct-access providers
   (...DirectAccessData), each of which would need a callback marshalling
   layer this step does not use. */

#ifndef CGDATAPROVIDER_H_
#define CGDATAPROVIDER_H_

#include "CGBase.h"
#include <CoreFoundation/CFData.h>

CF_ASSUME_NONNULL_BEGIN

/* A data provider.  Objects are opaque; the only way to obtain one is
   through the Create functions below. */
typedef struct CGDataProvider *CGDataProviderRef;

/* Return the CF type identifier for CoreGraphics data providers. */
CG_EXTERN CFTypeID CGDataProviderGetTypeID(void);

/* Return a data provider backed by `data'.  The provider retains `data' and
   releases it when it is released in turn, so the caller need not keep the
   data alive.  Returns NULL when `data' is NULL. */
CG_EXTERN CGDataProviderRef __nullable CGDataProviderCreateWithCFData(
    CFDataRef cg_nullable data);

/* Return a copy of the data provided by `provider', or NULL if `provider' is
   NULL.  Caller is responsible for releasing this object. */
CG_EXTERN CFDataRef __nullable CGDataProviderCopyData(
    CGDataProviderRef cg_nullable provider);

/* Increment the reference count of `provider' and return it. */
CG_EXTERN CGDataProviderRef CGDataProviderRetain(
    CGDataProviderRef cg_nullable provider);

/* Decrement the reference count of `provider', releasing it if the count
   reaches zero.  Does nothing when `provider' is NULL. */
CG_EXTERN void CGDataProviderRelease(CGDataProviderRef cg_nullable provider);

CF_ASSUME_NONNULL_END

#endif /* CGDATAPROVIDER_H_ */