#include "ir.h"

void lower_dump_ir(void);
#if !defined(__wasm__)
#include <sys/wait.h>
#include <unistd.h>
#else
// wasi-libc has no <sys/wait.h>; and system() is declared in stdlib.h but
// never defined — the browser build only emits wasm binaries directly
#define WEXITSTATUS(status) (status)
int system(const char *cmd) {
  (void)cmd;
  return -1;
}
#endif

extern const char PRELUDE_SOURCE[];
extern const char *g_root_dir;
const char *g_root_dir = ".";

// ------------------------------------------------------------------ io ----

static Str read_file_or_die(Str path) {
  FILE *f = fopen(str_to_c(path), "rb");
  if (!f) {
    fprintf(stderr, "rho: cannot open %.*s\n", (int)path.n, path.p);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = arena_alloc((size_t)n + 1);
  if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fprintf(stderr, "rho: short read on %.*s\n", (int)path.n, path.p);
    exit(2);
  }
  buf[n] = 0;
  fclose(f);
  return str_from_len(buf, (size_t)n);
}

static bool write_file(Str path, Str data) {
  // exec image (and any file) must land on a FRESH vnode: rewriting an
  // ad-hoc-signed executable in place leaves the kernel's cached page-hash
  // validation stale, and the next exec wedges uninterruptibly in kernel
  // space (the 2026-09-21 poison: identical bytes, dead inode). tmp+rename
  // gives every build its own vnode. The wasm build has no rename (browser).
  char tmp[4096];
#if !defined(__wasm__)
  snprintf(tmp, sizeof(tmp), "%.*s.tmp", (int)path.n, path.p);
#else
  snprintf(tmp, sizeof(tmp), "%.*s", (int)path.n, path.p);
#endif
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return false;
  fwrite(data.p, 1, data.n, f);
  fclose(f);
#if !defined(__wasm__)
  if (rename(tmp, str_to_c(path)) != 0) {
    remove(tmp);
    return false;
  }
#endif
  return true;
}

// ---------------------------------------------------------------- check ---=

// Compile (parse + check) a root file. Returns the module root or NULL after
// printing diagnostics.
static Decl *compile_root(Str path) {
  check_reset(); // fresh module registry per compilation unit
  clear_diags();
  prelude_init();
  Str src = read_file_or_die(path);
  Decl *root = parse_file(path, src);
  if (diag_count()) {
    flush_diags();
    return NULL;
  }
  check_module(root);
  if (diag_count()) {
    flush_diags();
    return NULL;
  }
  return root;
}

// --------------------------------------------------------------- selftest --

static bool str_read_file(Str path, Str *out) {
  FILE *f = fopen(str_to_c(path), "rb");
  if (!f)
    return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = arena_alloc((size_t)n + 1);
  if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    return false;
  }
  buf[n] = 0;
  fclose(f);
  *out = str_from_len(buf, (size_t)n);
  return true;
}

static Vec list_dir(Str dir, const char *ext) {
  Vec files = {0};
#if defined(__wasm__)
  (void)dir; (void)ext; // the browser build never lists directories
  return files;
#else
  SB cmd = {0};
  sb_printf(&cmd, "find %.*s -name '*%s' | sort", (int)dir.n, dir.p, ext);
  FILE *f = popen(str_to_c(sb_finish(&cmd)), "r");
  if (!f)
    return files;
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
    if (n)
      vec_push(&files, arena_strdup(line));
  }
  pclose(f);
  return files;
#endif
}

static int selftest_failures;

static void fail(const char *name, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "FAIL %s: ", name);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  selftest_failures++;
}

static void golden_check(Str got, Str golden_path, const char *name, bool update) {
  if (update) {
    if (!write_file(golden_path, got))
      fail(name, "cannot write golden %s", str_to_c(golden_path));
    return;
  }
  Str want;
  if (!str_read_file(golden_path, &want)) {
    fail(name, "missing golden %s", str_to_c(golden_path));
    return;
  }
  if (!str_eq(got, want)) {
    Str tmp = str_from(arena_printf("build/%s.got", name));
    write_file(tmp, got);
    fail(name, "output differs from %s (wrote %.*s)", str_to_c(golden_path), (int)tmp.n, tmp.p);
  }
}

