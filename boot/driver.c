// driver.c — command implementations. T1.1 delivers the skeleton;
// each command fills in as its phase lands (see TODO.md Phase 1).
#include "rho.h"
#include <unistd.h>
#include "sem.h"

int emit_program(Program *p, bool debug, char **wat_out, size_t *wat_len);

// the test verb sets this while compiling a test-block file: those
// per-test programs have no main. Every other mode keeps the law.
bool g_test_main_optional = false;

// shared front half for build/run/check
static Program *load_and_check(int argc, char **argv, const char **path_out) {
  if (argc < 1)
    return NULL;
  const char *path = argv[0];
  Program *p = program_new();
  for (int i = 1; i < argc; i++) {
    const char *nv = NULL;
    if (strncmp(argv[i], "--set ", 6) == 0)
      nv = argv[i] + 6; // one-token form: "--set name=value"
    else if (strcmp(argv[i], "--set") == 0 && i + 1 < argc &&
             strchr(argv[i + 1], '='))
      nv = argv[++i]; // two-token form: --set name=value
    if (nv) {
      char *eq = strchr(nv, '=');
      if (!eq)
        continue;
      SetOverride *so = vec_push(&p->sets);
      so->name = intern(nv, (size_t)(eq - nv));
      so->value = eq + 1;
    }
  }
  if (!program_load_graph(p, path)) {
    diags_print(stderr);
    return NULL;
  }
  bool ok = check_program(p);
  extern bool g_set_refused;
  if (g_set_refused)
    return NULL;
  if (!ok) {
    diags_print(stderr);
    return NULL;
  }
  if (path_out)
    *path_out = path;
  return p;
}

int cmd_build(int argc, char **argv) {
  const char *path = NULL;
  Program *p = load_and_check(argc, argv, &path);
  if (!p)
    return g_had_error ? EXIT_COMPILE : EXIT_USAGE;
  bool debug = false;
  const char *out = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-g") == 0)
      debug = true;
    else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
      out = argv[++i];
  }
  if (!out)
    out = "a.wasm";
  char *wat = NULL;
  size_t wat_len = 0;
  emit_program(p, debug, &wat, &wat_len);
  char watpath[512], wasmpath[512], tmppath[520];
  snprintf(watpath, sizeof watpath, "%s.wat", out);
  snprintf(wasmpath, sizeof wasmpath, "%s", out);
  FILE *wf = fopen(watpath, "w");
  fwrite(wat, 1, wat_len, wf);
  fclose(wf);
  // executable outputs always go through tmp + rename (standing law)
  snprintf(tmppath, sizeof tmppath, "%s.tmp", wasmpath);
  char cmd[1024];
  snprintf(cmd, sizeof cmd, "wat2wasm %s -o %s", watpath, tmppath);
  if (system(cmd) != 0) {
    fprintf(stderr, "rho: wat2wasm failed; WAT kept at %s\n", watpath);
    return 1;
  }
  if (rename(tmppath, wasmpath) != 0) {
    fprintf(stderr, "rho: rename %s -> %s failed\n", tmppath, wasmpath);
    return 1;
  }
  return 0;
}

int cmd_run(int argc, char **argv) {
  const char *path = NULL;
  Program *p = load_and_check(argc, argv, &path);
  if (!p)
    return g_had_error ? EXIT_COMPILE : EXIT_USAGE;
  char watz[] = "/tmp/rho-run.XXXXXX.wat";
  char wasmt[] = "/tmp/rho-run.XXXXXX.wasm";
  int fd = mkstemps(watz, 4);
  close(fd);
  mkstemps(wasmt, 5);
  char *wat = NULL;
  size_t wat_len = 0;
  emit_program(p, false, &wat, &wat_len);
  FILE *wf = fopen(watz, "w");
  fwrite(wat, 1, wat_len, wf);
  fclose(wf);
  char cmd[2048];
  // run through wasmtime; map traps to the panic catalog (stack
  // overflow = defined panic, exit 101 — spec §3)
  // wasmtime's trap backtrace goes to a file: a trap maps to the
  // panic catalog (stack overflow = defined panic, exit 101)
  snprintf(cmd, sizeof cmd,
           "wat2wasm %s -o %s 2>/dev/null && "
           "{ wasmtime run %s 2>/tmp/rho-trap.$$.err; rc=$?; "
           "  if grep -q -e 'wasm trap' -e 'stack overflow' "
           "/tmp/rho-trap.$$.err 2>/dev/null; then "
           "    msg=$(sed -n 's/.*wasm trap: //p' /tmp/rho-trap.$$.err "
           "| head -1 | sed 's/call stack exhausted/stack overflow/'); "
           "    [ -z \"$msg\" ] && msg='stack overflow'; "
           "    echo \"panic: $msg\" >&2; "
           "    rm -f /tmp/rho-trap.$$.err; exit 101; "
           "  fi; "
           "  cat /tmp/rho-trap.$$.err >&2 2>/dev/null; "
           "  rm -f /tmp/rho-trap.$$.err; exit $rc; }; "
           "exit 1",
           watz, wasmt, wasmt);
  int rc = system(cmd);
  unlink(watz);
  unlink(wasmt);
  if (WIFEXITED(rc))
    return WEXITSTATUS(rc);
  return 1;
}

// cmd_test lives in test.c — the §17 verb (T3.5).

// canonical formatter (T1.9): prints the checked AST back in canonical
// form; fmt(fmt(x)) == fmt(x) byte-for-byte
int cmd_fmt(int argc, char **argv) {
  if (argc < 1) {
    fprintf(stderr, "rho fmt <file.rho>\n");
    return EXIT_USAGE;
  }
  Program *p = program_new();
  if (!program_load_graph(p, argv[0])) {
    diags_print(stderr);
    return EXIT_COMPILE;
  }
  extern void fmt_program(FILE * out, Program * p);
  fmt_program(stdout, p);
  return 0;
}

int cmd_check(int argc, char **argv) {
  if (argc < 1) {
    fprintf(stderr, "rho check <file.rho> [--set name=value]...\n");
    return EXIT_USAGE;
  }
  const char *path = argv[0];
  Program *p = program_new();
  for (int i = 1; i < argc; i++) {
    const char *nv = NULL;
    if (strncmp(argv[i], "--set ", 6) == 0)
      nv = argv[i] + 6; // one-token form: "--set name=value"
    else if (strcmp(argv[i], "--set") == 0 && i + 1 < argc &&
             strchr(argv[i + 1], '='))
      nv = argv[++i]; // two-token form: --set name=value
    if (nv) {
      char *eq = strchr(nv, '=');
      if (!eq) {
        fprintf(stderr, "rho: --set needs name=value\n");
        return EXIT_USAGE;
      }
      SetOverride *so = vec_push(&p->sets);
      so->name = intern(nv, (size_t)(eq - nv));
      so->value = eq + 1;
    }
  }
  if (!program_load_graph(p, path)) {
    diags_print(stderr);
    return EXIT_COMPILE;
  }
  bool ok = check_program(p);
  extern bool g_set_refused;
  if (g_set_refused)
    return EXIT_USAGE; // exit 2: refusal
  if (!ok) {
    diags_print(stderr);
    return EXIT_COMPILE;
  }
  printf("ok\n");
  return 0;
}

int cmd_dump_ast(const char *path) {
  Module *m = module_load(g_arena, path);
  if (g_had_error) {
    diags_print(stderr);
    return EXIT_COMPILE;
  }
  dump_module(stdout, m);
  return 0;
}

