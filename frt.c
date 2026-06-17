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

#include <ctype.h>
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

typedef struct
{
  void **ptrs;  /* registered heap pointers */
  size_t count; /* number of registered pointers */
  size_t cap;   /* allocated capacity of ptrs[] */
} cshift_arena_t;

/* Forward declarations of arena functions */
void *__cshift_arena_push(cshift_arena_t *a, void *ptr);

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

/* Reads one line from stdin (strips trailing '\n').
 * Returns a malloc'd string.  The caller is responsible for registering
 * it with the scope arena via __cshift_arena_push if needed.
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
  return line;
#endif
}

/* Arena-tracked version — registers result with the caller's scope arena.
 * Usage from C<<:  string line = read_line_a(__arena);
 */
char *read_line_a(cshift_arena_t *arena)
{
  char *s = read_line();
  if (arena && s)
    __cshift_arena_push(arena, s);
  return s;
}

/* Reads entire file into a malloc'd null-terminated string.
 * Returns NULL on error. Use read_file_s_a for arena-tracked version. */
char *read_file_s(const char *path)
{
  if (!path)
    return NULL;
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  if (sz < 0 || (unsigned long)sz > (1UL << 30))
  {
    fclose(f);
    return NULL;
  } /* 1 GB limit */
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

/* Arena-tracked file read. */
char *read_file_s_a(const char *path, cshift_arena_t *arena)
{
  char *s = read_file_s(path);
  if (arena && s)
    __cshift_arena_push(arena, s);
  return s;
}

/* Writes null-terminated string to file. Returns 0 on success, -1 on error. */
int write_file_s(const char *path, const char *contents)
{
  if (!path || !contents)
    return -1;
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  size_t len = strlen(contents);
  size_t written = fwrite(contents, 1, len, f);
  fclose(f);
  return (written == len) ? 0 : -1;
}

/* Append string to file. Returns 0 on success, -1 on error. */
int append_file_s(const char *path, const char *contents)
{
  if (!path || !contents)
    return -1;
  FILE *f = fopen(path, "ab");
  if (!f)
    return -1;
  size_t len = strlen(contents);
  size_t written = fwrite(contents, 1, len, f);
  fclose(f);
  return (written == len) ? 0 : -1;
}

/* Check if a file exists and is readable. */
int file_exists(const char *path)
{
  if (!path)
    return 0;
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  fclose(f);
  return 1;
}

/* Get file size in bytes, or -1 on error. */
long file_size_s(const char *path)
{
  if (!path)
    return -1;
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  if (fseek(f, 0, SEEK_END) != 0)
  {
    fclose(f);
    return -1;
  }
  long sz = ftell(f);
  fclose(f);
  return sz;
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

/* ── Safe string operations ──────────────────────────────────────────────── */
/* C<< strings are null-terminated char* (C-ABI compatible). These helpers   */
/* wrap common operations with null-safety and arena tracking.               */

/* Null-safe strlen — returns 0 for NULL. */
int str_len(const char *s) { return s ? (int)strlen(s) : 0; }

/* Safe char access — returns 0 (NUL) for out-of-bounds. */
char str_char_at(const char *s, int idx)
{
  if (!s)
    return 0;
  int l = (int)strlen(s);
  if (idx < 0)
    idx = l + idx;
  if (idx < 0 || idx >= l)
    return 0;
  return s[idx];
}

/* Safe compare — null-safe strcmp wrapper. */
int str_eq(const char *a, const char *b)
{
  if (!a && !b)
    return 1;
  if (!a || !b)
    return 0;
  return strcmp(a, b) == 0;
}

int str_starts_with(const char *s, const char *prefix)
{
  if (!s || !prefix)
    return 0;
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

int str_ends_with(const char *s, const char *suffix)
{
  if (!s || !suffix)
    return 0;
  size_t sl = strlen(s), xl = strlen(suffix);
  return sl >= xl && strcmp(s + sl - xl, suffix) == 0;
}

/* Arena-tracked safe string functions */
char *str_to_upper_a(const char *s, cshift_arena_t *arena)
{
  if (!s)
    return NULL;
  char *r = strdup(s);
  if (!r)
    return NULL;
  for (char *p = r; *p; p++)
    *p = (char)toupper((unsigned char)*p);
  if (arena)
    __cshift_arena_push(arena, r);
  return r;
}

char *str_to_lower_a(const char *s, cshift_arena_t *arena)
{
  if (!s)
    return NULL;
  char *r = strdup(s);
  if (!r)
    return NULL;
  for (char *p = r; *p; p++)
    *p = (char)tolower((unsigned char)*p);
  if (arena)
    __cshift_arena_push(arena, r);
  return r;
}

char *str_repeat_a(const char *s, int n, cshift_arena_t *arena)
{
  char *r = str_repeat(s, n);
  if (arena && r)
    __cshift_arena_push(arena, r);
  return r;
}

char *str_trim_a(const char *s, cshift_arena_t *arena)
{
  if (!s)
    return strdup("");
  while (*s && isspace((unsigned char)*s))
    s++;
  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1]))
    len--;
  char *r = (char *)malloc(len + 1);
  if (!r)
    return NULL;
  memcpy(r, s, len);
  r[len] = '\0';
  if (arena)
    __cshift_arena_push(arena, r);
  return r;
}

