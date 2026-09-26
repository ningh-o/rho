// test.c — the rho test verb (T3.5, spec §17). Two test forms, one
// verb:
//
//   file tests    a `*_test.rho` file is a program: fn main runs,
//                 exit 0 passes; `// out:` / `// exit:` / `// set:`
//                 headers pin behavior, `// expect:` demands a check
//                 diagnostic naming every substring, `// err:` pins
//                 stderr substrings (the panic catalog), `// pending:`
//                 marks the expected-fail ledger for ratified law not
//                 yet implemented (a pending case that passes prints
//                 PROMOTE and counts as a failure until the marker
//                 comes off).
//   block tests   a top-level `test "name" { ... }` is one test: the
//                 program is rebuilt per test with only that block
//                 emitted (the selection rides the emission side),
//                 judged by panic (nonzero) versus clean return (0).
//
// A directory argument picks up `*_test.rho` files, block-carrying
// files, and case directories (a directory holding a `main.rho` entry,
// whose headers it carries), recursively through area directories,
// name sorted. The verb exits 1 on any failure — including when
// nothing matched: a fake green is a red. Every spawned run sits
// under a hard time cap (fork + poll + kill; never an unbounded wait).
#include "rho.h"
#include "sem.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// per-test hard cap (seconds): compile + assemble + run
#define TEST_CAP_SECONDS 10

extern bool g_test_main_optional; // driver.c
extern const char *g_emit_test_name; // emit.c
int emit_program(Program *p, bool debug, char **wat_out, size_t *wat_len);

static char *read_file_all(const char *path, size_t *n_out) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  char *buf = malloc((size_t)sz + 1);
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[got] = 0;
  *n_out = got;
  return buf;
}

// ---------------------------------------------------------------- cases

typedef enum { TK_FILE, TK_BLOCK, TK_FORCE } TestKind;

typedef struct {
  const char *name;  // display name, unique across the run
  const char *path;  // the .rho file to compile
  const char *block; // TK_BLOCK: the entry-symbol name ("test:<name>")
  TestKind kind;
} TestCase;

static Vec g_tests; // of TestCase

// -------------------------------------------------------------- headers

typedef struct {
  Vec out;    // of const char* — expected stdout lines (each + '\n')
  bool has_exit;
  long exit_code;
  Vec sets;   // of const char* — "name=value"
  Vec expect; // of const char* — check must fail naming each
  Vec err;    // of const char* — stderr must contain each
  const char *pending; // task id, or NULL
} Headers;

static char *xstrdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  memcpy(p, s, n);
  return p;
}

static void parse_headers(const char *path, Headers *h) {
  size_t n = 0;
  char *text = read_file_all(path, &n);
  if (!text)
    return;
  const char *s = text;
  while (*s) {
    const char *eol = strchr(s, '\n');
    size_t len = eol ? (size_t)(eol - s) : strlen(s);
    // a NUL-terminated copy of this line (no '\n'), so header text
    // never runs past the line into the rest of the file
    char *line = xstrdup(s);
    line[len] = 0;
    if (len > 3 && line[0] == '/' && line[1] == '/' && line[2] == ' ') {
      const char *rest = line + 3;
      size_t rlen = len - 3;
      if (rlen > 5 && strncmp(rest, "out: ", 5) == 0) {
        *VPUSH(h->out, const char *) = xstrdup(rest + 5);
      } else if (rlen > 6 && strncmp(rest, "exit: ", 6) == 0) {
        h->has_exit = true;
        h->exit_code = strtol(rest + 6, NULL, 10);
      } else if (rlen > 5 && strncmp(rest, "set: ", 5) == 0) {
        *VPUSH(h->sets, const char *) = xstrdup(rest + 5);
      } else if (rlen > 8 && strncmp(rest, "expect: ", 8) == 0) {
        *VPUSH(h->expect, const char *) = xstrdup(rest + 8);
      } else if (rlen > 5 && strncmp(rest, "err: ", 5) == 0) {
        *VPUSH(h->err, const char *) = xstrdup(rest + 5);
      } else if (rlen >= 9 && strncmp(rest, "pending: ", 9) == 0) {
        const char *p = rest + 9;
        size_t pl = strlen(p);
        while (pl > 0 && (p[pl - 1] == ' ' || p[pl - 1] == '\r'))
          pl--;
        char *t = xstrdup(p);
        t[pl] = 0;
        h->pending = t;
      }
    }
    free(line);
    if (!eol)
      break;
    s = eol + 1;
  }
  free(text);
}

