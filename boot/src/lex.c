#include "rho.h"
#include <ctype.h>

typedef struct Lexer {
  Str file;
  const char *p, *end;
  int line, col;
  Vec *out;
  Vec pending; // char* line comments waiting for the next token
} Lexer;

static const struct {
  const char *word;
  Tok kind;
} KEYWORDS[] = {
    {"fn", KW_FN},         {"let", KW_LET},       {"mut", KW_MUT},
    {"if", KW_IF},         {"else", KW_ELSE},     {"while", KW_WHILE},
    {"loop", KW_LOOP},     {"break", KW_BREAK},   {"continue", KW_CONTINUE},
    {"return", KW_RETURN}, {"defer", KW_DEFER},   {"struct", KW_STRUCT},
    {"enum", KW_ENUM},     {"use", KW_USE},       {"pub", KW_PUB},
    {"static", KW_STATIC}, {"const", KW_CONST},   {"match", KW_MATCH},
    {"as", KW_AS},         {"new", KW_NEW},       {"null", KW_NULL},
    {"true", KW_TRUE},     {"false", KW_FALSE},   {"weak", KW_WEAK},
    {"self", KW_SELF},     {"extern", KW_EXTERN},
};

static Tok ident_or_keyword(Str s) {
  for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++)
    if (str_eq_c(s, KEYWORDS[i].word))
      return KEYWORDS[i].kind;
  return TK_IDENT;
}

static Token *tok_new(Lexer *lx, Tok kind, const char *start) {
  Token *t = arena_alloc_zeroed(sizeof(Token));
  t->kind = kind;
  t->text = str_from_len(start, lx->p - start);
  t->file = lx->file;
  t->line = lx->line;
  t->col = lx->col;
  t->pre = lx->pending;
  lx->pending = (Vec){0};
  vec_push(lx->out, t);
  return t;
}

static void lex_error(Lexer *lx, const char *msg) {
  err_at(lx->file, lx->line, lx->col, "%s", msg);
}

static bool is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// digit value in the given base, or -1
static int digit_val(char c, uint64_t base) {
  int d;
  if (c >= '0' && c <= '9')
    d = c - '0';
  else if (c >= 'a' && c <= 'f')
    d = c - 'a' + 10;
  else if (c >= 'A' && c <= 'F')
    d = c - 'A' + 10;
  else
    return -1;
  if ((uint64_t)d >= base)
    return -1;
  return d;
}