static int run_selftest(bool update) {

  // 1. AST goldens: tests/frontend/*.rho -> tests/golden/ast/<name>.dump
  Vec fe = list_dir(str_from("tests/frontend"), ".rho");
  for (size_t i = 0; i < fe.n; i++) {
    const char *path = fe.items[i];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char name[256];
    snprintf(name, sizeof(name), "%s", base);
    char *dot = strrchr(name, '.');
    if (dot)
      *dot = 0;

    check_reset();
    clear_diags();
    prelude_init();
    Str src = read_file_or_die(str_from(path));
    Decl *root = parse_file(str_from(path), src);
    golden_check(dump_module(root),
                 str_from(arena_printf("tests/golden/ast/%s.dump", name)), name, update);
    if (diag_count()) {
      SB dsb = {0};
      render_diags(&dsb);
      golden_check(sb_finish(&dsb),
                   str_from(arena_printf("tests/golden/diag/%s_parse.err", name)),
                   arena_printf("%s parse", name), update);
    }
    check_module(root);
    if (diag_count()) {
      SB dsb = {0};
      render_diags(&dsb);
      golden_check(sb_finish(&dsb),
                   str_from(arena_printf("tests/golden/diag/%s_check.err", name)),
                   arena_printf("%s check", name), update);
    }
  }

  // 2. diagnostic goldens: tests/diag/*.rho -> tests/golden/diag/<name>.err
  Vec dg = list_dir(str_from("tests/diag"), ".rho");
  for (size_t i = 0; i < dg.n; i++) {
    const char *path = dg.items[i];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char name[256];
    snprintf(name, sizeof(name), "%s", base);
    char *dot = strrchr(name, '.');
    if (dot)
      *dot = 0;

    check_reset();
    clear_diags();
    prelude_init();
    Str src = read_file_or_die(str_from(path));
    Decl *root = parse_file(str_from(path), src);
    check_module(root);
    SB dsb = {0};
    render_diags(&dsb);
    golden_check(sb_finish(&dsb), str_from(arena_printf("tests/golden/diag/%s.err", name)),
                 name, update);
  }

  if (selftest_failures == 0)
    printf("selftest ok\n");
  else
    printf("selftest: %d failure(s)\n", selftest_failures);
  return selftest_failures ? 1 : 0;
}

static void usage(void);

// ------------------------------------------------------------- codegen ----=

// boot ships one backend; the self-hosted compiler (self/rho.rho) owns the
// mac/linux, arm64(aarch64)/amd64 targets.
static Target parse_target(const char *s) {
  if (!strcmp(s, "wasm32-wasi"))
    return TGT_WASM32_WASI;
  fprintf(stderr,
          "rho: unsupported target: %s (boot ships wasm only; native "
          "backends live in the self-hosted compiler)\n", s);
  exit(2);
}

// full pipeline: check -> lower -> emit. wasm32-wasi produces a runnable
// module directly. Returns the path to the built artifact (arena). On any
// failure prints diagnostics, returns NULL. `announce` prints the artifact
// line (build); run and test build silently.
static const char *build_to(const char *file, Target target, const char *out_path,
                            bool announce) {
  Decl *root = compile_root(str_from(file));
  if (!root)
    return NULL;
  Module *root_mod = NULL;
  for (size_t i = 0; i < g_module_order.n; i++) {
    Module *m = g_module_order.items[i];
    if (str_eq_c(m->path, file))
      root_mod = m;
  }
  lower_set_root(root_mod);
  lower_program();
  if (getenv("RHO_DUMP_IR"))
    lower_dump_ir();
  SB emitted = {0};
  // wasm: the emitter produces the binary module directly
  emit_wasm(target, &emitted);
  if (!write_file(str_from(out_path), sb_finish(&emitted))) {
    fprintf(stderr, "rho: cannot write %s\n", out_path);
    return NULL;
  }
  if (announce)
    printf("built %s\n", out_path);
  return out_path;
}

