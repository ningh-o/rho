#include "ir.h"

// wasm32-wasi emitter — implementation follows docs/wasm-design.md
// (locked design). Reserved for the 0.0.4 milestone; the native
// backends do not depend on this file.
void emit_wasm(Target target, SB *out) {
  (void)target;
  (void)out;
}