// ------------------------------------------------------------- diagnose

static void diags_reset(void) {
  vec_clear(&g_diags);
  g_had_error = false;
}

static char *capture_diags(void) {
  char *buf = NULL;
  size_t len = 0;
  FILE *ms = open_memstream(&buf, &len);
  if (!ms)
    return NULL;
  diags_print(ms);
  fclose(ms);
  if (!buf)
    buf = xstrdup("");
  return buf;
}

// front half shared by every per-test compile: fresh program, the
// case's --set headers applied, full check. Returns NULL (with the
// diagnostics captured) on any failure.
static Program *compile_case(const char *path, const Headers *h,
                             bool allow_no_main, char **diags_out) {
  diags_reset();
  Program *p = program_new();
  for (size_t i = 0; i < VLEN(h->sets); i++) {
    const char *nv = *VAT(h->sets, const char *, i);
    char *eq = strchr(nv, '=');
    if (!eq)
      continue;
    SetOverride *so = vec_push(&p->sets);
    so->name = intern(nv, (size_t)(eq - nv));
    so->value = eq + 1;
  }
  if (!program_load_graph(p, path)) {
    *diags_out = capture_diags();
    return NULL;
  }
  extern bool g_set_refused;
  g_test_main_optional = allow_no_main;
  bool ok = check_program(p);
  g_test_main_optional = false;
  if (g_set_refused || !ok) {
    *diags_out = capture_diags();
    return NULL;
  }
  *diags_out = capture_diags();
  return p;
}

// ------------------------------------------------------------------ run

// spawn `sh -c cmd` with stdout/stderr redirected into files; poll
// under the hard cap, kill on overrun. Returns the exit status, or
// -1 on timeout / spawn trouble.
static int run_capped(const char *cmd, const char *outp, const char *errp,
                      bool *timed_out) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    int ofd = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int efd = open(errp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ofd >= 0)
      dup2(ofd, 1);
    if (efd >= 0)
      dup2(efd, 2);
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }
  time_t start = time(NULL);
  int st = 0;
  for (;;) {
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid)
      break;
    if (r < 0)
      return -1;
    if (time(NULL) - start >= TEST_CAP_SECONDS) {
      kill(pid, SIGKILL);
      waitpid(pid, &st, 0);
      *timed_out = true;
      break;
    }
    struct timespec ts = {0, 20 * 1000 * 1000}; // 20 ms poll
    nanosleep(&ts, NULL);
  }
  if (WIFEXITED(st))
    return WEXITSTATUS(st);
  return -1;
}

// emit + assemble + execute the program; returns the exit status and
// fills the stdout/stderr captures (malloc'd, NUL-terminated).
static int run_program(const char *wat, size_t wat_len, char **out_s,
                       size_t *out_n, char **err_s, size_t *err_n,
                       bool *timed_out) {
  char watp[] = "/tmp/rho-test-XXXXXX.wat";
  char wasmp[] = "/tmp/rho-test-XXXXXX.wasm";
  char outp[] = "/tmp/rho-test-XXXXXX.out";
  char errp[] = "/tmp/rho-test-XXXXXX.err";
  int fd = mkstemps(watp, 4);
  if (fd < 0)
    return -1;
  write(fd, wat, wat_len);
  close(fd);
  if (mkstemps(wasmp, 5) < 0 || mkstemps(outp, 4) < 0 ||
      mkstemps(errp, 4) < 0) {
    unlink(watp);
    return -1;
  }
  // wasmtime traps map to the panic catalog exactly as rho run does:
  // "wasm trap" / stack overflow -> "panic: <msg>" on stderr, exit 101
  char cmd[4096];
  snprintf(cmd, sizeof cmd,
           "wat2wasm '%s' -o '%s' 2>/dev/null && "
           "{ wasmtime run '%s' 2>/tmp/rho-trap.$$.err; rc=$?; "
           "  if grep -q -e 'wasm trap' -e 'stack overflow' "
           "/tmp/rho-trap.$$.err 2>/dev/null; then "
           "    msg=$(sed -n 's/.*wasm trap: //p' /tmp/rho-trap.$$.err "
           "| head -1); "
           "    [ -z \"$msg\" ] && msg='stack overflow'; "
           "    echo \"panic: $msg\"; "
           "    rm -f /tmp/rho-trap.$$.err; exit 101; "
           "  fi; "
           "  cat /tmp/rho-trap.$$.err >&2 2>/dev/null; "
           "  rm -f /tmp/rho-trap.$$.err; exit $rc; }; exit 1",
           watp, wasmp, wasmp);
  int rc = run_capped(cmd, outp, errp, timed_out);
  unlink(watp);
  unlink(wasmp);
  size_t n = 0;
  char *text = read_file_all(outp, &n);
  *out_s = text ? text : xstrdup("");
  *out_n = text ? n : 0;
  n = 0;
  text = read_file_all(errp, &n);
  *err_s = text ? text : xstrdup("");
  *err_n = text ? n : 0;
  unlink(outp);
  unlink(errp);
  return rc;
}