void lex_file(Str file, Str src, Vec *out_tokens) {
  Lexer lx = {file, src.p, src.p + src.n, 1, 1, out_tokens, {0}};
  while (lx.p < lx.end) {
    char c = *lx.p;
    const char *start = lx.p;
    int start_line = lx.line, start_col = lx.col;

    if (c == '\n') {
      lx.p++;
      lx.line++;
      lx.col = 1;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      lx.p++;
      lx.col++;
      continue;
    }
    if (c == '/' && lx.p + 1 < lx.end && lx.p[1] == '/') {
      // keep line comments as trivia on the next token so fmt can re-emit
      lx.p += 2;
      const char *cs = lx.p;
      while (lx.p < lx.end && *lx.p != '\n')
        lx.p++;
      vec_push(&lx.pending, arena_strndup(cs, lx.p - cs));
      continue;
    }

    // identifiers & keywords
    if (is_ident_start(c)) {
      while (lx.p < lx.end && is_ident_char(*lx.p))
        lx.p++;
      Token *t = tok_new(&lx, ident_or_keyword(str_from_len(start, lx.p - start)), start);
      t->line = start_line;
      t->col = start_col;
      lx.col += (int)(lx.p - start);
      continue;
    }

    // numbers (int / float / hex / bin)
    if (is_digit(c)) {
      bool is_float = false;
      uint64_t base = 10;
      if (c == '0' && lx.p + 1 < lx.end && (lx.p[1] == 'x' || lx.p[1] == 'b')) {
        base = lx.p[1] == 'x' ? 16 : 2;
        lx.p += 2;
      }
      const char *digits = lx.p;
      while (lx.p < lx.end && (*lx.p == '_' || digit_val(*lx.p, base) >= 0))
        lx.p++;
      if (base == 10 && lx.p + 1 < lx.end && lx.p[0] == '.' && is_digit(lx.p[1])) {
        is_float = true;
        lx.p++; // '.'
        while (lx.p < lx.end && (is_digit(*lx.p) || *lx.p == '_'))
          lx.p++;
      }
      if (base == 10 && lx.p < lx.end && (*lx.p == 'e' || *lx.p == 'E')) {
        const char *save = lx.p;
        lx.p++;
        if (lx.p < lx.end && (*lx.p == '+' || *lx.p == '-'))
          lx.p++;
        if (lx.p < lx.end && is_digit(*lx.p)) {
          is_float = true;
          while (lx.p < lx.end && is_digit(*lx.p))
            lx.p++;
        } else {
          lx.p = save; // not an exponent after all
        }
      }
      if (lx.p == digits) {
        lex_error(&lx, "malformed number");
        lx.p++;
        lx.col++;
        continue;
      }
      Token *t = tok_new(&lx, is_float ? TK_FLOAT : TK_INT, start);
      t->line = start_line;
      t->col = start_col;
      if (is_float) {
        SB clean = {0};
        for (const char *q = start; q < lx.p; q++)
          if (*q != '_')
            sb_push(&clean, *q);
        sb_push(&clean, 0);
        t->fv = strtod(clean.buf, NULL);
      } else {
        uint64_t v = 0;
        bool overflow = false;
        for (const char *q = digits; q < lx.p; q++) {
          if (*q == '_')
            continue;
          int d = digit_val(*q, base);
          if (d < 0) {
            err_at(file, start_line, start_col, "invalid digit for this base");
            break;
          }
          uint64_t next = v * base + (uint64_t)d;
          if (next < v)
            overflow = true;
          v = next;
        }
        if (overflow)
          err_at(file, start_line, start_col, "integer literal too large");
        t->iv = v;
      }
      lx.col += (int)(lx.p - start);
      continue;
    }

    // strings
    if (c == '"') {
      lx.p++;
      SB sb = {0};
      for (;;) {
        if (lx.p >= lx.end || *lx.p == '\n') {
          lex_error(&lx, "unterminated string literal");
          break;
        }
        char d = *lx.p;
        if (d == '"') {
          lx.p++;
          break;
        }
        if (d == '\\') {
          lx.p++;
          if (lx.p >= lx.end) {
            lex_error(&lx, "unterminated escape");
            break;
          }
          char e = *lx.p++;
          switch (e) {
          case 'n': sb_push(&sb, '\n'); break;
          case 't': sb_push(&sb, '\t'); break;
          case 'r': sb_push(&sb, '\r'); break;
          case '\\': sb_push(&sb, '\\'); break;
          case '"': sb_push(&sb, '"'); break;
          case '0': sb_push(&sb, '\0'); break;
          case 'x': {
            if (lx.p + 1 < lx.end && isxdigit(lx.p[0]) && isxdigit(lx.p[1])) {
              char hex[3] = {lx.p[0], lx.p[1], 0};
              sb_push(&sb, (char)strtol(hex, NULL, 16));
              lx.p += 2;
            } else {
              lex_error(&lx, "malformed \\x escape");
            }
            break;
          }
          default:
            lex_error(&lx, "unknown escape");
            sb_push(&sb, e);
          }
          continue;
        }
        sb_push(&sb, d);
        lx.p++;
      }
      Token *t = tok_new(&lx, TK_STR, start);
      t->line = start_line;
      t->col = start_col;
      t->text = sb_finish(&sb); // decoded contents
      lx.col += (int)(lx.p - start);
      continue;
    }

    // multi-char operators first (longest match: `...` before `..`)
#define OP3(a, b, c3, k)                                                         \
  if (c == a && lx.p + 2 < lx.end && lx.p[1] == b && lx.p[2] == c3) {            \
    lx.p += 3;                                                                   \
    lx.col += 3;                                                                 \
    tok_new(&lx, k, start);                                                      \
    continue;                                                                    \
  }
    OP3('.', '.', '.', P_ELLIPSIS3)
#undef OP3
#define OP2(a, b, k)                                                           \
  if (c == a && lx.p + 1 < lx.end && lx.p[1] == b) {                           \
    lx.p += 2;                                                                 \
    lx.col += 2;                                                               \
    tok_new(&lx, k, start);                                                    \
    continue;                                                                  \
  }
    OP2('=', '=', P_EQ)
    OP2('!', '=', P_NE)
    OP2('<', '=', P_LE)
    OP2('>', '=', P_GE)
    OP2('&', '&', P_ANDAND)
    OP2('|', '|', P_OROR)
    OP2('&', '=', P_AMPEQ)
    OP2('|', '=', P_PIPEEQ)
    OP2('^', '=', P_CARETEQ)
    OP2('+', '=', P_PLUSEQ)
    OP2('-', '=', P_MINUSEQ)
    OP2('*', '=', P_STAREQ)
    OP2('/', '=', P_SLASHEQ)
    OP2('%', '=', P_PERCENTEQ)
    OP2('=', '>', P_FATARROW)
    OP2('-', '>', P_ARROW)
    OP2('.', '.', P_ELLIPSIS2)
    OP2('<', '<', P_SHL)
    OP2('>', '>', P_SHR)
#undef OP2

    Tok kind = TK_EOF;
    switch (c) {
    case '(': kind = P_LPAREN; break;
    case ')': kind = P_RPAREN; break;
    case '{': kind = P_LBRACE; break;
    case '}': kind = P_RBRACE; break;
    case '[': kind = P_LBRACKET; break;
    case ']': kind = P_RBRACKET; break;
    case ',': kind = P_COMMA; break;
    case ':': kind = P_COLON; break;
    case ';': kind = P_SEMI; break;
    case '.': kind = P_DOT; break;
    case '+': kind = P_PLUS; break;
    case '-': kind = P_MINUS; break;
    case '*': kind = P_STAR; break;
    case '/': kind = P_SLASH; break;
    case '%': kind = P_PERCENT; break;
    case '!': kind = P_BANG; break;
    case '~': kind = P_TILDE; break;
    case '&': kind = P_AMP; break;
    case '|': kind = P_PIPE; break;
    case '^': kind = P_CARET; break;
    case '<': kind = P_LT; break;
    case '>': kind = P_GT; break;
    case '=': kind = P_ASSIGN; break;
    case '?': kind = P_QMARK; break;
    default:
      lex_error(&lx, "unexpected character");
      lx.p++;
      lx.col++;
      continue;
    }
    lx.p++;
    lx.col++;
    tok_new(&lx, kind, start);
  }
  Token *eof = arena_alloc_zeroed(sizeof(Token));
  eof->kind = TK_EOF;
  eof->text = str_from("");
  eof->pre = lx.pending;
  eof->file = file;
  eof->line = lx.line;
  eof->col = lx.col;
  vec_push(out_tokens, eof);
}

