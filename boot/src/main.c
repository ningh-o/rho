#include "ir.h"

void lower_dump_ir(void);
void lower_dump_ir_if_requested(void);
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

// the freestanding arm64-mac runtime: _rho_rt_start initializes the bump
// allocator's heap pointer, calls _main (the emitter's trampoline), and
// exits with its return code; _write/_exit are the raw syscalls the mac
// prelude calls. Assembled into every arm64-mac image — no libc.
static const char RT_ARM64[] =
    ".section __TEXT,__text,regular,pure_instructions\n"
    ".globl _rho_rt_start\n"
    "_rho_rt_start:\n"
    "  adrp x8, _rho_rt_heap@PAGE\n"
    "  add x8, x8, _rho_rt_heap@PAGEOFF\n"
    "  adrp x9, _rho__prelude___HEAP@PAGE\n"
    "  add x9, x9, _rho__prelude___HEAP@PAGEOFF\n"
    "  str x8, [x9]\n"
    "  bl _main\n"
    "  mov x16, #1\n"
    "  movk x16, #32, lsl 16\n"
    "  svc #0x80\n"
    ".globl _write\n"
    "_write:\n"
    "  mov x16, #4\n"
    "  movk x16, #32, lsl 16\n"
    "  svc #0x80\n"
    "  ret\n"
    ".globl _exit\n"
    "_exit:\n"
    "  mov x16, #1\n"
    "  movk x16, #32, lsl 16\n"
    "  svc #0x80\n";

// the freestanding amd64-linux runtime: same contract, SysV + raw
// syscalls (write=1, exit=60). Symbols ride the ELF dialect — bare names.
static const char RT_AMD64_LINUX[] =
    ".text\n"
    "rho_rt_start:\n"
    "  leaq _rho_rt_heap(%rip), %rax\n"
    "  leaq rho__prelude___HEAP(%rip), %rcx\n"
    "  movq %rax, (%rcx)\n"
    "  call main\n"
    "  movl %eax, %edi\n"
    "  movl $60, %eax\n"
    "  syscall\n"
    "write:\n"
    "  movl $1, %eax\n"
    "  syscall\n"
    "  ret\n"
    "exit:\n"
    "  movl $60, %eax\n"
    "  syscall\n";

// the freestanding arm64-linux runtime: the syscall number rides x8 —
// arm64 linux uses the asm-generic table (write=64, exit=93)
static const char RT_ARM64_LINUX[] =
    ".text\n"
    "rho_rt_start:\n"
    "  adrp x8, _rho_rt_heap@PAGE\n"
    "  add x8, x8, _rho_rt_heap@PAGEOFF\n"
    "  adrp x9, rho__prelude___HEAP@PAGE\n"
    "  add x9, x9, rho__prelude___HEAP@PAGEOFF\n"
    "  str x8, [x9]\n"
    "  bl main\n"
    "  mov x8, #93\n"
    "  svc #0\n"
    ".globl write\n"
    "write:\n"
    "  mov x8, #64\n"
    "  svc #0\n"
    "  ret\n"
    ".globl exit\n"
    "exit:\n"
    "  mov x8, #93\n"
    "  svc #0\n";

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

static void usage(void);

// ------------------------------------------------------------- codegen ----=

static Target parse_target(const char *s) {
  if (!strcmp(s, "amd64-linux"))
    return TGT_AMD64_LINUX;
  if (!strcmp(s, "amd64-mac"))
    return TGT_AMD64_MAC;
  if (!strcmp(s, "arm64-mac"))
    return TGT_ARM64_MAC;
  if (!strcmp(s, "arm64-linux"))
    return TGT_ARM64_LINUX;
  if (!strcmp(s, "wasm32-wasi"))
    return TGT_WASM32_WASI;
  fprintf(stderr, "rho: unknown target `%s`\n", s);
  exit(2);
}