// --------------------------------------------------------------- judge

static bool all_present(const Vec *needles, const char *hay) {
  for (size_t i = 0; i < VLEN(*needles); i++)
    if (!strstr(hay, *VAT(*needles, const char *, i)))
      return false;
  return true;
}

static void print_head(const char *label, const char *text) {
  if (!text || !*text)
    return;
  printf("  %s: ", label);
  const char *nl = strchr(text, '\n');
  size_t n = nl ? (size_t)(nl - text) : strlen(text);
  if (n > 160)
    n = 160;
  printf("%.*s\n", (int)n, text);
}

// judge one case by its own headers; returns 1 pass, 0 fail; on fail
// fills a short reason. `only` selects the block for TK_BLOCK cases.
static bool judge_case(const TestCase *tc, const Headers *h, char **reason,
                       char **detail) {
  if (tc->kind == TK_BLOCK) {
    char *diags = NULL;
    Program *p = compile_case(tc->path, h, true, &diags);
    if (!p) {
      *reason = xstrdup("compile error");
      *detail = diags;
      return false;
    }
    extern const char *g_emit_test_name;
    g_emit_test_name = tc->block;
    char *wat = NULL;
    size_t wat_len = 0;
    emit_program(p, false, &wat, &wat_len);
    g_emit_test_name = NULL;
    char *out_s = NULL, *err_s = NULL;
    size_t out_n = 0, err_n = 0;
    bool timed_out = false;
    int rc = run_program(wat, wat_len, &out_s, &out_n, &err_s, &err_n,
                         &timed_out);
    if (timed_out) {
      *reason = xstrdup("timed out under the cap");
      return false;
    }
    if (rc != 0) {
      *reason = xstrdup("the test panicked");
      *detail = err_s;
      return false;
    }
    return true;
  }

  // file / case-dir forms: `// expect:` cases are diagnostic pins —
  // check must fail naming every substring; everything else runs.
  if (VLEN(h->expect) > 0) {
    char *diags = NULL;
    Program *p = compile_case(tc->path, h, false, &diags);
    if (p) {
      *reason = xstrdup("expected a compile diagnostic, check passed");
      return false;
    }
    if (!all_present(&h->expect, diags)) {
      *reason = xstrdup("diagnostic missing a named substring");
      *detail = diags;
      return false;
    }
    return true;
  }

  char *diags = NULL;
  Program *p = compile_case(tc->path, h, false, &diags);
  if (!p) {
    *reason = xstrdup("compile error");
    *detail = diags;
    return false;
  }
  char *wat = NULL;
  size_t wat_len = 0;
  emit_program(p, false, &wat, &wat_len);
  char *out_s = NULL, *err_s = NULL;
  size_t out_n = 0, err_n = 0;
  bool timed_out = false;
  int rc = run_program(wat, wat_len, &out_s, &out_n, &err_s, &err_n,
                       &timed_out);
  if (timed_out) {
    *reason = xstrdup("timed out under the cap");
    return false;
  }
  if (VLEN(h->err) > 0) {
    // a panic-catalog pin: nonzero exit (or the pinned code), stderr
    // naming every substring
    if (rc == 0) {
      *reason = xstrdup("expected a failure, the program exited 0");
      return false;
    }
    if (h->has_exit && rc != h->exit_code) {
      *reason = xstrdup("exit code mismatch");
      return false;
    }
    if (!all_present(&h->err, err_s)) {
      *reason = xstrdup("stderr missing a named substring");
      *detail = err_s;
      return false;
    }
    return true;
  }
  long want_rc = h->has_exit ? h->exit_code : 0;
  if (rc != want_rc) {
    char buf[128];
    snprintf(buf, sizeof buf, "exit %d, want %ld", rc, want_rc);
    *reason = xstrdup(buf);
    *detail = err_s;
    return false;
  }
  // expected stdout: each `// out:` line pins one full output line
  size_t want_n = 0;
  for (size_t i = 0; i < VLEN(h->out); i++)
    want_n += strlen(*VAT(h->out, const char *, i)) + 1;
  char *want = malloc(want_n + 1);
  size_t at = 0;
  for (size_t i = 0; i < VLEN(h->out); i++) {
    size_t l = strlen(*VAT(h->out, const char *, i));
    memcpy(want + at, *VAT(h->out, const char *, i), l);
    at += l;
    want[at++] = '\n';
  }
  want[at] = 0;
  bool ok = out_n == want_n && memcmp(out_s, want, want_n) == 0;
  free(want);
  if (!ok) {
    *reason = xstrdup("stdout mismatch");
    *detail = out_s;
  }
  return ok;
}