const char *tok_name(Tok t) {
  switch (t) {
  case TK_EOF: return "end of file";
  case TK_IDENT: return "identifier";
  case TK_INT: return "integer";
  case TK_FLOAT: return "float";
  case TK_STR: return "string";
  case KW_FN: return "`fn`";
  case KW_LET: return "`let`";
  case KW_MUT: return "`mut`";
  case KW_IF: return "`if`";
  case KW_ELSE: return "`else`";
  case KW_WHILE: return "`while`";
  case KW_LOOP: return "`loop`";
  case KW_BREAK: return "`break`";
  case KW_CONTINUE: return "`continue`";
  case KW_RETURN: return "`return`";
  case KW_DEFER: return "`defer`";
  case KW_STRUCT: return "`struct`";
  case KW_ENUM: return "`enum`";
  case KW_USE: return "`use`";
  case KW_PUB: return "`pub`";
  case KW_STATIC: return "`static`";
  case KW_CONST: return "`const`";
  case KW_MATCH: return "`match`";
  case KW_AS: return "`as`";
  case KW_NEW: return "`new`";
  case KW_NULL: return "`null`";
  case KW_TRUE: return "`true`";
  case KW_FALSE: return "`false`";
  case KW_WEAK: return "`weak`";
  case KW_SELF: return "`self`";
  case KW_EXTERN: return "`extern`";
  case P_LPAREN: return "`(`";
  case P_RPAREN: return "`)`";
  case P_LBRACE: return "`{`";
  case P_RBRACE: return "`}`";
  case P_LBRACKET: return "`[`";
  case P_RBRACKET: return "`]`";
  case P_COMMA: return "`,`";
  case P_COLON: return "`:`";
  case P_SEMI: return "`;`";
  case P_DOT: return "`.`";
  case P_ARROW: return "`->`";
  case P_FATARROW: return "`=>`";
  case P_ELLIPSIS2: return "`..`";
  case P_ELLIPSIS3: return "`...`";
  case P_PLUS: return "`+`";
  case P_MINUS: return "`-`";
  case P_STAR: return "`*`";
  case P_SLASH: return "`/`";
  case P_PERCENT: return "`%`";
  case P_BANG: return "`!`";
  case P_TILDE: return "`~`";
  case P_AMP: return "`&`";
  case P_PIPE: return "`|`";
  case P_CARET: return "`^`";
  case P_SHL: return "`<<`";
  case P_SHR: return "`>>`";
  case P_ANDAND: return "`&&`";
  case P_OROR: return "`||`";
  case P_EQ: return "`==`";
  case P_NE: return "`!=`";
  case P_LT: return "`<`";
  case P_GT: return "`>`";
  case P_LE: return "`<=`";
  case P_GE: return "`>=`";
  case P_ASSIGN: return "`=`";
  case P_PLUSEQ: return "`+=`";
  case P_MINUSEQ: return "`-=`";
  case P_STAREQ: return "`*=`";
  case P_SLASHEQ: return "`/=`";
  case P_PERCENTEQ: return "`%=`";
  case P_AMPEQ: return "`&=`";
  case P_PIPEEQ: return "`|=`";
  case P_CARETEQ: return "`^=`";
  case P_SHLEQ: return "`<<=`";
  case P_SHREQ: return "`>>=`";
  case P_QMARK: return "`?`";
  }
  return "?";
}

