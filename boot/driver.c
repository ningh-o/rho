// driver.c — command implementations. T1.1 delivers the skeleton;
// each command fills in as its phase lands (see TODO.md Phase 1).
#include "rho.h"

int cmd_build(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr, "rho build: not implemented yet (Phase 1, T1.7+)\n");
  return EXIT_USAGE;
}

int cmd_run(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr, "rho run: not implemented yet (Phase 1, T1.9+)\n");
  return EXIT_USAGE;
}

int cmd_test(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr, "rho test: not implemented yet (Phase 1, T1.9+)\n");
  return EXIT_USAGE;
}

int cmd_fmt(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr, "rho fmt: not implemented yet (Phase 1, T1.9)\n");
  return EXIT_USAGE;
}

int cmd_check(int argc, char **argv) {
  if (argc < 1) {
    fprintf(stderr, "rho check <file.rho> [--set name=value]...\n");
    return EXIT_USAGE;
  }
  const char *path = argv[0];
  Program *p = program_new();
  if (!program_load_graph(p, path)) {
    diags_print(stderr);
    return EXIT_COMPILE;
  }
  bool ok = check_program(p);
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
