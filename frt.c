/*
 * frt.c  —  C<< Foundation Runtime
 *
 * Implements the thin helpers declared in stdlib.cs §20 that don't exist
 * as plain libc symbols (errno wrappers, string builders, etc.).
 *
 * Compile & link with every C<< binary:
 *   cc -c frt.c -o frt.o
 *   cshift myprogram.cs -o myprogram   # cshift links frt.o automatically
 *   # OR manually:
 *   cc myprogram.o frt.o -lc -lm -o myprogram
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

/* ── errno ──────────────────────────────────────────────────────────────── */

int get_errno(void) { return errno; }
void set_errno(int val) { errno = val; }

/* ── Safe arithmetic ────────────────────────────────────────────────────── */

/* Returns 1 if overflow, 0 on success. Writes result to *out. */
int add_i32_safe(int a, int b, int *out)
{
  long long r = (long long)a + (long long)b;
  if (r > 0x7FFFFFFF || r < -(long long)0x80000000)
    return 1;
  *out = (int)r;
  return 0;
}

int mul_i32_safe(int a, int b, int *out)
{
  long long r = (long long)a * (long long)b;
  if (r > 0x7FFFFFFF || r < -(long long)0x80000000)
    return 1;
  *out = (int)r;
  return 0;
}

/* ── String helpers ─────────────────────────────────────────────────────── */

/* Caller must free() the returned string. */
char *str_concat(const char *a, const char *b)
{
  if (!a)
    a = "";
  if (!b)
    b = "";
  size_t la = strlen(a), lb = strlen(b);
  char *r = (char *)malloc(la + lb + 1);
  if (!r)
    return NULL;
  memcpy(r, a, la);
  memcpy(r + la, b, lb + 1);
  return r;
}

char *str_repeat(const char *s, int n)
{
  if (!s || n <= 0)
    return strdup("");
  size_t len = strlen(s);
  char *r = (char *)malloc(len * (size_t)n + 1);
  if (!r)
    return NULL;
  for (int i = 0; i < n; i++)
    memcpy(r + (size_t)i * len, s, len);
  r[len * (size_t)n] = '\0';
  return r;
}

/* Returns a malloc'd substring s[start..end) (negative indices count from end).
 */
char *str_slice(const char *s, int start, int end)
{
  if (!s)
    return strdup("");
  int len = (int)strlen(s);
  if (start < 0)
    start = len + start;
  if (end < 0)
    end = len + end;
  if (start < 0)
    start = 0;
  if (end > len)
    end = len;
  if (start >= end)
    return strdup("");
  int sz = end - start;
  char *r = (char *)malloc((size_t)sz + 1);
  if (!r)
    return NULL;
  memcpy(r, s + start, (size_t)sz);
  r[sz] = '\0';
  return r;
}

/* Returns index of first occurrence of needle in s, or -1. */
int str_index_of(const char *s, const char *needle)
{
  if (!s || !needle)
    return -1;
  const char *p = strstr(s, needle);
  return p ? (int)(p - s) : -1;
}

/* Returns malloc'd copy of s with every occurrence of `from` replaced by `to`.
 */
char *str_replace(const char *s, const char *from, const char *to)
{
  if (!s || !from || !to)
    return strdup(s ? s : "");
  size_t flen = strlen(from);
  if (flen == 0)
    return strdup(s);

  /* Count occurrences */
  size_t count = 0;
  const char *p = s;
  while ((p = strstr(p, from)) != NULL)
  {
    count++;
    p += flen;
  }

  size_t slen = strlen(s), tlen = strlen(to);
  size_t new_len = slen + count * (tlen - flen) + 1;
  char *result = (char *)malloc(new_len);
  if (!result)
    return NULL;

  char *w = result;
  p = s;
  const char *found;
  while ((found = strstr(p, from)) != NULL)
  {
    size_t chunk = (size_t)(found - p);
    memcpy(w, p, chunk);
    w += chunk;
    memcpy(w, to, tlen);
    w += tlen;
    p = found + flen;
  }
  strcpy(w, p);
  return result;
}

char *int_to_str(long long val)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", val);
  return strdup(buf);
}

