/* CoreGraphics - CGInternal.h
   Copyright (C) 2026, LibreDarwin

   Declarations for CoreGraphics' internal helpers that are not part of
   the public ABI but are referenced by the code in this directory.

   Not installed; not exported. */

#ifndef CGINTERNAL_H_
#define CGINTERNAL_H_

#include "CGBase.h"
#include <stdarg.h>

CG_BEGIN_DECLS

/* Marks a symbol that exists in Apple's CoreGraphics but is not part of the
   exported ABI.  Apple's binary records these as "private externals": they
   have a real address and are called from inside the framework, but the
   linker never publishes them, so they are absent from the SDK's .tbd and
   from the dynamic symbol table.  Hiding them here keeps our export list
   identical to Apple's instead of leaking extra public entry points. */
#define CG_PRIVATE __attribute__((visibility("hidden")))

/* The declaration form matching CG_PRIVATE.  CG_EXTERN cannot be used here
   because it hard-codes visibility("default"), which contradicts the
   hidden attribute. */
#define CG_EXTERN_PRIVATE extern CG_PRIVATE

/* Report a CoreGraphics error.  Mirrors the internal _CGPostError entry
   point seen in the disassembly, which is called printf-style, e.g.
   CGPostError("%s: singular matrix.", "CGAffineTransformInvert").
   It is a private symbol in the original framework and is hidden here to
   match. */
void CGPostError(const char *format, ...) CG_PRINTF_FUNCTION(1, 2)
    CG_PRIVATE;

/* The MD5 of `len' bytes at `data', written as 16 bytes to `out'.  Used only
   for the ICC profile ID, which is the MD5 of the profile with its flags and
   ID fields zeroed; see CGMD5.c. */
void CGMD5(const void *data, size_t len, unsigned char out[16]) CG_PRIVATE;

CG_END_DECLS

#endif /* CGINTERNAL_H_ */