static int cmd_build_run_test(const char *cmd, int argc, char **argv) {
  if (!strcmp(cmd, "test")) {
    const char *dir = "corpus";
    Target tt = TGT_WASM32_WASI;
    for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--target") && i + 1 < argc)
        tt = parse_target(argv[++i]);
      else
        dir = argv[i];
    }
    Vec files = list_dir(str_from(dir), ".rho");
    int failures = 0, ran = 0;
    for (size_t i = 0; i < files.n; i++) {
      const char *path = files.items[i];
      const char *base = strrchr(path, '/');
      base = base ? base + 1 : path;
      char name[256];
      snprintf(name, sizeof(name), "%s", base);
      char *dot = strrchr(name, '.');
      if (dot)
        *dot = 0;
      const char *bin = arena_printf("build/corpus_%s", name);
      const char *built = build_to(path, tt, bin, false);
      if (!built) {
        printf("FAIL %s (build)\n", name);
        failures++;
        continue;
      }
      SB run_cmd = {0};
      sb_printf(&run_cmd, "%s > /tmp/rho_out_XXXX 2>/dev/null < /dev/null", built);
      // use a fixed temp name for determinism
      const char *tmpout = arena_printf("/tmp/rho_out_%s", name);
      SB rc = {0};
      int code;
      // wasmtime prints stdout to fd 1; panic exit 101 propagates
      sb_printf(&rc, "wasmtime run %s > %s 2>/dev/null", built, tmpout);
      int status = system(str_to_c(sb_finish(&rc)));
      code = WEXITSTATUS(status);
      // expected exit
      int want_code = 0;
      Str src = read_file_or_die(str_from(path));
      SB first = {0};
      for (size_t k = 0; k < src.n && src.p[k] != '\n'; k++)
        sb_push(&first, src.p[k]);
      const char *line = first.buf ? first.buf : "";
      const char *ex = strstr(line, "exit:");
      if (ex)
        want_code = (int)strtol(ex + 5, NULL, 10);
      // expected stdout
      Str want_out = {0};
      const char *out_path = arena_printf("%.*s/%s.out", (int)(strlen(dir)), dir, name);
      str_read_file(str_from(out_path), &want_out);
      Str got_out = {0};
      str_read_file(str_from(tmpout), &got_out);
      ran++;
      bool ok = code == want_code && str_eq(got_out, want_out);
      if (!ok) {
        printf("FAIL %s (exit %d, want %d)%s\n", name, code, want_code,
               got_out.n != want_out.n ? " stdout differs" : "");
        failures++;
      }
      (void)run_cmd;
    }
    printf("%s: %d ran, %d failed\n", failures ? "corpus FAIL" : "corpus ok", ran, failures);
    return failures ? 1 : 0;
  }

  // build / run
  const char *file = NULL, *out = NULL, *target_s = "wasm32-wasi";
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc)
      out = argv[++i];
    else if (!strcmp(argv[i], "--target") && i + 1 < argc)
      target_s = argv[++i];
    else
      file = argv[i];
  }
  if (!file) {
    usage();
    return 2;
  }
  Target target = parse_target(target_s);
  const char *bin;
  if (!strcmp(cmd, "build")) {
    bin = out ? out : "a.wasm";
  } else {
#if defined(__wasm__)
    bin = "/tmp/rho_run";
#else
    bin = arena_printf("/tmp/rho_run_%d", (int)getpid());
#endif
  }
  const char *built = build_to(file, target, bin, !strcmp(cmd, "build"));
  if (!built)
    return 1;
  if (!strcmp(cmd, "run")) {
    SB rc = {0};
    sb_printf(&rc, "wasmtime run %s", built);
    for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--")) {
        for (int j = i + 1; j < argc; j++)
          sb_printf(&rc, " \"%s\"", argv[j]);
        break;
      }
    }
    int status = system(str_to_c(sb_finish(&rc)));
    return WEXITSTATUS(status);
  }
  // build_to already announced the artifact ("built …" / "wrote …")
  return 0;
}

// ------------------------------------------------------------------ main ---

static void usage(void) {
  fprintf(stderr,
          "rho 0.4.0\n"
          "usage: rho <command> [args]\n"
          "  check  <file>              parse + typecheck\n"
          "  build  <file> [-o out]     emit a runnable wasm32-wasi module\n"
          "  run    <file> [-- args]    build + execute (wasmtime)\n"
          "  test   [dir] [--target t]  compile + run corpus programs\n"
          "  selftest [--update-goldens]\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const char *cmd = argv[1];

  if (!strcmp(cmd, "--version") || !strcmp(cmd, "version")) {
    printf("rho 0.4.0\n");
    return 0;
  }

  if (!strcmp(cmd, "selftest"))
    return run_selftest(argc > 2 && !strcmp(argv[2], "--update-goldens"));

  if (!strcmp(cmd, "check")) {
    if (argc < 3) {
      usage();
      return 2;
    }
    Decl *root = compile_root(str_from(argv[2]));
    if (!root)
      return 1;
    printf("ok %s\n", argv[2]);
    return 0;
  }

  if (!strcmp(cmd, "build") || !strcmp(cmd, "run") || !strcmp(cmd, "test"))
    return cmd_build_run_test(cmd, argc, argv);

  usage();
  return 2;
}
