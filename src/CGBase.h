/* CoreGraphics - CGBase.h
   Copyright (C) 2026, LibreDarwin

   Base types, visibility macros and compiler-feature shims for
   CoreGraphics.  ABI-compatible with Apple's CGBase.h; the wording and
   structure here are ours.

   This header intentionally does not depend on CoreFoundation or on
   <Availability.h>.  Those are not yet available in this tree, so the
   handful of macros we need are defined locally with the same expansion
   Apple's headers use.  When CoreFoundation lands, the local definitions
   in this file should be deleted in favour of the CF ones. */

#ifndef CGBASE_H_
#define CGBASE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <float.h>
#include <TargetConditionals.h>
#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFCGTypes.h>
#include <os/availability.h>

#if defined(__has_attribute)
#  define __CG_HAS_COMPILER_ATTRIBUTE(attribute) __has_attribute(attribute)
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define __CG_HAS_COMPILER_ATTRIBUTE(attribute) (1)
#else
#  define __CG_HAS_COMPILER_ATTRIBUTE(attribute) (0)
#endif

#if defined(__has_extension)
#  define __CG_HAS_COMPILER_EXTENSION(extension) __has_extension(extension)
#elif defined(__has_feature)
#  define __CG_HAS_COMPILER_EXTENSION(extension) __has_feature(extension)
#else
#  define __CG_HAS_COMPILER_EXTENSION(extension) (0)
#endif

/* Definition of `__WIN32__' where appropriate if it's not already defined. */

#if !defined(__WIN32__)
# if defined(_WIN32) || defined(__CYGWIN32__)
#  define __WIN32__ 1
# endif
#endif

/* Definition of `CG_EXTERN'. */

#if !defined(CG_EXTERN)
# if defined(__WIN32__)
#  if defined(CG_BUILDING_CG)
#   if defined(__cplusplus)
#    define CG_EXTERN extern "C" __declspec(dllexport)
#   else
#    define CG_EXTERN extern __declspec(dllexport)
#   endif
#  else /* !defined(CG_BUILDING_CG) */
#   if defined(__cplusplus)
#    define CG_EXTERN extern "C" __declspec(dllimport)
#   else
#    define CG_EXTERN extern __declspec(dllimport)
#   endif
#  endif /* !defined(CG_BUILDING_CG) */
# else /* !defined(__WIN32__) */
#  if defined(__cplusplus)
#   define CG_EXTERN extern "C" __attribute__((visibility("default")))
#  else
#   define CG_EXTERN extern __attribute__((visibility("default")))
#  endif
# endif /* !defined(__WIN32__) */
#endif /* !defined(CG_EXTERN) */

/* Definition of `CG_LOCAL'. */

#if !defined(CG_LOCAL)
# if __CG_HAS_COMPILER_ATTRIBUTE(visibility)
#  if defined(__cplusplus)
#   define CG_LOCAL extern "C" __attribute__((visibility("hidden")))
#  else
#   define CG_LOCAL extern __attribute__((visibility("hidden")))
#  endif
# else
#  define CG_LOCAL CG_EXTERN
# endif
#endif /* !defined(CG_LOCAL) */

/* Definition of `CG_EXTERN_64' */

#if !defined(CG_EXTERN_64)
# if defined(__LP64__)
#  define CG_EXTERN_64 CG_EXTERN
# else /* !defined(__LP64__) */
#  define CG_EXTERN_64 CG_LOCAL
# endif /* defined(__LP64__) */
#endif /* !defined(CG_EXTERN_64) */

/* Definition of `CG_EXTERN_32' */

#if !defined(CG_EXTERN_32)
# if defined(__LP64__)
#  define CG_EXTERN_32 CG_LOCAL __attribute__((unused))
# else /* !defined(__LP64__) */
#  define CG_EXTERN_32 CG_EXTERN
# endif /* !defined(__LP64__) */
#endif /* !defined(CG_EXTERN_32) */

/* Definition of `CG_LOCAL_64' */

#if !defined(CG_LOCAL_64)
# if defined(__LP64__)
#  define CG_LOCAL_64 CG_LOCAL
# else /* !defined(__LP64__) */
#  define CG_LOCAL_64 CG_LOCAL __attribute__((unused))
# endif /* !defined(__LP64__) */
#endif /* !defined(CG_LOCAL_64) */