const char *tok_spell(Tok t) {
  switch (t) {
  case P_PLUS: return "+";
  case P_PLUSEQ: return "+=";
  case P_MINUS: return "-";
  case P_MINUSEQ: return "-=";
  case P_STAR: return "*";
  case P_STAREQ: return "*=";
  case P_SLASH: return "/";
  case P_SLASHEQ: return "/=";
  case P_PERCENT: return "%";
  case P_PERCENTEQ: return "%=";
  case P_BANG: return "!";
  case P_TILDE: return "~";
  case P_AMP: return "&";
  case P_AMPEQ: return "&=";
  case P_PIPE: return "|";
  case P_PIPEEQ: return "|=";
  case P_CARET: return "^";
  case P_CARETEQ: return "^=";
  case P_SHL: return "<<";
  case P_SHLEQ: return "<<=";
  case P_SHR: return ">>";
  case P_SHREQ: return ">>=";
  case P_ANDAND: return "&&";
  case P_OROR: return "||";
  case P_EQ: return "==";
  case P_NE: return "!=";
  case P_LT: return "<";
  case P_GT: return ">";
  case P_LE: return "<=";
  case P_GE: return ">=";
  case P_ASSIGN: return "=";
  case P_ELLIPSIS2: return "..";
  case P_ELLIPSIS3: return "...";
  case P_ARROW: return "->";
  case P_FATARROW: return "=>";
  case P_COLON: return ":";
  case P_SEMI: return ";";
  case P_COMMA: return ",";
  case P_DOT: return ".";
  case P_LPAREN: return "(";
  case P_RPAREN: return ")";
  case P_LBRACE: return "{";
  case P_RBRACE: return "}";
  case P_LBRACKET: return "[";
  case P_RBRACKET: return "]";
  case P_QMARK: return "?";
  default: return tok_name(t);
  }
}
