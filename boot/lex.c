// lex.c — the lexer. Full grammar per spec/syntax.md §2: keywords,
// integer literals (dec/hex/bin, underscores), floats with exponents,
// the exact escape set for single-line strings, fully verbatim
// triple-quoted strings, all operators.
#include "rho.h"

#include <ctype.h>
#include <errno.h>

typedef struct Lexer {
  const char *path; // interned
  const char *src;
  size_t pos, len;
  int line, col;
  Vec toks; // of Token
} Lexer;

static const struct { const char *spell; TokKind kind; } k_keywords[] = {
    {"as", K_AS},         {"break", K_BREAK},   {"const", K_CONST},
    {"continue", K_CONTINUE}, {"defer", K_DEFER}, {"dyn", K_DYN},
    {"else", K_ELSE},     {"enum", K_ENUM},     {"extern", K_EXTERN},
    {"false", K_FALSE},   {"fn", K_FN},         {"for", K_FOR},
    {"if", K_IF},         {"impl", K_IMPL},     {"let", K_LET},
    {"loop", K_LOOP},     {"match", K_MATCH},   {"mut", K_MUT},
    {"new", K_NEW},       {"null", K_NULL},     {"pub", K_PUB},
    {"return", K_RETURN}, {"static", K_STATIC}, {"struct", K_STRUCT},
    {"trait", K_TRAIT},   {"true", K_TRUE},     {"use", K_USE},
    {"while", K_WHILE},
    {"i8", K_I8},         {"i16", K_I16},       {"i32", K_I32},
    {"i64", K_I64},       {"u8", K_U8},         {"u16", K_U16},
    {"u32", K_U32},       {"u64", K_U64},       {"usize", K_USIZE},
    {"f32", K_F32},       {"f64", K_F64},       {"bool", K_BOOL},
    {"string", K_STRING},
};

static char peek(Lexer *lx) {
  return lx->pos < lx->len ? lx->src[lx->pos] : 0;
}
static char peek2(Lexer *lx) {
  return lx->pos + 1 < lx->len ? lx->src[lx->pos + 1] : 0;
}
static char advance(Lexer *lx) {
  char c = lx->src[lx->pos++];
  if (c == '\n') {
    lx->line++;
    lx->col = 1;
  } else {
    lx->col++;
  }
  return c;
}

static void push_tok(Lexer *lx, TokKind kind) {
  Token *t = vec_push(&lx->toks);
  t->kind = kind;
  t->file = lx->path;
  t->line = lx->line;
  t->col = lx->col;
}

static void push_tok_here(Lexer *lx, TokKind kind, int line, int col) {
  Token *t = vec_push(&lx->toks);
  t->kind = kind;
  t->file = lx->path;
  t->line = line;
  t->col = col;
}

static bool is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_ident_char(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}