/* Split string by delimiter — returns arena-tracked array of strings.
 * out_count receives the number of parts. */
char **str_split_a(const char *s, const char *delim, int *out_count, cshift_arena_t *arena)
{
  if (!s || !delim || !out_count)
  {
    if (out_count)
      *out_count = 0;
    return NULL;
  }
  /* Count parts */
  size_t dlen = strlen(delim);
  if (dlen == 0)
  {
    *out_count = 0;
    return NULL;
  }
  int count = 1;
  const char *p = s;
  while ((p = strstr(p, delim)))
  {
    count++;
    p += dlen;
  }

  char **parts = (char **)malloc((size_t)count * sizeof(char *));
  if (!parts)
  {
    *out_count = 0;
    return NULL;
  }
  if (arena)
    __cshift_arena_push(arena, parts);

  int i = 0;
  p = s;
  const char *found;
  while ((found = strstr(p, delim)) != NULL)
  {
    size_t len2 = (size_t)(found - p);
    char *part = (char *)malloc(len2 + 1);
    if (!part)
      break;
    memcpy(part, p, len2);
    part[len2] = '\0';
    if (arena)
      __cshift_arena_push(arena, part);
    parts[i++] = part;
    p = found + dlen;
  }
  /* last part */
  parts[i] = strdup(p);
  if (arena && parts[i])
    __cshift_arena_push(arena, parts[i]);
  *out_count = count;
  return parts;
}

/* ── C<< Arena Allocator ──────────────────────────────────────────────────── */
/* Each C<< scope that allocates heap memory creates a cshift_arena_t.        */
/* All heap allocations in that scope are registered here. On scope exit,    */
/* cshift_arena_free_all() frees every registered pointer in one pass.       */
/* reset; calls cshift_arena_reset() which frees all data but keeps the      */
/* arena struct alive for re-use in the same scope.                          */

#include <stdlib.h>
#include <string.h>

/* Initialize an arena (already stack-allocated by the caller). */
void __cshift_arena_init(cshift_arena_t *a)
{
  a->ptrs = NULL;
  a->count = 0;
  a->cap = 0;
}

/* Register a heap pointer with the arena. Returns ptr unchanged so it can   */
/* be used as a pass-through: p = __cshift_arena_push(arena, vec_new(16));     */
void *__cshift_arena_push(cshift_arena_t *a, void *ptr)
{
  if (!ptr)
    return ptr;
  if (a->count >= a->cap)
  {
    size_t new_cap = a->cap ? a->cap * 2 : 8;
    a->ptrs = realloc(a->ptrs, new_cap * sizeof(void *));
    a->cap = new_cap;
  }
  a->ptrs[a->count++] = ptr;
  return ptr;
}

/* Free all registered pointers. The arena struct itself is also freed.      */
/* Call this on normal scope exit.                                           */
void __cshift_arena_free_all(cshift_arena_t *a)
{
  for (size_t i = a->count; i-- > 0;) /* LIFO order */
    free(a->ptrs[i]);
  free(a->ptrs);
  a->ptrs = NULL;
  a->count = 0;
  a->cap = 0;
}

/* Free all registered pointers but keep the arena alive.                    */
/* Called by the `reset;` statement.                                         */
void __cshift_arena_reset(cshift_arena_t *a)
{
  for (size_t i = a->count; i-- > 0;)
    free(a->ptrs[i]);
  /* keep a->ptrs buffer; reset count so it can be reused */
  a->count = 0;
}