// ----------------------------------------------------------- collection

// scan one .rho file's decls for test blocks; a file that fails to
// parse becomes a TK_FORCE case so the breakage surfaces as a FAIL.
static bool collect_blocks(const char *path, const char *stem) {
  diags_reset();
  Module *m = module_load(g_arena, path);
  if (g_had_error || !m) {
    TestCase *tc = VPUSH(g_tests, TestCase);
    tc->name = aprintf(g_arena, "%s", stem);
    tc->path = intern_c(path);
    tc->block = NULL;
    tc->kind = TK_FORCE;
    return false;
  }
  for (size_t i = 0; i < reflist_len(m->decls); i++) {
    Node *d = node_get(reflist_at(m->decls, i));
    if (d->kind != NT_TEST)
      continue;
    TestCase *tc = VPUSH(g_tests, TestCase);
    tc->name = aprintf(g_arena, "%s:%s", stem, d->name);
    tc->path = intern_c(path);
    tc->block = aprintf(g_arena, "test:%s", d->name);
    tc->kind = TK_BLOCK;
  }
  return true;
}

static bool stem_ends_test(const char *name) {
  size_t n = strlen(name);
  return n > 9 && strcmp(name + n - 9, "_test.rho") == 0;
}
static bool is_rho(const char *name) {
  size_t n = strlen(name);
  return n > 4 && strcmp(name + n - 4, ".rho") == 0;
}

static bool collect_blocks(const char *path, const char *stem);

static void collect_file(const char *path, const char *relname,
                         const char *relstem) {
  // one parse decides the shape: a file that fails to parse becomes
  // exactly ONE force case (a must-check-clean failure naming the
  // parse diagnostics) — never a file case and a force case under
  // the same name
  if (!collect_blocks(path, relstem))
    return;
  if (stem_ends_test(relname)) {
    TestCase *tc = VPUSH(g_tests, TestCase);
    tc->name = aprintf(g_arena, "%s", relstem);
    tc->path = intern_c(path);
    tc->block = NULL;
    tc->kind = TK_FILE;
  }
}

static void collect_dir(const char *dir, const char *prefix);

static void collect_one_entry(const char *dir, const char *name,
                              const char *prefix) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  char rel[512];
  rel[0] = 0;
  if (prefix[0])
    snprintf(rel, sizeof rel, "%s/%s", prefix, name);
  else
    snprintf(rel, sizeof rel, "%s", name);
  struct stat st;
  if (stat(path, &st) != 0)
    return;
  if (S_ISDIR(st.st_mode)) {
    char mainp[1100];
    snprintf(mainp, sizeof mainp, "%s/main.rho", path);
    if (access(mainp, R_OK) == 0) {
      // a case directory: one multi-file test, main.rho carries the
      // headers
      TestCase *tc = VPUSH(g_tests, TestCase);
      tc->name = aprintf(g_arena, "%s", rel);
      tc->path = intern_c(mainp);
      tc->block = NULL;
      tc->kind = TK_FILE;
      return;
    }
    collect_dir(path, rel);
    return;
  }
  if (!S_ISREG(st.st_mode) || !is_rho(name))
    return;
  char stem[512];
  snprintf(stem, sizeof stem, "%s", rel);
  size_t ln = strlen(stem);
  if (stem_ends_test(name))
    stem[ln - 9] = 0; // strip "_test.rho"
  else
    stem[ln - 4] = 0; // strip ".rho"
  collect_file(path, name, stem);
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(const char **)a, *(const char **)b);
}