// full pipeline: check -> lower -> emit. wasm32-wasi produces a runnable
// module directly; native targets emit assembly text — assembling and
// linking are the platform toolchain's job, never an invocation from here.
// Returns the path to the built artifact (arena). On any failure prints
// diagnostics, returns NULL. `announce` prints the artifact line (build);
// run and test build silently.
static const char *build_to(const char *file, Target target, const char *out_path,
                            bool announce) {
  extern bool g_prelude_wasm;
  extern bool g_prelude_native;
  g_prelude_wasm = target == TGT_WASM32_WASI;
  g_prelude_native = target != TGT_WASM32_WASI;
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
  if (target == TGT_AMD64_LINUX || target == TGT_AMD64_MAC) {
    lower_dump_ir_if_requested();
    emit_amd64(target, &emitted);
    if (target == TGT_AMD64_LINUX) {
      // rho's own assembly -> own assembler -> static ELF; no libc, no
      // interpreter, no external toolchain anywhere
      Str text = sb_finish(&emitted);
      if (getenv("RHO_KEEP_ASM")) {
        char *p = arena_printf("%s.s", out_path);
        write_file(str_from(p), text);
      }
      SB full = {0};
      sb_append_c(&full, RT_AMD64_LINUX);
      sb_printf(&full, "%.*s", (int)text.n, text.p);
      SB secs[3];
      uint64_t str_va, data_va, heap_va;
      if (asm86_assemble(full.buf, 0x400000ull + ELF64_HDR, 0x1000, 0, secs,
                         &str_va, &data_va, &heap_va))
        return NULL;
      if (elf64_write(secs, 62 /*EM_X86_64*/, 0x400000ull + ELF64_HDR, data_va,
                      heap_va, out_path))
        return NULL;
      if (announce)
        printf("built %s\n", out_path);
      return out_path;
    }
  } else if (target == TGT_ARM64_LINUX) {
    lower_dump_ir_if_requested();
    emit_arm64(target, &emitted);
    Str text = sb_finish(&emitted);
    if (getenv("RHO_KEEP_ASM")) {
      char *p = arena_printf("%s.s", out_path);
      write_file(str_from(p), text);
    }
    SB full = {0};
    sb_append_c(&full, RT_ARM64_LINUX);
    sb_printf(&full, "%.*s", (int)text.n, text.p);
    SB secs[3];
    uint64_t str_va, data_va, heap_va;
    if (asm64_assemble(full.buf, 0x400000ull + ELF64_HDR, 0x1000, 0, secs,
                       &str_va, &data_va, &heap_va))
      return NULL;
    if (elf64_write(secs, 183 /*EM_AARCH64*/, 0x400000ull + ELF64_HDR, data_va,
                    heap_va, out_path))
      return NULL;
    if (announce)
      printf("built %s\n", out_path);
    return out_path;
  } else if (target == TGT_ARM64_MAC) {
    // rho's own assembly format -> own assembler -> static Mach-O with an
    // ad-hoc signature; no external toolchain anywhere
    lower_dump_ir_if_requested();
    emit_arm64(target, &emitted);
    Str text = sb_finish(&emitted);
    if (getenv("RHO_KEEP_ASM")) { // debug: keep the assembly text
      char *p = arena_printf("%s.s", out_path);
      write_file(str_from(p), text);
    }
    SB full = {0};
    sb_append_c(&full, RT_ARM64);
    sb_printf(&full, "%.*s", (int)text.n, text.p);
    // image base: VM 0x100000000 + the fixed 648-byte load-command block;
    // 16K pages — Apple Silicon maps segments on 16K boundaries
    SB secs[3];
    uint64_t str_va, data_va, heap_va;
    if (asm64_assemble(full.buf, 0x100000000ull + ARM64_MAC_HDR, 0x4000,
                       0x100000000ull + 0x1000000ull, secs, &str_va, &data_va,
                       &heap_va))
      return NULL;
    if (macho64_write(secs, 0x100000000ull + ARM64_MAC_HDR, data_va, heap_va,
                      out_path))
      return NULL;
    if (announce)
      printf("built %s\n", out_path);
    return out_path;
  } else if (target == TGT_WASM32_WASI) {
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
  // shared native tail (amd64 + arm64): write the assembly text and stop.
  // The toolchain does not shell out to an assembler/linker.
  const char *s_path = arena_printf("%s.s", out_path);
  if (!write_file(str_from(s_path), sb_finish(&emitted))) {
    fprintf(stderr, "rho: cannot write %s\n", s_path);
    return NULL;
  }
  if (announce)
    printf("wrote %s (assemble+link with the platform toolchain, e.g. cc %s -o %s)\n",
           s_path, s_path, out_path);
  return s_path;
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
#if defined(__linux__)
    if (tt != TGT_WASM32_WASI && tt != TGT_ARM64_MAC && tt != TGT_AMD64_LINUX &&
        tt != TGT_ARM64_LINUX) {
#else
    if (tt != TGT_WASM32_WASI && tt != TGT_ARM64_MAC) {
#endif
      fprintf(stderr,
              "rho: corpus execution runs on wasm32-wasi (wasmtime) or "
              "arm64-mac (in-tree image)%s\n",
#if defined(__linux__)
              " or the linux targets (in-tree images)"
#else
              "; the linux targets build images but need a linux host to run"
#endif
      );
      return 2;
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
      sb_printf(&run_cmd, "%s > /tmp/rho_out_XXXX 2>/dev/null", built);
      // use a fixed temp name for determinism
      const char *tmpout = arena_printf("/tmp/rho_out_%s", name);
      SB rc = {0};
      int code;
      if (tt == TGT_ARM64_MAC || tt == TGT_ARM64_LINUX || tt == TGT_AMD64_LINUX) {
        // a runnable image the toolchain built itself: execute directly
        sb_printf(&rc, "%s > %s", built, tmpout);
      } else {
        // wasmtime prints stdout to fd 1; panic exit 101 propagates
        sb_printf(&rc, "wasmtime run %s > %s 2>/dev/null", built, tmpout);
      }
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
#if !defined(__linux__)
  if (!strcmp(cmd, "run") && target != TGT_WASM32_WASI && target != TGT_ARM64_MAC) {
    fprintf(stderr, "rho: run supports wasm32-wasi (wasmtime) and arm64-mac "
                    "(in-tree image); linux targets need a linux host\n");
    return 2;
  }
#endif
  const char *bin;
  if (!strcmp(cmd, "build")) {
    bin = out ? out : (target == TGT_WASM32_WASI ? "a.wasm" : "a.s");
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
    if (target == TGT_ARM64_MAC || target == TGT_ARM64_LINUX ||
        target == TGT_AMD64_LINUX) {
      sb_append_c(&rc, built);
      for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
          for (int j = i + 1; j < argc; j++)
            sb_printf(&rc, " \"%s\"", argv[j]);
          break;
        }
      }
    } else {
      sb_printf(&rc, "wasmtime run %s", built);
      for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
          for (int j = i + 1; j < argc; j++)
            sb_printf(&rc, " \"%s\"", argv[j]);
          break;
        }
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
          "rho 0.3.4\n"
          "usage: rho <command> [args]\n"
          "  check  <file>              parse + typecheck\n"
          "  fmt    [-w] <file>         print canonical formatting\n"
          "  build  <file> [-o out]     emit the artifact for --target\n"
          "                             (default wasm32-wasi: a runnable module;\n"
          "                              arm64-mac: a static Mach-O, signed;\n"
          "                              linux targets: static ELF images)\n"
          "  run    <file> [-- args]    build + execute (wasm32-wasi via\n"
          "                             wasmtime; native images run directly\n"
          "                             on a matching host)\n"
          "  test   [dir] [--target t]  compile + run corpus programs\n"
          "                             (wasm32-wasi, arm64-mac, or linux\n"
          "                             targets on a linux host)\n"
          "  selftest [--update-goldens] [--fmt]\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const char *cmd = argv[1];

  if (!strcmp(cmd, "--version") || !strcmp(cmd, "version")) {
    printf("rho 0.3.4\n");
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

  if (!strcmp(cmd, "build") || !strcmp(cmd, "run") || !strcmp(cmd, "test"))
    return cmd_build_run_test(cmd, argc, argv);

  if (!strcmp(cmd, "asmtest")) {
    // development-time oracle: assemble rho assembly to a raw image so
    // tools/check_asm64.sh can diff it against the system assembler
    if (argc < 4) {
      usage();
      return 2;
    }
    Str src = read_file_or_die(str_from(argv[2]));
    char *buf = arena_alloc(src.n + 1);
    memcpy(buf, src.p, src.n);
    buf[src.n] = 0;
    return asm64_test(buf, argv[3]);
  }

  usage();
  return 2;
}