/* ── Arena-tracked string builders ──────────────────────────────────────── */
/* These are identical to their base versions but register the result with
 * the caller's scope arena so no manual free() is needed.               */

char *str_concat_a(const char *a, const char *b, cshift_arena_t *arena)
{
  char *s = str_concat(a, b);
  if (arena && s)
    __cshift_arena_push(arena, s);
  return s;
}

char *int_to_str_a(long long val, cshift_arena_t *arena)
{
  char *s = int_to_str(val);
  if (arena && s)
    __cshift_arena_push(arena, s);
  return s;
}

char *float_to_str_a(double val, int precision, cshift_arena_t *arena)
{
  char *s = float_to_str(val, precision);
  if (arena && s)
    __cshift_arena_push(arena, s);
  return s;
}

char *str_slice_a(const char *s, int start, int end, cshift_arena_t *arena)
{
  char *r = str_slice(s, start, end);
  if (arena && r)
    __cshift_arena_push(arena, r);
  return r;
}

char *str_replace_a(const char *s, const char *from, const char *to, cshift_arena_t *arena)
{
  char *r = str_replace(s, from, to);
  if (arena && r)
    __cshift_arena_push(arena, r);
  return r;
}

/* ── C<< Container Implementations ──────────────────────────────────────────
 * All containers are type-erased (void*) — the element type is handled by
 * the C<< compiler which generates correctly-sized loads/stores. Each
 * container struct is heap-allocated and tracked by the scope arena.
 *
 * Design: elements are stored as raw bytes (memcpy in/out). The C<< codegen
 * already ensures the right type width at each call site.
 * ────────────────────────────────────────────────────────────────────────── */

#include <stdio.h>

/* ── Vector<T> ─────────────────────────────────────────────────────────────── */
typedef struct
{
  void *data;     /* element storage */
  size_t len;     /* current element count */
  size_t cap;     /* allocated capacity in elements */
  size_t elem_sz; /* bytes per element */
} CShiftVec;

void *vec_new(size_t elem_sz)
{
  /* elem_sz is the chunk_size in the cll signature — we repurpose it as
   * the element size since C<< cannot express elem_sz separately.
   * In practice the codegen passes the LLVM type width. */
  CShiftVec *v = (CShiftVec *)calloc(1, sizeof(CShiftVec));
  if (!v)
    return NULL;
  v->elem_sz = elem_sz ? elem_sz : 8; /* default 8-byte elements */
  return v;
}

int vec_push(CShiftVec *v, unsigned long long word)
{
  if (!v)
    return -1;
  if (v->len >= v->cap)
  {
    size_t new_cap = v->cap ? v->cap * 2 : 8;
    void *nd = realloc(v->data, new_cap * v->elem_sz);
    if (!nd)
      return -1;
    v->data = nd;
    v->cap = new_cap;
  }
  memcpy((char *)v->data + v->len * v->elem_sz, &word, v->elem_sz);
  v->len++;
  return 0;
}

/* vec_get: returns element as a 64-bit word (caller extracts the right width) */
unsigned long long vec_get(CShiftVec *v, unsigned long long idx)
{
  if (!v || idx >= v->len)
    return 0;
  unsigned long long word = 0;
  memcpy(&word, (char *)v->data + idx * v->elem_sz, v->elem_sz);
  return word;
}

int vec_set(CShiftVec *v, unsigned long long idx, unsigned long long word)
{
  if (!v || idx >= v->len)
    return -1;
  memcpy((char *)v->data + idx * v->elem_sz, &word, v->elem_sz);
  return 0;
}

unsigned long long vec_len(CShiftVec *v) { return v ? (unsigned long long)v->len : 0; }

int vec_pop(CShiftVec *v, void *out)
{
  if (!v || v->len == 0)
    return -1;
  v->len--;
  if (out)
    memcpy(out, (char *)v->data + v->len * v->elem_sz, v->elem_sz);
  return 0;
}

int vec_remove(CShiftVec *v, unsigned long long idx, void *out)
{
  if (!v || idx >= v->len)
    return -1;
  if (out)
    memcpy(out, (char *)v->data + idx * v->elem_sz, v->elem_sz);
  size_t tail = (v->len - idx - 1) * v->elem_sz;
  if (tail > 0)
    memmove((char *)v->data + idx * v->elem_sz, (char *)v->data + (idx + 1) * v->elem_sz, tail);
  v->len--;
  return 0;
}

