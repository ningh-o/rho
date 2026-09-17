#include "rho.h"

const char *PRIM_NAMES[PRIM_COUNT] = {"bool", "i8",  "i16",  "i32",    "i64",
                                      "u8",   "u16", "u32",  "u64",    "f32",
                                      "f64",  "string",   "usize", "isize", "void"};

// ---------------------------------------------------------------- arena ---

static Arena *arena_cur = NULL;

static void arena_block(size_t size) {
  if (size < 1 << 20)
    size = 1 << 20;
  Arena *a = malloc(sizeof(Arena) + size);
  if (!a) {
    fprintf(stderr, "rho: out of memory\n");
    exit(1);
  }
  a->at = (char *)(a + 1);
  a->left = size;
  a->next = arena_cur;
  arena_cur = a;
}

void *arena_alloc(size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (!arena_cur || arena_cur->left < n)
    arena_block(n);
  void *p = arena_cur->at;
  arena_cur->at += n;
  arena_cur->left -= n;
  return p;
}

void *arena_alloc_zeroed(size_t n) {
  void *p = arena_alloc(n);
  memset(p, 0, n);
  return p;
}

char *arena_strndup(const char *s, size_t n) {
  char *p = arena_alloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

char *arena_strdup(const char *s) { return arena_strndup(s, strlen(s)); }

char *arena_printf(const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char *p = arena_alloc(n + 1);
  vsnprintf(p, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  return p;
}

// --------------------------------------------------------------- strings ---

Str str_from(const char *c) { return str_from_len(c, strlen(c)); }

Str str_from_len(const char *c, size_t n) {
  Str s = {c, n};
  return s;
}

bool str_eq(Str a, Str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }

bool str_eq_c(Str a, const char *b) { return str_eq(a, str_from(b)); }

Str str_slice(Str s, size_t lo, size_t hi) {
  if (lo > s.n)
    lo = s.n;
  if (hi > s.n)
    hi = s.n;
  if (hi < lo)
    hi = lo;
  Str r = {s.p + lo, hi - lo};
  return r;
}

char *str_to_c(Str s) { return arena_strndup(s.p, s.n); }

void sb_grow(SB *sb, size_t extra) {
  if (sb->n + extra <= sb->cap)
    return;
  size_t cap = sb->cap ? sb->cap : 256;
  while (cap < sb->n + extra)
    cap *= 2;
  char *buf = arena_alloc(cap);
  if (sb->n)
    memcpy(buf, sb->buf, sb->n);
  sb->buf = buf;
  sb->cap = cap;
}

void sb_push(SB *sb, char c) {
  sb_grow(sb, 1);
  sb->buf[sb->n++] = c;
}

void sb_append(SB *sb, Str s) {
  if (!s.n)
    return;
  sb_grow(sb, s.n);
  memcpy(sb->buf + sb->n, s.p, s.n);
  sb->n += s.n;
}

void sb_append_c(SB *sb, const char *s) { sb_append(sb, str_from(s)); }

void sb_printf(SB *sb, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  sb_grow(sb, (size_t)n);
  vsnprintf(sb->buf + sb->n, (size_t)n + 1, fmt, ap2);
  sb->n += (size_t)n;
  va_end(ap2);
}

Str sb_finish(SB *sb) {
  Str s = {sb->buf ? sb->buf : "", sb->n};
  return s;
}

// ------------------------------------------------------------------ vec ---

void vec_push(Vec *v, void *p) {
  if (v->n == v->cap) {
    size_t cap = v->cap ? v->cap * 2 : 8;
    void **items = arena_alloc(cap * sizeof(void *));
    if (v->n)
      memcpy(items, v->items, v->n * sizeof(void *));
    v->items = items;
    v->cap = cap;
  }
  v->items[v->n++] = p;
}

void *vec_pop(Vec *v) { return v->n ? v->items[--v->n] : NULL; }

void *vec_last(Vec *v) { return v->n ? v->items[v->n - 1] : NULL; }

// ------------------------------------------------------------------ map ---

typedef struct MapEnt {
  Str key;
  void *val;
  bool used;
} MapEnt;

size_t map_hash(Str s) {
  uint64_t h = 1469598103934665603ull; // FNV-1a, fixed seed: determinism
  for (size_t i = 0; i < s.n; i++) {
    h ^= (unsigned char)s.p[i];
    h *= 1099511628211ull;
  }
  return (size_t)h;
}

static void map_grow(Map *m) {
  size_t ncap = m->cap ? m->cap * 2 : 16;
  MapEnt *slots = arena_alloc_zeroed(ncap * sizeof(MapEnt));
  Map old = *m;
  m->slots = slots;
  m->cap = ncap;
  m->n = 0;
  m->keys = (Vec){0};
  for (size_t i = 0; old.cap && i < old.cap; i++) {
    if (old.slots[i].used)
      map_put(m, old.slots[i].key, old.slots[i].val);
  }
  // the rehash above re-appended keys in slot order, which REORDERS the
  // list; restore the original insertion order or any loop iterating
  // m->keys by index (check phase 2/3, lower) skips and double-visits
  // entries across the grow
  m->keys = old.keys;
}

void *map_get(const Map *m, Str key) {
  if (!m->cap)
    return NULL;
  size_t i = map_hash(key) & (m->cap - 1);
  while (m->slots[i].used) {
    if (str_eq(m->slots[i].key, key))
      return m->slots[i].val;
    i = (i + 1) & (m->cap - 1);
  }
  return NULL;
}

bool map_has(const Map *m, Str key) {
  if (!m->cap)
    return false;
  size_t i = map_hash(key) & (m->cap - 1);
  while (m->slots[i].used) {
    if (str_eq(m->slots[i].key, key))
      return true;
    i = (i + 1) & (m->cap - 1);
  }
  return false;
}

void map_put(Map *m, Str key, void *val) {
  if (m->n * 10 >= m->cap * 7)
    map_grow(m);
  size_t i = map_hash(key) & (m->cap - 1);
  while (m->slots[i].used) {
    if (str_eq(m->slots[i].key, key)) {
      m->slots[i].val = val;
      return;
    }
    i = (i + 1) & (m->cap - 1);
  }
  m->slots[i].used = true;
  m->slots[i].key = key;
  m->slots[i].val = val;
  m->n++;
  Str *k = arena_alloc(sizeof(Str));
  *k = key;
  vec_push(&m->keys, k);
}

// ------------------------------------------------------------------ diag ---

Vec diags = {0};
bool any_error = false;

void err_at(Str file, int line, int col, const char *fmt, ...) {
  Diag *d = arena_alloc(sizeof(Diag));
  d->file = file;
  d->line = line;
  d->col = col;
  va_list ap;
  va_start(ap, fmt);
  d->msg = arena_printf_(fmt, ap);
  va_end(ap);
  vec_push(&diags, d);
  any_error = true;
}

void err_range(Str file, int line, int col, int len, const char *fmt, ...) {
  (void)len;
  va_list ap;
  va_start(ap, fmt);
  err_at(file, line, col, "%s", arena_printf_(fmt, ap));
  va_end(ap);
}

// va_list helper shared by err_at/err_range wrappers.
char *arena_printf_(const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  char *p = arena_alloc((size_t)n + 1);
  vsnprintf(p, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  return p;
}

void render_diags(SB *sb) {
  for (size_t i = 0; i < diags.n; i++) {
    Diag *d = diags.items[i];
    sb_printf(sb, "%.*s:%d:%d: error: %s\n", (int)d->file.n, d->file.p, d->line, d->col, d->msg);
  }
}

void flush_diags(void) {
  for (size_t i = 0; i < diags.n; i++) {
    Diag *d = diags.items[i];
    fprintf(stderr, "%.*s:%d:%d: error: %s\n", (int)d->file.n, d->file.p, d->line, d->col, d->msg);
  }
}

void clear_diags(void) { diags = (Vec){0}; any_error = false; }

int diag_count(void) { return (int)diags.n; }
