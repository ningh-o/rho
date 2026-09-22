// not a kernel: measures the runtime process overhead (spawn -> exit of an
// empty wasm32-wasi module) so kernel medians can be read against it.
#include <stdlib.h>
int main(void) { return 0; }