static void collect_dir(const char *dir, const char *prefix) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  VEC(const char *, names);
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.')
      continue;
    *VPUSH(names, const char *) = aprintf(g_arena, "%s", de->d_name);
  }
  closedir(d);
  qsort(names.data, VLEN(names), names.elem, cmp_str);
  for (size_t i = 0; i < VLEN(names); i++)
    collect_one_entry(dir, *VAT(names, const char *, i), prefix);
}

// ----------------------------------------------------------------- verb

static int cmp_case(const void *a, const void *b) {
  return strcmp(((const TestCase *)a)->name, ((const TestCase *)b)->name);
}

int cmd_test(int argc, char **argv) {
  if (argc < 1) {
    fprintf(stderr, "rho test <path>... [filter]\n");
    return EXIT_USAGE;
  }
  const char *filter = NULL;
  vec_init(&g_tests, sizeof(TestCase));
  for (int i = 0; i < argc; i++) {
    struct stat st;
    if (stat(argv[i], &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        collect_dir(argv[i], "");
      } else {
        // a direct file argument: collect under its own basename
        const char *base = strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];
        char stem[512];
        snprintf(stem, sizeof stem, "%s", base);
        size_t ln = strlen(stem);
        if (stem_ends_test(stem))
          stem[ln - 9] = 0;
        else
          stem[ln - 4] = 0;
        collect_file(argv[i], base, stem);
      }
      continue;
    }
    if (filter) {
      fprintf(stderr, "rho test: more than one filter given\n");
      return EXIT_USAGE;
    }
    filter = argv[i];
  }
  if (VLEN(g_tests))
    if (VLEN(g_tests))
    qsort(g_tests.data, VLEN(g_tests), g_tests.elem, cmp_case);

  // duplicate names would make a run unjudgeable — refuse loudly
  for (size_t i = 1; i < VLEN(g_tests); i++) {
    if (strcmp(VAT(g_tests, TestCase, i - 1)->name,
               VAT(g_tests, TestCase, i)->name) == 0) {
      fprintf(stderr, "rho test: duplicate test name '%s'\n",
              VAT(g_tests, TestCase, i)->name);
      return 1;
    }
  }

  size_t selected = 0;
  for (size_t i = 0; i < VLEN(g_tests); i++)
    if (!filter ||
        strstr(VAT(g_tests, TestCase, i)->name, filter) != NULL)
      selected++;
  if (selected == 0) {
    printf("test: no tests matched\n");
    return 1; // a fake green is a red
  }

  size_t pass = 0, fail = 0, pending = 0;
  for (size_t i = 0; i < VLEN(g_tests); i++) {
    const TestCase *tc = VAT(g_tests, TestCase, i);
    if (filter && strstr(tc->name, filter) == NULL)
      continue;
    Headers h;
    memset(&h, 0, sizeof h);
    vec_init(&h.out, sizeof(const char *));
    vec_init(&h.sets, sizeof(const char *));
    vec_init(&h.expect, sizeof(const char *));
    vec_init(&h.err, sizeof(const char *));
    parse_headers(tc->path, &h);
    char *reason = NULL, *detail = NULL;
    bool ok = tc->kind == TK_FORCE ? false
                                   : judge_case(tc, &h, &reason, &detail);
    if (h.pending) {
      // the expected-fail ledger: a pending case must fail by its own
      // criteria; passing prints PROMOTE and counts as a failure until
      // the marker comes off (no stale ledger under a green gate)
      if (ok) {
        printf("PROMOTE %s — passed; remove its // pending: marker\n",
               tc->name);
        fail++;
      } else {
        printf("pending %s\n", tc->name);
        pending++;
      }
      continue;
    }
    if (ok) {
      printf("ok      %s\n", tc->name);
      pass++;
    } else {
      printf("FAIL    %s: %s\n", tc->name, reason ? reason : "?");
      if (detail)
        print_head("note", detail);
      fail++;
    }
  }
  printf("test: %zu pass, %zu fail, %zu pending (of %zu)\n", pass, fail,
         pending, selected);
  return fail ? 1 : 0;
}