// Decode one escape sequence at lx->pos (just past the backslash).
// Returns false on an invalid escape; *out receives the decoded bytes.
static bool decode_escape(Lexer *lx, Vec *out, const char **err) {
  char c = advance(lx);
  switch (c) {
  case 'n':
    *(unsigned char *)vec_push(out) = '\n';
    return true;
  case 'r':
    *(unsigned char *)vec_push(out) = '\r';
    return true;
  case 't':
    *(unsigned char *)vec_push(out) = '\t';
    return true;
  case '0':
    *(unsigned char *)vec_push(out) = 0;
    return true;
  case '\\':
    *(unsigned char *)vec_push(out) = '\\';
    return true;
  case '"':
    *(unsigned char *)vec_push(out) = '"';
    return true;
  case 'x': {
    int v = 0, digits = 0;
    while (digits < 2 && isxdigit((unsigned char)peek(lx))) {
      char h = advance(lx);
      v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
      digits++;
    }
    if (digits != 2) {
      *err = "\\x needs two hex digits";
      return false;
    }
    *(unsigned char *)vec_push(out) = (unsigned char)v;
    return true;
  }
  case 'u': {
    if (peek(lx) != '{') {
      *err = "\\u needs '{'";
      return false;
    }
    advance(lx);
    unsigned long cp = 0;
    int digits = 0;
    while (peek(lx) != '}') {
      char h = peek(lx);
      if (!isxdigit((unsigned char)h)) {
        *err = "\\u{…} needs hex digits";
        return false;
      }
      advance(lx);
      cp = cp * 16 + (unsigned long)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
      digits++;
      if (cp > 0x10FFFF) {
        *err = "\\u code point out of range";
        return false;
      }
    }
    advance(lx); // '}'
    if (digits == 0) {
      *err = "\\u{} needs at least one digit";
      return false;
    }
    // encode UTF-8
    if (cp < 0x80) {
      *(unsigned char *)vec_push(out) = (unsigned char)cp;
    } else if (cp < 0x800) {
      *(unsigned char *)vec_push(out) = (unsigned char)(0xC0 | (cp >> 6));
      *(unsigned char *)vec_push(out) = (unsigned char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      *(unsigned char *)vec_push(out) = (unsigned char)(0xE0 | (cp >> 12));
      *(unsigned char *)vec_push(out) =
          (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
      *(unsigned char *)vec_push(out) = (unsigned char)(0x80 | (cp & 0x3F));
    } else {
      *(unsigned char *)vec_push(out) = (unsigned char)(0xF0 | (cp >> 18));
      *(unsigned char *)vec_push(out) =
          (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
      *(unsigned char *)vec_push(out) =
          (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
      *(unsigned char *)vec_push(out) = (unsigned char)(0x80 | (cp & 0x3F));
    }
    return true;
  }
  default:
    *err = "unknown escape";
    return false;
  }
}

static void lex_string(Lexer *lx) {
  int line = lx->line, col = lx->col;
  // caller confirmed the opening quote
  advance(lx); // opening "
  // triple-quoted?
  if (peek(lx) == '"' && peek2(lx) == '"') {
    advance(lx);
    advance(lx);
    // fully verbatim: copy bytes until the first """ — no escapes
    size_t start = lx->pos;
    while (lx->pos < lx->len) {
      if (peek(lx) == '"' && peek2(lx) == '"' &&
          lx->pos + 2 < lx->len && lx->src[lx->pos + 2] == '"') {
        size_t n = lx->pos - start;
        Token *t = vec_push(&lx->toks);
        t->kind = T_TSTRING;
        t->file = lx->path;
        t->line = line;
        t->col = col;
        t->text.p = lx->src + start;
        t->text.n = n;
        advance(lx);
        advance(lx);
        advance(lx);
        return;
      }
      advance(lx);
    }
    diag_at(DIAG_ERROR, lx->path, line, col,
            "unterminated triple-quoted string");
    push_tok_here(lx, T_TSTRING, line, col);
    Token *bad = VAT(lx->toks, Token, vec_len(&lx->toks) - 1);
    bad->text.p = "";
    bad->text.n = 0;
    return;
  }
  // single-line string with escapes
  VEC(unsigned char, out);
  const char *err = NULL;
  while (peek(lx) != '"') {
    char c = peek(lx);
    if (c == 0 || c == '\n') {
      diag_at(DIAG_ERROR, lx->path, lx->line, lx->col,
              "unterminated string literal");
      break;
    }
    if (c == '\\') {
      advance(lx);
      if (!decode_escape(lx, &out, &err)) {
        diag_at(DIAG_ERROR, lx->path, lx->line, lx->col, "%s", err);
        // resync: skip to closing quote on this line
        while (peek(lx) != '"' && peek(lx) != '\n' && peek(lx) != 0)
          advance(lx);
        break;
      }
      continue;
    }
    *(unsigned char *)vec_push(&out) = (unsigned char)advance(lx);
  }
  if (peek(lx) == '"')
    advance(lx); // closing
  Token *t = vec_push(&lx->toks);
  t->kind = T_STRING;
  t->file = lx->path;
  t->line = line;
  t->col = col;
  // copy into arena so the token owns its bytes
  unsigned char *bytes = arena_alloc(g_arena, vec_len(&out) + 1, 1);
  if (vec_len(&out))
    memcpy(bytes, VAT(out, unsigned char, 0), vec_len(&out));
  bytes[vec_len(&out)] = 0;
  t->text.p = (const char *)bytes;
  t->text.n = vec_len(&out);
  free(out.data);
}

static void lex_number(Lexer *lx) {
  int line = lx->line, col = lx->col;
  const char *start = lx->src + lx->pos;
  size_t n0 = lx->pos;

  // hex / bin
  if (peek(lx) == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
    advance(lx);
    advance(lx);
    size_t digits = 0;
    uint64_t v = 0;
    bool over = false;
    while (isxdigit((unsigned char)peek(lx)) || peek(lx) == '_') {
      char c = advance(lx);
      if (c == '_')
        continue;
      int d = c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
      if (v > (UINT64_MAX - (unsigned)d) / 16)
        over = true;
      v = v * 16 + (uint64_t)d;
      digits++;
    }
    if (digits == 0 || is_ident_char(peek(lx))) {
      diag_at(DIAG_ERROR, lx->path, line, col,
              "malformed hex literal");
    }
    if (over) {
      diag_at(DIAG_ERROR, lx->path, line, col,
              "integer literal too large");
      v = UINT64_MAX;
    }
    Token *t = vec_push(&lx->toks);
    t->kind = T_INT;
    t->file = lx->path;
    t->line = line;
    t->col = col;
    t->i = v;
    return;
  }
  if (peek(lx) == '0' && (peek2(lx) == 'b' || peek2(lx) == 'B')) {
    advance(lx);
    advance(lx);
    size_t digits = 0;
    uint64_t v = 0;
    while (peek(lx) == '0' || peek(lx) == '1' || peek(lx) == '_') {
      char c = advance(lx);
      if (c == '_')
        continue;
      v = v * 2 + (uint64_t)(c - '0');
      digits++;
    }
    if (digits == 0 || is_ident_char(peek(lx)))
      diag_at(DIAG_ERROR, lx->path, line, col, "malformed binary literal");
    Token *t = vec_push(&lx->toks);
    t->kind = T_INT;
    t->file = lx->path;
    t->line = line;
    t->col = col;
    t->i = v;
    return;
  }

  bool is_float = false;
  while (isdigit((unsigned char)peek(lx)) || peek(lx) == '_')
    advance(lx);
  if (peek(lx) == '.' && isdigit((unsigned char)peek2(lx))) {
    is_float = true;
    advance(lx); // .
    while (isdigit((unsigned char)peek(lx)) || peek(lx) == '_')
      advance(lx);
  }
  if (peek(lx) == 'e' || peek(lx) == 'E') {
    size_t save = lx->pos;
    int sline = lx->line, scol = lx->col;
    advance(lx);
    if (peek(lx) == '+' || peek(lx) == '-')
      advance(lx);
    if (!isdigit((unsigned char)peek(lx))) {
      // not an exponent after all; rewind (e.g. `2e` then ident)
      lx->pos = save;
      lx->line = sline;
      lx->col = scol;
    } else {
      is_float = true;
      while (isdigit((unsigned char)peek(lx)) || peek(lx) == '_')
        advance(lx);
    }
  }
  if (is_ident_start(peek(lx))) {
    diag_at(DIAG_ERROR, lx->path, line, col, "malformed number literal");
    while (is_ident_char(peek(lx)))
      advance(lx);
  }

  // strip underscores for strtod
  size_t n = lx->pos - n0;
  char *clean = arena_alloc(g_arena, n + 1, 1);
  size_t j = 0;
  for (size_t i = 0; i < n; i++)
    if (start[i] != '_')
      clean[j++] = start[i];
  clean[j] = 0;

  Token *t = vec_push(&lx->toks);
  t->kind = is_float ? T_FLOAT : T_INT;
  t->file = lx->path;
  t->line = line;
  t->col = col;
  if (is_float) {
    t->f = strtod(clean, NULL);
  } else {
    errno = 0;
    unsigned long long v = strtoull(clean, NULL, 10);
    if (errno == ERANGE) {
      diag_at(DIAG_ERROR, lx->path, line, col, "integer literal too large");
      v = UINT64_MAX;
    }
    t->i = (uint64_t)v;
  }
}

static void lex_ident(Lexer *lx) {
  int line = lx->line, col = lx->col;
  size_t start = lx->pos;
  while (is_ident_char(peek(lx)))
    advance(lx);
  size_t n = lx->pos - start;
  const char *spelling = intern(lx->src + start, n);
  for (size_t i = 0;
       i < sizeof(k_keywords) / sizeof(k_keywords[0]); i++) {
    if (spelling[0] == k_keywords[i].spell[0] &&
        strcmp(spelling, k_keywords[i].spell) == 0) {
      push_tok_here(lx, k_keywords[i].kind, line, col);
      return;
    }
  }
  Token *t = vec_push(&lx->toks);
  t->kind = T_IDENT;
  t->file = lx->path;
  t->line = line;
  t->col = col;
  t->text.p = spelling;
  t->text.n = n;
}

static void lex_oper(Lexer *lx) {
  int line = lx->line, col = lx->col;
  char c = advance(lx);
  char c2 = peek(lx);
  TokKind k = T_EOF;
  switch (c) {
  case '(':
    k = T_LPAREN;
    break;
  case ')':
    k = T_RPAREN;
    break;
  case '{':
    k = T_LBRACE;
    break;
  case '}':
    k = T_RBRACE;
    break;
  case '[':
    k = T_LBRACK;
    break;
  case ']':
    k = T_RBRACK;
    break;
  case ',':
    k = T_COMMA;
    break;
  case ';':
    k = T_SEMI;
    break;
  case ':':
    k = T_COLON;
    break;
  case '.':
    if (c2 == '.') {
      advance(lx);
      if (peek(lx) == '.') {
        advance(lx);
        k = T_ELLIPSIS;
      } else {
        k = T_DOTDOT;
      }
    } else {
      k = T_DOT;
    }
    break;
  case '~':
    k = T_TILDE;
    break;
  case '+':
    k = c2 == '=' ? (advance(lx), T_PLUSEQ) : T_PLUS;
    break;
  case '-':
    if (c2 == '>') {
      advance(lx);
      k = T_ARROW;
    } else if (c2 == '=') {
      advance(lx);
      k = T_DASHEQ;
    } else {
      k = T_DASH;
    }
    break;
  case '*':
    k = c2 == '=' ? (advance(lx), T_STAREQ) : T_STAR;
    break;
  case '/':
    k = c2 == '=' ? (advance(lx), T_SLASHEQ) : T_SLASH;
    break;
  case '%':
    k = c2 == '=' ? (advance(lx), T_PCTEQ) : T_PERCENT;
    break;
  case '&':
    if (c2 == '&') {
      advance(lx);
      k = T_ANDAND;
    } else if (c2 == '=') {
      advance(lx);
      k = T_AMPEQ;
    } else {
      k = T_AMP;
    }
    break;
  case '|':
    if (c2 == '|') {
      advance(lx);
      k = T_OROR;
    } else if (c2 == '=') {
      advance(lx);
      k = T_PIPEEQ;
    } else {
      k = T_PIPE;
    }
    break;
  case '^':
    k = c2 == '=' ? (advance(lx), T_CARETEQ) : T_CARET;
    break;
  case '!':
    k = c2 == '=' ? (advance(lx), T_NE) : T_BANG;
    break;
  case '?':
    k = T_QUESTION;
    break;
  case '<':
    if (c2 == '<') {
      advance(lx);
      k = peek(lx) == '=' ? (advance(lx), T_SHLEQ) : T_SHL;
    } else {
      k = c2 == '=' ? (advance(lx), T_LE) : T_LT;
    }
    break;
  case '>':
    if (c2 == '>') {
      advance(lx);
      k = peek(lx) == '=' ? (advance(lx), T_SHREQ) : T_SHR;
    } else {
      k = c2 == '=' ? (advance(lx), T_GE) : T_GT;
    }
    break;
  case '=':
    if (c2 == '=') {
      advance(lx);
      k = T_EQEQ;
    } else if (c2 == '>') {
      advance(lx);
      k = T_FATARROW;
    } else {
      k = T_EQ;
    }
    break;
  default:
    diag_at(DIAG_ERROR, lx->path, line, col, "unexpected character '%c'", c);
    return; // skip
  }
  push_tok_here(lx, k, line, col);
}

Lexer *lex_file(Arena *a, const char *path, const char *src) {
  (void)a;
  Lexer *lx = calloc(1, sizeof(Lexer));
  lx->path = intern_c(path);
  lx->src = src;
  lx->len = strlen(src);
  lx->line = 1;
  lx->col = 1;
  vec_init(&lx->toks, sizeof(Token));

  while (lx->pos < lx->len) {
    char c = peek(lx);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance(lx);
      continue;
    }
    if (c == '/' && peek2(lx) == '/') {
      while (peek(lx) != '\n' && peek(lx) != 0)
        advance(lx);
      continue;
    }
    if (c == '"') {
      lex_string(lx);
      continue;
    }
    if (isdigit((unsigned char)c)) {
      lex_number(lx);
      continue;
    }
    if (is_ident_start(c)) {
      lex_ident(lx);
      continue;
    }
    lex_oper(lx);
  }
  push_tok(lx, T_EOF);
  return lx;
}

const Token *lex_tokens(const Lexer *lx, size_t *n) {
  *n = vec_len(&lx->toks);
  return (const Token *)lx->toks.data;
}
