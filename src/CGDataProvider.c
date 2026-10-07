/* CoreGraphics - CGDataProvider.c
   Copyright (C) 2026, LibreDarwin
   SPDX-License-Identifier: BSD-3-Clause

   The CFData-backed data provider.

   A provider is a reference-counted object that owns a CFData and hands its
   bytes back through CGDataProviderCopyData.  That is the whole surface the
   profile-construction APIs in this tree consume: none of them needs the
   callback-based, sequential-IO or direct-access provider machinery, so
   nothing of it is built here.  The type is a CoreFoundation object in
   Apple's implementation, which is observable only through
   CGDataProviderGetTypeID; the value below mirrors what the current OS
   assigns at runtime. */

#include <CoreFoundation/CFData.h>
#include <stdlib.h>

#include "CGDataProvider.h"
#include "CGInternal.h"

#define CG_DATA_PROVIDER_TYPE_ID 75

struct CGDataProvider {
    /* Saturating reference count.  Callers hold it via CGDataProviderRef. */
    long refcount;
    /* The bytes the provider backs, retained, or NULL. */
    CFDataRef data;
};

CFTypeID CGDataProviderGetTypeID(void)
{
    return CG_DATA_PROVIDER_TYPE_ID;
}

CGDataProviderRef CGDataProviderCreateWithCFData(CFDataRef data)
{
    struct CGDataProvider *p;

    if (!data)
        return NULL;
    p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->refcount = 1;
    p->data = CFRetain(data);
    return p;
}

CFDataRef CGDataProviderCopyData(CGDataProviderRef provider)
{
    struct CGDataProvider *p = provider;

    if (!p || !p->data)
        return NULL;
    return (CFDataRef)CFRetain(p->data);
}

CGDataProviderRef CGDataProviderRetain(CGDataProviderRef provider)
{
    struct CGDataProvider *p = provider;

    if (p)
        p->refcount++;
    return provider;
}

void CGDataProviderRelease(CGDataProviderRef provider)
{
    struct CGDataProvider *p = provider;

    if (!p)
        return;
    if (--p->refcount > 0)
        return;
    if (p->data)
        CFRelease(p->data);
    free(p);
}