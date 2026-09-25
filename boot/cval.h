// cval.h — the comptime value (shared by the folder and the emitter).
#ifndef RHO_CVAL_H
#define RHO_CVAL_H

struct Type;

typedef enum { CV_INT, CV_UINT, CV_FLOAT, CV_BOOL, CV_STR } CVKind;

typedef struct CVal {
  CVKind kind;
  struct Type *ty; // the comptime type (builtin only)
  int64_t i;
  uint64_t u;
  double f;
  bool b;
  Str s;
} CVal;

#endif
