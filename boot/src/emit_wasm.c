#include "ir.h"

// wasm32-wasi emitter — implementation planned for 0.0.4 (see
// docs/wasm-design.md for the locked design). The native backends do not
// depend on this file.
void emit_wasm(Target target, SB *out) {
  (void)target;
  (void)out;
}