char *float_to_str(double val, int precision)
{
  char fmt[16], buf[64];
  snprintf(fmt, sizeof(fmt), "%%.%df", precision < 0 ? 6 : precision);
  snprintf(buf, sizeof(buf), fmt, val);
  return strdup(buf);
}

/* ── I/O helpers ─────────────────────────────────────────────────────────── */

/* Reads one line from stdin (strips trailing '\n').  Returns malloc'd string.
 */
char *read_line(void)
{
  char *line = NULL;
  size_t cap = 0;
#ifdef _WIN32
  char buf[4096];
  if (!fgets(buf, sizeof(buf), stdin))
    return strdup("");
  size_t l = strlen(buf);
  if (l > 0 && buf[l - 1] == '\n')
    buf[l - 1] = '\0';
  return strdup(buf);
#else
  ssize_t n = getline(&line, &cap, stdin);
  if (n < 0)
  {
    free(line);
    return strdup("");
  }
  if (n > 0 && line[n - 1] == '\n')
    line[n - 1] = '\0';
  return line; /* already malloc'd by getline */
#endif
}

/* Reads entire file into a malloc'd null-terminated string. */
char *read_file_s(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  if (sz < 0)
  {
    fclose(f);
    return NULL;
  }
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf)
  {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)sz, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

/* Writes null-terminated string to file. Returns 0 on success. */
int write_file_s(const char *path, const char *contents)
{
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  size_t len = strlen(contents);
  size_t written = fwrite(contents, 1, len, f);
  fclose(f);
  return (written == len) ? 0 : -1;
}

/* ── Timing ──────────────────────────────────────────────────────────────── */

/* Monotonic seconds with nanosecond resolution. */
double now_seconds(void)
{
#ifdef _WIN32
  LARGE_INTEGER freq, count;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&count);
  return (double)count.QuadPart / (double)freq.QuadPart;
#elif defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#else
  return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

/* ── C<< Arena Allocator ──────────────────────────────────────────────────── */
/* Each C<< scope that allocates heap memory creates a cshift_arena_t.        */
/* All heap allocations in that scope are registered here. On scope exit,    */
/* cshift_arena_free_all() frees every registered pointer in one pass.       */
/* reset; calls cshift_arena_reset() which frees all data but keeps the      */
/* arena struct alive for re-use in the same scope.                          */

#include <stdlib.h>
#include <string.h>

typedef struct {
    void  **ptrs;   /* registered heap pointers */
    size_t  count;  /* number of registered pointers */
    size_t  cap;    /* allocated capacity of ptrs[] */
} cshift_arena_t;

/* Initialize an arena (already stack-allocated by the caller). */
static inline void cshift_arena_init(cshift_arena_t *a)
{
    a->ptrs  = NULL;
    a->count = 0;
    a->cap   = 0;
}

/* Register a heap pointer with the arena. Returns ptr unchanged so it can   */
/* be used as a pass-through: p = cshift_arena_push(arena, vec_new(16));     */
static inline void *cshift_arena_push(cshift_arena_t *a, void *ptr)
{
    if (!ptr) return ptr;
    if (a->count >= a->cap) {
        size_t new_cap = a->cap ? a->cap * 2 : 8;
        a->ptrs = realloc(a->ptrs, new_cap * sizeof(void *));
        a->cap  = new_cap;
    }
    a->ptrs[a->count++] = ptr;
    return ptr;
}

/* Free all registered pointers. The arena struct itself is also freed.      */
/* Call this on normal scope exit.                                           */
static inline void cshift_arena_free_all(cshift_arena_t *a)
{
    for (size_t i = a->count; i-- > 0; )   /* LIFO order */
        free(a->ptrs[i]);
    free(a->ptrs);
    a->ptrs  = NULL;
    a->count = 0;
    a->cap   = 0;
}

/* Free all registered pointers but keep the arena alive.                    */
/* Called by the `reset;` statement.                                         */
static inline void cshift_arena_reset(cshift_arena_t *a)
{
    for (size_t i = a->count; i-- > 0; )
        free(a->ptrs[i]);
    /* keep a->ptrs buffer; reset count so it can be reused */
    a->count = 0;
}