int vec_contains(CShiftVec *v, unsigned long long needle)
{
  if (!v)
    return 0;
  for (size_t i = 0; i < v->len; i++)
  {
    unsigned long long word = 0;
    memcpy(&word, (char *)v->data + i * v->elem_sz, v->elem_sz);
    if (word == needle)
      return 1;
  }
  return 0;
}

void vec_clear(CShiftVec *v)
{
  if (v)
    v->len = 0;
}

void vec_free(CShiftVec *v)
{
  if (!v)
    return;
  free(v->data);
  free(v);
}

/* ── StringBuilder ─────────────────────────────────────────────────────────── */
typedef struct
{
  char *buf;
  size_t len;
  size_t cap;
} CShiftSB;

void *sb_new(void)
{
  CShiftSB *sb = (CShiftSB *)calloc(1, sizeof(CShiftSB));
  return sb;
}

static int sb_ensure(CShiftSB *sb, size_t extra)
{
  size_t need = sb->len + extra + 1;
  if (need > sb->cap)
  {
    size_t nc = sb->cap ? sb->cap * 2 : 64;
    while (nc < need)
      nc *= 2;
    char *nb = (char *)realloc(sb->buf, nc);
    if (!nb)
      return -1;
    sb->buf = nb;
    sb->cap = nc;
  }
  return 0;
}

int sb_append(CShiftSB *sb, const char *s)
{
  if (!sb || !s)
    return -1;
  size_t slen = strlen(s);
  if (sb_ensure(sb, slen) < 0)
    return -1;
  memcpy(sb->buf + sb->len, s, slen);
  sb->len += slen;
  sb->buf[sb->len] = '\0';
  return 0;
}

int sb_append_char(CShiftSB *sb, char c)
{
  if (!sb)
    return -1;
  if (sb_ensure(sb, 1) < 0)
    return -1;
  sb->buf[sb->len++] = c;
  sb->buf[sb->len] = '\0';
  return 0;
}

int sb_append_int(CShiftSB *sb, long long val)
{
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%lld", val);
  return sb_append(sb, tmp);
}

int sb_append_float(CShiftSB *sb, double val, int prec)
{
  char fmt[16], tmp[64];
  snprintf(fmt, sizeof(fmt), "%%.%df", prec);
  snprintf(tmp, sizeof(tmp), fmt, val);
  return sb_append(sb, tmp);
}

/* sb_build: returns a malloc'd copy of the buffer (caller/arena tracks it) */
char *sb_build(CShiftSB *sb)
{
  if (!sb)
    return strdup("");
  return strdup(sb->buf ? sb->buf : "");
}

unsigned long long sb_len(CShiftSB *sb) { return sb ? (unsigned long long)sb->len : 0; }
void sb_clear(CShiftSB *sb)
{
  if (sb)
    sb->len = 0;
  if (sb && sb->buf)
    sb->buf[0] = '\0';
}
void sb_free(CShiftSB *sb)
{
  if (!sb)
    return;
  free(sb->buf);
  free(sb);
}

/* ── HashMap<K,V> (string keys, 64-bit values) ─────────────────────────────── */
/* Simple open-addressing hash map: keys are C strings, values are 8-byte words */
typedef struct
{
  char **keys;
  unsigned long long *vals;
  size_t used;
  size_t cap;
} CShiftMap;

static size_t map_hash(const char *key, size_t cap)
{
  size_t h = 14695981039346656037ULL;
  for (const char *p = key; *p; p++)
    h = (h ^ (unsigned char)*p) * 1099511628211ULL;
  return h % cap;
}

void *map_new(void)
{
  CShiftMap *m = (CShiftMap *)calloc(1, sizeof(CShiftMap));
  return m;
}

static int map_grow(CShiftMap *m)
{
  size_t nc = m->cap ? m->cap * 2 : 16;
  char **nk = (char **)calloc(nc, sizeof(char *));
  unsigned long long *nv = (unsigned long long *)calloc(nc, sizeof(unsigned long long));
  if (!nk || !nv)
  {
    free(nk);
    free(nv);
    return -1;
  }
  for (size_t i = 0; i < m->cap; i++)
  {
    if (!m->keys[i])
      continue;
    size_t j = map_hash(m->keys[i], nc);
    while (nk[j])
      j = (j + 1) % nc;
    nk[j] = m->keys[i];
    nv[j] = m->vals[i];
  }
  free(m->keys);
  free(m->vals);
  m->keys = nk;
  m->vals = nv;
  m->cap = nc;
  return 0;
}

