#include "rho.h"

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
  FILE *f = fopen(str_to_c(path), "wb");
  if (!f)
    return false;
  fwrite(data.p, 1, data.n, f);
  fclose(f);
  return true;
}

// ---------------------------------------------------------------- check ---=

// Compile (parse + check) a root file. Returns the module root or NULL after
// printing diagnostics.
static Decl *compile_root(Str path) {
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

static int run_selftest(bool update, bool fmt_only) {

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

    if (!fmt_only) {
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

    // 2. fmt roundtrip + idempotence (parse errors naturally skip via check)
    check_reset();
    clear_diags();
    prelude_init();
    Str src2 = read_file_or_die(str_from(path));
    Decl *root2 = parse_file(str_from(path), src2);
    if (!diag_count()) {
      Str once = fmt_module(root2);
      Decl *reparsed = parse_file(str_from(path), once);
      if (diag_count()) {
        SB dsb = {0};
        render_diags(&dsb);
        fail(arena_printf("%s fmt", name), "fmt output does not reparse:\n%.*s", (int)dsb.n,
             sb_finish(&dsb).p);
      } else {
        Str twice = fmt_module(reparsed);
        if (!str_eq(once, twice))
          fail(arena_printf("%s fmt", name), "fmt is not idempotent");
        // strongest pin: reformatting must preserve the AST exactly
        if (!str_eq(dump_module(root2), dump_module(reparsed)))
          fail(arena_printf("%s fmt", name), "fmt changed the AST");
        // strongest pin: reformatting must preserve the AST exactly
        if (!str_eq(dump_module(root2), dump_module(reparsed)))
          fail(arena_printf("%s fmt", name), "fmt changed the AST");
        // and the reformatted source must still typecheck the same
        check_reset();
        prelude_init();
        check_module(reparsed);
        if (diag_count()) {
          SB dsb = {0};
          render_diags(&dsb);
          fail(arena_printf("%s fmt", name), "fmt output fails check:\n%.*s", (int)dsb.n, sb_finish(&dsb).p);
        }
      }
    }
  }

  // 3. diagnostic goldens: tests/diag/*.rho -> tests/golden/diag/<name>.err
  if (!fmt_only) {
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
  }

  if (selftest_failures == 0)
    printf("selftest ok\n");
  else
    printf("selftest: %d failure(s)\n", selftest_failures);
  return selftest_failures ? 1 : 0;
}

// ------------------------------------------------------------------ main ---

static void usage(void) {
  fprintf(stderr,
          "rho 0.0.1\n"
          "usage: rho <command> [args]\n"
          "  check  <file>              parse + typecheck\n"
          "  fmt    [-w] <file>         print canonical formatting\n"
          "  build  <file> [-o out]     (arrives in 0.0.2)\n"
          "  run    <file>              (arrives in 0.0.2)\n"
          "  test   [file|dir]          (arrives in 0.0.2)\n"
          "  selftest [--update-goldens] [--fmt]\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const char *cmd = argv[1];

  if (!strcmp(cmd, "--version") || !strcmp(cmd, "version")) {
    printf("rho 0.0.1\n");
    return 0;
  }

  if (!strcmp(cmd, "selftest"))
    return run_selftest(argc > 2 && !strcmp(argv[2], "--update-goldens"),
                        argc > 2 && !strcmp(argv[2], "--fmt"));

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

  if (!strcmp(cmd, "fmt")) {
    bool inplace = false;
    const char *file = NULL;
    for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "-w"))
        inplace = true;
      else
        file = argv[i];
    }
    if (!file) {
      usage();
      return 2;
    }
    check_reset();
    clear_diags();
    prelude_init();
    Str src = read_file_or_die(str_from(file));
    Decl *root = parse_file(str_from(file), src);
    if (diag_count()) {
      flush_diags();
      return 1;
    }
    Str out = fmt_module(root);
    if (inplace) {
      if (!write_file(str_from(file), out)) {
        fprintf(stderr, "rho: cannot write %s\n", file);
        return 2;
      }
    } else {
      fwrite(out.p, 1, out.n, stdout);
    }
    return 0;
  }

  if (!strcmp(cmd, "build") || !strcmp(cmd, "run") || !strcmp(cmd, "test")) {
    fprintf(stderr, "rho: `%s` arrives in 0.0.2 (code generation)\n", cmd);
    return 2;
  }

  usage();
  return 2;
}
