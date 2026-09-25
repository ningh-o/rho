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
  (void)argc;
  (void)argv;
  fprintf(stderr, "rho check: not implemented yet (Phase 1, T1.6+)\n");
  return EXIT_USAGE;
}

int cmd_dump_ast(const char *path) {
  (void)path;
  fprintf(stderr, "rho dump-ast: not implemented yet (Phase 1, T1.3)\n");
  return EXIT_USAGE;
}