int map_set(CShiftMap *m, const char *key, unsigned long long val)
{
  if (!m || !key)
    return -1;
  if (m->used * 2 >= m->cap && map_grow(m) < 0)
    return -1;
  size_t i = map_hash(key, m->cap);
  while (m->keys[i] && strcmp(m->keys[i], key) != 0)
    i = (i + 1) % m->cap;
  if (!m->keys[i])
  {
    m->keys[i] = strdup(key);
    m->used++;
  }
  m->vals[i] = val;
  return 0;
}

int map_get(CShiftMap *m, const char *key, void *out)
{
  if (!m || !key || !m->cap)
    return 0;
  size_t i = map_hash(key, m->cap);
  while (m->keys[i])
  {
    if (strcmp(m->keys[i], key) == 0)
    {
      if (out)
        memcpy(out, &m->vals[i], 8);
      return 1;
    }
    i = (i + 1) % m->cap;
  }
  return 0;
}

int map_has(CShiftMap *m, const char *key) { return map_get(m, key, NULL); }

int map_remove(CShiftMap *m, const char *key)
{
  if (!m || !key || !m->cap)
    return 0;
  size_t i = map_hash(key, m->cap);
  while (m->keys[i])
  {
    if (strcmp(m->keys[i], key) == 0)
    {
      free(m->keys[i]);
      m->keys[i] = NULL;
      m->vals[i] = 0;
      m->used--;
      return 1;
    }
    i = (i + 1) % m->cap;
  }
  return 0;
}

unsigned long long map_len(CShiftMap *m) { return m ? (unsigned long long)m->used : 0; }
void map_clear(CShiftMap *m)
{
  if (!m)
    return;
  for (size_t i = 0; i < m->cap; i++)
  {
    free(m->keys[i]);
    m->keys[i] = NULL;
    m->vals[i] = 0;
  }
  m->used = 0;
}
void map_free(CShiftMap *m)
{
  if (!m)
    return;
  map_clear(m);
  free(m->keys);
  free(m->vals);
  free(m);
}

/* ── LinkedList<T> ─────────────────────────────────────────────────────────── */
typedef struct CShiftListNode
{
  struct CShiftListNode *next, *prev;
  unsigned long long data;
} CShiftListNode;
typedef struct
{
  CShiftListNode *head, *tail;
  size_t len;
  size_t elem_sz;
} CShiftList;

void *list_new(void) { return calloc(1, sizeof(CShiftList)); }

int list_push_back(CShiftList *l, unsigned long long val)
{
  if (!l)
    return -1;
  CShiftListNode *nd = (CShiftListNode *)calloc(1, sizeof(CShiftListNode));
  nd->data = val;
  nd->prev = l->tail;
  if (l->tail)
    l->tail->next = nd;
  else
    l->head = nd;
  l->tail = nd;
  l->len++;
  return 0;
}
int list_push(CShiftList *l, unsigned long long val)
{
  if (!l)
    return -1;
  CShiftListNode *nd = (CShiftListNode *)calloc(1, sizeof(CShiftListNode));
  nd->data = val;
  nd->next = l->head;
  if (l->head)
    l->head->prev = nd;
  else
    l->tail = nd;
  l->head = nd;
  l->len++;
  return 0;
}
int list_push_front(CShiftList *l, unsigned long long v) { return list_push(l, v); }
int list_pop_front(CShiftList *l, void *out)
{
  if (!l || !l->head)
    return -1;
  CShiftListNode *nd = l->head;
  if (out)
    memcpy(out, &nd->data, 8);
  l->head = nd->next;
  if (l->head)
    l->head->prev = NULL;
  else
    l->tail = NULL;
  free(nd);
  l->len--;
  return 0;
}
int list_pop(CShiftList *l, void *out) { return list_pop_front(l, out); }
int list_pop_back(CShiftList *l, void *out)
{
  if (!l || !l->tail)
    return -1;
  CShiftListNode *nd = l->tail;
  if (out)
    memcpy(out, &nd->data, 8);
  l->tail = nd->prev;
  if (l->tail)
    l->tail->next = NULL;
  else
    l->head = NULL;
  free(nd);
  l->len--;
  return 0;
}
unsigned long long list_get(CShiftList *l, unsigned long long idx)
{
  if (!l)
    return 0;
  CShiftListNode *nd = l->head;
  for (unsigned long long i = 0; i < idx && nd; i++)
    nd = nd->next;
  return nd ? nd->data : 0;
}
unsigned long long list_len(CShiftList *l) { return l ? (unsigned long long)l->len : 0; }
void list_clear(CShiftList *l)
{
  if (!l)
    return;
  CShiftListNode *nd = l->head;
  while (nd)
  {
    CShiftListNode *nx = nd->next;
    free(nd);
    nd = nx;
  }
  l->head = l->tail = NULL;
  l->len = 0;
}
void list_free(CShiftList *l)
{
  if (!l)
    return;
  list_clear(l);
  free(l);
}

