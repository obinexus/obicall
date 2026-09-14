#ifndef OBICALL_PLATFORM_H
#define OBICALL_PLATFORM_H

/* Explicit calling convention for every exported ABI function. On x86_64
 * (Windows x64 / SysV x64) cdecl and the platform default coincide, but we
 * name it explicitly so the contract is documented rather than assumed. */
#if defined(_WIN32)
#define OBICALL_CALL __cdecl
#else
#define OBICALL_CALL
#endif

#if defined(_WIN32)
#define OBICALL_EXPORT __declspec(dllexport)
#define OBICALL_IMPORT __declspec(dllimport)
#else
#define OBICALL_EXPORT __attribute__((visibility("default")))
#define OBICALL_IMPORT
#endif

#ifdef __cplusplus
#define OBICALL_BEGIN_DECLS extern "C" {
#define OBICALL_END_DECLS }
#else
#define OBICALL_BEGIN_DECLS
#define OBICALL_END_DECLS
#endif

#endif /* OBICALL_PLATFORM_H */