/* Definition of `CG_LOCAL_32' */

#if !defined(CG_LOCAL_32)
# if defined(__LP64__)
#  define CG_LOCAL_32 CG_LOCAL __attribute__((unused))
# else /* !defined(__LP64__) */
#  define CG_LOCAL_32 CG_LOCAL
# endif /* !defined(__LP64__) */
#endif /* !defined(CG_LOCAL_32) */

/* Definition of `__CG_DEPRECATED'. */

#if !defined(__CG_DEPRECATED)
# if __CG_HAS_COMPILER_ATTRIBUTE(deprecated) && !defined(CG_BUILDING_CG)
#  define __CG_DEPRECATED __attribute__((deprecated))
# else
#  define __CG_DEPRECATED
# endif
#endif

/* Definition of `__CG_DEPRECATED_WITH_MSG'. */

#if !defined(__CG_DEPRECATED_WITH_MSG)
# if __CG_HAS_COMPILER_ATTRIBUTE(deprecated)                          \
    && __CG_HAS_COMPILER_EXTENSION(attribute_deprecated_with_message) \
    && !defined(CG_BUILDING_CG)
#  define __CG_DEPRECATED_WITH_MSG(msg) __attribute__((deprecated(msg)))
# else
#  define __CG_DEPRECATED_WITH_MSG(msg) __CG_DEPRECATED
# endif
#endif

/* Definition of `__CG_DEPRECATED_ENUMERATOR'. */

#if !defined(__CG_DEPRECATED_ENUMERATOR)
# if __CG_HAS_COMPILER_ATTRIBUTE(deprecated)                        \
   && __CG_HAS_COMPILER_EXTENSION(enumerator_attributes)            \
   && !defined(CG_BUILDING_CG)
#  define __CG_DEPRECATED_ENUMERATOR __attribute__((deprecated))
# else
#  define __CG_DEPRECATED_ENUMERATOR
# endif
#endif

/* Definition of `__CG_STATIC_ASSERT'. */