/* ── Set<T> (sorted array) ─────────────────────────────────────────────────── */
typedef struct
{
  unsigned long long *data;
  size_t len, cap;
} CShiftSet;
void *set_new(void) { return calloc(1, sizeof(CShiftSet)); }
int set_insert(CShiftSet *s, unsigned long long v)
{
  if (!s)
    return -1;
  /* Binary search for insertion point */
  size_t lo = 0, hi = s->len;
  while (lo < hi)
  {
    size_t mid = (lo + hi) / 2;
    if (s->data[mid] < v)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < s->len && s->data[lo] == v)
    return 0; /* already present */
  if (s->len >= s->cap)
  {
    size_t nc = s->cap ? s->cap * 2 : 8;
    s->data = (unsigned long long *)realloc(s->data, nc * sizeof(unsigned long long));
    s->cap = nc;
  }
  memmove(s->data + lo + 1, s->data + lo, (s->len - lo) * sizeof(unsigned long long));
  s->data[lo] = v;
  s->len++;
  return 1;
}
int set_contains(CShiftSet *s, unsigned long long v)
{
  if (!s || !s->len)
    return 0;
  size_t lo = 0, hi = s->len;
  while (lo < hi)
  {
    size_t mid = (lo + hi) / 2;
    if (s->data[mid] < v)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < s->len && s->data[lo] == v ? 1 : 0;
}
int set_remove(CShiftSet *s, unsigned long long v)
{
  if (!s || !s->len)
    return 0;
  size_t lo = 0, hi = s->len;
  while (lo < hi)
  {
    size_t mid = (lo + hi) / 2;
    if (s->data[mid] < v)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo >= s->len || s->data[lo] != v)
    return 0;
  memmove(s->data + lo, s->data + lo + 1, (s->len - lo - 1) * sizeof(unsigned long long));
  s->len--;
  return 1;
}
unsigned long long set_len(CShiftSet *s) { return s ? (unsigned long long)s->len : 0; }
void set_free(CShiftSet *s)
{
  if (!s)
    return;
  free(s->data);
  free(s);
}

/* ── BitSet ─────────────────────────────────────────────────────────────────── */
typedef struct
{
  unsigned char *data;
  size_t nbits;
} CShiftBitSet;
void *bitset_new(size_t nbits)
{
  CShiftBitSet *bs = (CShiftBitSet *)calloc(1, sizeof(CShiftBitSet));
  bs->nbits = nbits;
  bs->data = (unsigned char *)calloc((nbits + 7) / 8, 1);
  return bs;
}
int bitset_set(CShiftBitSet *bs, size_t i)
{
  if (!bs || i >= bs->nbits)
    return -1;
  bs->data[i / 8] |= (1 << (i % 8));
  return 0;
}
int bitset_clear(CShiftBitSet *bs, size_t i)
{
  if (!bs || i >= bs->nbits)
    return -1;
  bs->data[i / 8] &= ~(1 << (i % 8));
  return 0;
}
int bitset_toggle(CShiftBitSet *bs, size_t i)
{
  if (!bs || i >= bs->nbits)
    return -1;
  bs->data[i / 8] ^= (1 << (i % 8));
  return 0;
}
int bitset_get(CShiftBitSet *bs, size_t i)
{
  if (!bs || i >= bs->nbits)
    return 0;
  return (bs->data[i / 8] >> (i % 8)) & 1;
}
unsigned long long bitset_count(CShiftBitSet *bs)
{
  if (!bs)
    return 0;
  unsigned long long n = 0;
  for (size_t i = 0; i < (bs->nbits + 7) / 8; i++)
  {
    unsigned char b = bs->data[i];
    while (b)
    {
      n += b & 1;
      b >>= 1;
    }
  }
  return n;
}
void bitset_and(CShiftBitSet *d, CShiftBitSet *s)
{
  if (!d || !s)
    return;
  size_t nb = (d->nbits < s->nbits ? d->nbits : s->nbits + 7) / 8;
  for (size_t i = 0; i < nb; i++)
    d->data[i] &= s->data[i];
}
void bitset_or(CShiftBitSet *d, CShiftBitSet *s)
{
  if (!d || !s)
    return;
  size_t nb = (d->nbits < s->nbits ? d->nbits : s->nbits + 7) / 8;
  for (size_t i = 0; i < nb; i++)
    d->data[i] |= s->data[i];
}
void bitset_xor(CShiftBitSet *d, CShiftBitSet *s)
{
  if (!d || !s)
    return;
  size_t nb = (d->nbits < s->nbits ? d->nbits : s->nbits + 7) / 8;
  for (size_t i = 0; i < nb; i++)
    d->data[i] ^= s->data[i];
}
void bitset_not(CShiftBitSet *bs)
{
  if (!bs)
    return;
  size_t nb = (bs->nbits + 7) / 8;
  for (size_t i = 0; i < nb; i++)
    bs->data[i] = ~bs->data[i];
}
void bitset_free(CShiftBitSet *bs)
{
  if (!bs)
    return;
  free(bs->data);
  free(bs);
}

/* ── SortedVec<T> (sorted dynamic array with custom comparator) ─────────────── */
typedef struct
{
  void *data;
  size_t len, cap, elem_sz;
  int (*cmp)(const void *, const void *);
} CShiftSVec;
void *svec_new(int (*cmp)(const void *, const void *))
{
  CShiftSVec *sv = (CShiftSVec *)calloc(1, sizeof(CShiftSVec));
  sv->cmp = cmp;
  sv->elem_sz = 8;
  return sv;
}
int svec_push(CShiftSVec *sv, unsigned long long v)
{
  if (!sv)
    return -1;
  if (sv->len >= sv->cap)
  {
    size_t nc = sv->cap ? sv->cap * 2 : 8;
    sv->data = realloc(sv->data, nc * sv->elem_sz);
    sv->cap = nc;
  }
  /* insertion sort */
  size_t i = sv->len;
  while (i > 0)
  {
    unsigned long long prev;
    memcpy(&prev, (char *)sv->data + (i - 1) * sv->elem_sz, sv->elem_sz);
    if (prev <= v)
      break;
    memcpy((char *)sv->data + i * sv->elem_sz, &prev, sv->elem_sz);
    i--;
  }
  memcpy((char *)sv->data + i * sv->elem_sz, &v, sv->elem_sz);
  sv->len++;
  return 0;
}
unsigned long long svec_get(CShiftSVec *sv, unsigned long long idx)
{
  if (!sv || idx >= sv->len)
    return 0;
  unsigned long long w = 0;
  memcpy(&w, (char *)sv->data + idx * sv->elem_sz, sv->elem_sz);
  return w;
}
unsigned long long svec_len(CShiftSVec *sv) { return sv ? sv->len : 0; }
int svec_find(CShiftSVec *sv, unsigned long long v)
{
  if (!sv)
    return -1;
  size_t lo = 0, hi = sv->len;
  while (lo < hi)
  {
    size_t mid = (lo + hi) / 2;
    unsigned long long w = 0;
    memcpy(&w, (char *)sv->data + mid * sv->elem_sz, sv->elem_sz);
    if (w < v)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < sv->len)
  {
    unsigned long long w = 0;
    memcpy(&w, (char *)sv->data + lo * sv->elem_sz, sv->elem_sz);
    if (w == v)
      return (int)lo;
  }
  return -1;
}
int svec_remove(CShiftSVec *sv, unsigned long long v)
{
  if (!sv)
    return -1;
  int idx = svec_find(sv, v);
  if (idx < 0)
    return -1;
  memmove((char *)sv->data + idx * sv->elem_sz, (char *)sv->data + (idx + 1) * sv->elem_sz,
          (sv->len - idx - 1) * sv->elem_sz);
  sv->len--;
  return 0;
}
void svec_free(CShiftSVec *sv)
{
  if (!sv)
    return;
  free(sv->data);
  free(sv);
}

/* map_insert and map_contains are the canonical names; map_set/map_has are aliases */
int map_insert(CShiftMap *m, const char *key, unsigned long long val)
{
  return map_set(m, key, val);
}

int map_contains(CShiftMap *m, const char *key) { return map_has(m, key); }
