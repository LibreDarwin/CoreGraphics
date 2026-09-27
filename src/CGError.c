/* CoreGraphics - CGError.c
   Copyright (C) 2026, LibreDarwin

   CoreGraphics error reporting.

   The real framework funnels these through CFError/CFLog, which are not
   yet part of this tree.  Until CoreFoundation lands we write the message
   to stderr so the behaviour (an error is reported when an operation
   fails) is preserved and no symbol is missing.  This file should be
   replaced by the CFError-backed implementation once CoreFoundation is
   available. */

#include "CGInternal.h"

#include <stdio.h>

void
CGPostError(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    fputs("CoreGraphics: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}
