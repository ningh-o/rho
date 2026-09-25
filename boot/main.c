// main.c — CLI entry and the selftest harness.
#include "rho.h"

#include <stdarg.h>

static void usage(FILE *out) {
  fprintf(out,
          "rho — the seed compiler (boot). 0.1.0 target: wasm32-wasi.\n"
          "usage:\n"
          "  rho build <file.rho> [-o out.wasm] [--set name=value]... [-g]\n"
          "  rho run   <file.rho> [--set name=value]... [-- <args>]\n"
          "  rho test  <dir-or-file>\n"
          "  rho fmt   <file.rho>\n"
          "  rho check <file.rho> [--set name=value]...\n"
          "  rho dump-ast <file.rho>\n"
          "  rho selftest\n");
}

int main(int argc, char **argv) {
  g_arena = arena_new(1 << 20);
  vec_init(&g_diags, sizeof(Diag));
  if (argc < 2) {
    usage(stderr);
    return EXIT_USAGE;
  }
  const char *cmd = argv[1];
  if (strcmp(cmd, "build") == 0)
    return cmd_build(argc - 2, argv + 2);
  if (strcmp(cmd, "run") == 0)
    return cmd_run(argc - 2, argv + 2);
  if (strcmp(cmd, "test") == 0)
    return cmd_test(argc - 2, argv + 2);
  if (strcmp(cmd, "fmt") == 0)
    return cmd_fmt(argc - 2, argv + 2);
  if (strcmp(cmd, "check") == 0)
    return cmd_check(argc - 2, argv + 2);
  if (strcmp(cmd, "selftest") == 0)
    return cmd_selftest();
  if (strcmp(cmd, "dump-ast") == 0) {
    if (argc != 3) {
      usage(stderr);
      return EXIT_USAGE;
    }
    return cmd_dump_ast(argv[2]);
  }
  usage(stderr);
  return EXIT_USAGE;
}