#if !defined(__CG_STATIC_ASSERT)
# if defined(__cplusplus) && __CG_HAS_COMPILER_EXTENSION(cxx_static_assert)
#  define __CG_STATIC_ASSERT(constant_expression)                 \
     static_assert(constant_expression, #constant_expression)
# elif !defined(__cplusplus) && __CG_HAS_COMPILER_EXTENSION(c_static_assert)
#  define __CG_STATIC_ASSERT(constant_expression)                 \
     _Static_assert(constant_expression, #constant_expression)
# else
#  define __CG_STATIC_ASSERT(constant_expression)
# endif
#endif

/* Definition of `CG_INLINE'. */

#if !defined(CG_INLINE)
# if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#  define CG_INLINE static inline
# elif defined(__cplusplus)
#  define CG_INLINE static inline
# elif defined(__GNUC__)
#  define CG_INLINE static __inline__
# elif defined(__WIN32__)
#  define CG_INLINE static __inline
# else
#  define CG_INLINE static
# endif
#endif

/* Definition of `__CG_NO_INLINE'. */

#if !defined(__CG_NO_INLINE)
# if __CG_HAS_COMPILER_ATTRIBUTE(noinline)
#  define __CG_NO_INLINE static __attribute__((noinline))
# else
#  define __CG_NO_INLINE static
# endif
#endif

/* Definition of CG_PURE. */

#if !defined(CG_PURE)
# if __CG_HAS_COMPILER_ATTRIBUTE(pure)
#  define CG_PURE __attribute__((pure))
# else
#  define CG_PURE
# endif
#endif

/* Definition of `CG_BEGIN_DECLS' and `CG_END_DECLS'. */

#if !defined(CG_BEGIN_DECLS)
# if defined(__cplusplus)
#  define CG_BEGIN_DECLS extern "C" {
#  define CG_END_DECLS }
# else
#  define CG_BEGIN_DECLS
#  define CG_END_DECLS
# endif
#endif

/* Definition of `CG_PRINTF_FUNCTION'. */

#if !defined(CG_PRINTF_FUNCTION)
# if __CG_HAS_COMPILER_ATTRIBUTE(format)                            \
    && __CG_HAS_COMPILER_EXTENSION(format_nonliteral)
#  define CG_PRINTF_FUNCTION(F,A) __attribute__((format(__NSString__, F, A)))
# else
#  define CG_PRINTF_FUNCTION(F,A)
# endif
#endif

/* Definition of `CG_BOXABLE'. */

#if defined(__has_attribute) && __has_attribute(objc_boxable)
# define CG_BOXABLE __attribute__((objc_boxable))
#else
# define CG_BOXABLE
#endif

/* Availability.  Apple's headers pull these in from <Availability.h> and
   <CoreFoundation/CFBase.h>.  Until CoreFoundation is available in this
   tree we no-op them so that the declarations still parse and the ABI is
   unchanged. */

#if !defined(CG_AVAILABILITY_MACROS)
# define CG_AVAILABILITY_MACROS 1
# if !defined(API_AVAILABLE)
#  define API_AVAILABLE(...)
# endif
# if !defined(API_UNAVAILABLE)
#  define API_UNAVAILABLE(...)
# endif
# if !defined(API_DEPRECATED)
#  define API_DEPRECATED(...)
# endif
# if !defined(API_DEPRECATED_WITH_REPLACEMENT)
#  define API_DEPRECATED_WITH_REPLACEMENT(...)
# endif
# if !defined(CF_CLOSED_ENUM)
#  if defined(__cplusplus)
#   define CF_CLOSED_ENUM(type, name) CF_ENUM(name, type)
#  else
#   define CF_CLOSED_ENUM(type, name) enum name : type
#  endif
# endif
# if !defined(CF_ENUM)
#  if defined(__cplusplus)
#   define CF_ENUM(name, type, ...) enum name : type
#  else
#   define CF_ENUM(name, type, ...) enum name
#  endif
# endif
# if !defined(CF_ASSUME_NONNULL_BEGIN)
#  define CF_ASSUME_NONNULL_BEGIN
#  define CF_ASSUME_NONNULL_END
# endif
# if !defined(CF_IMPLICIT_BRIDGING_ENABLED)
#  define CF_IMPLICIT_BRIDGING_ENABLED
# endif
# if !defined(CF_BRIDGED_TYPE)
#  define CF_BRIDGED_TYPE(id)
#  define CF_BRIDGED_MUTABLE_TYPE(id)
# endif
#endif /* !defined(CG_AVAILABILITY_MACROS) */

#if (defined(TARGET_OS_LINUX) && TARGET_OS_LINUX) || defined(CG_LINUX)
# if defined(__x86_64__)
#  define CG_OS_VERSION_2020 1
typedef unsigned int boolean_t;
# else
#  define CG_OS_VERSION_2020 1
typedef int boolean_t;
# endif
#else
# define CG_OS_VERSION_2020 1
#endif

/* Definition of `CGFLOAT_TYPE', `CGFLOAT_IS_DOUBLE', `CGFLOAT_MIN',
   `CGFLOAT_MAX' and `CGFLOAT_EPSILON'. */

#if !defined(CF_DEFINES_CG_TYPES)
# if defined(__LP64__) && __LP64__
#  define CGFLOAT_TYPE double
#  define CGFLOAT_IS_DOUBLE 1
#  define CGFLOAT_MIN DBL_MIN
#  define CGFLOAT_MAX DBL_MAX
#  define CGFLOAT_EPSILON DBL_EPSILON
# else
#  define CGFLOAT_TYPE float
#  define CGFLOAT_IS_DOUBLE 0
#  define CGFLOAT_MIN FLT_MIN
#  define CGFLOAT_MAX FLT_MAX
#  define CGFLOAT_EPSILON FLT_EPSILON
# endif

/* Definition of the `CGFloat' type and `CGFLOAT_DEFINED'. */

typedef CGFLOAT_TYPE CGFloat;
#define CGFLOAT_DEFINED 1
#endif /* CF_DEFINES_CG_TYPES */

/* Nullability qualifiers used by the CoreGraphics headers. `cg_nullable' is
   defined away for Swift clients, which have no spelling for it. */

#if defined(__swift__)
#   define cg_nullable
#else
#   define cg_nullable __nullable
#endif

#if __has_feature(nullability_on_arrays)
# define CG_NONNULL_ARRAY __nonnull
# define CG_NULLABLE_ARRAY __nullable
#else
# define CG_NONNULL_ARRAY
# define CG_NULLABLE_ARRAY
#endif

#endif /* CGBASE_H_ */
