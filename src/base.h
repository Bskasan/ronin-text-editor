// base.h — fixed-width types, assert, arena, String8, UTF-8/16, formatter, dev log.
// Pure code. OS primitives come from platform.h (os_*).

#ifndef BASE_H
#define BASE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h> // memcpy / memset / memmove only

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef int32_t  b32;
typedef float    f32;
typedef double   f64;

#ifndef TEAL_DEV
#define TEAL_DEV 0
#endif
// The D3D11 debug layer; off in the optimized dev build (build.bat bench) so it does not skew timings.
#ifndef TEAL_D3D_DEBUG
#define TEAL_D3D_DEBUG TEAL_DEV
#endif

#define ARRAY_COUNT(a) ((i64)(sizeof(a) / sizeof((a)[0])))
#define KB(n) ((u64)(n) << 10)
#define MB(n) ((u64)(n) << 20)
#define GB(n) ((u64)(n) << 30)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define ALIGN_UP_POW2(x, a) (((x) + (a) - 1) & ~((u64)(a) - 1))

#if TEAL_DEV
// Logs the failed condition before breaking: without a debugger the break ends the process.
void assert_log(const char *file, int line, const char *expr);
#define ASSERT(c) do { if (!(c)) { assert_log(__FILE__, __LINE__, #c); __debugbreak(); } } while (0)
#else
#define ASSERT(c) ((void)0)
#endif

// ---------------------------------------------------------------------------
// Arena: reserve a large range up front, commit on demand.

typedef struct Arena {
    u8 *base;
    u64 reserved;
    u64 committed;
    u64 pos;
} Arena;

#define ARENA_COMMIT_GRANULARITY KB(64)

Arena arena_create(u64 reserve_size);
void *arena_push(Arena *arena, u64 size, u64 align); // zeroed
u64   arena_pos(Arena *arena);
void  arena_pop_to(Arena *arena, u64 pos);
void  arena_reset(Arena *arena);

#define PUSH_ARRAY(arena, T, n) ((T *)arena_push((arena), sizeof(T) * (u64)(n), _Alignof(T)))
#define PUSH_STRUCT(arena, T) PUSH_ARRAY(arena, T, 1)

// ---------------------------------------------------------------------------
// Strings. UTF-8, not NUL-terminated.

typedef struct String8 {
    u8 *data;
    i64 len;
} String8;

// UTF-16 for the Win32 boundary. Always NUL-terminated; len excludes the NUL.
typedef struct String16 {
    u16 *data;
    i64 len;
} String16;

#define STR8_LIT(s) ((String8){ (u8 *)(s), (i64)sizeof(s) - 1 })

String8  str8(u8 *data, i64 len);
String8  str8_cstr(const char *s);
b32      str8_equal(String8 a, String8 b);
String8  str8_copy(Arena *arena, String8 s);

#define UTF_REPLACEMENT 0xFFFD

u32      utf8_decode(u8 *s, i64 len, i64 *advance);
i64      utf8_encode(u32 codepoint, u8 *out); // out must hold 4 bytes
String16 str16_from_str8(Arena *arena, String8 s);
String8  str8_from_str16(Arena *arena, u16 *s, i64 len);

// Formatter. Supports only:
//   %d i32   %u u32   %D i64   %U u64   %x u32 hex   %X u64 hex
//   %s const char*   %S String8   %c char   %%
// An optional '0' flag and width apply to the numeric specifiers (e.g. %03d).
i64     fmt_v(u8 *out, i64 cap, const char *fmt, va_list args); // returns full length
String8 str8_fmtv(Arena *arena, const char *fmt, va_list args);
String8 str8_fmt(Arena *arena, const char *fmt, ...);

// ---------------------------------------------------------------------------
// Dev-only log: OutputDebugString + build\teal.log.

#if TEAL_DEV
void log_fmt(const char *fmt, ...);
#define LOG(...) log_fmt(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

#endif // BASE_H
