// prelude.c — prelude.rho embedded verbatim (generated form; the
// .rho file is the source of truth — regenerate with
// tools/embed_prelude.py).
const char *prelude_src(void) {
  return
    "// prelude.rho — the kernel, part 1 (grows to its full form in T1.8).\n"
    "// Loaded before every program; its public names are the prelude.\n"
    "// printf/eprintf/format/len/panic/assert are compiler-intrinsic call\n"
    "// targets: their signatures here anchor checking, the emitter\n"
    "// implements them (format desugar per spec §5, panic per spec §2).\n"
    "\n"
    "pub enum Option[T] {\n"
    "  Some(T),\n"
    "  None,\n"
    "}\n"
    "\n"
    "pub enum Result[T, E] {\n"
    "  Ok(T),\n"
    "  Err(E),\n"
    "}\n"
    "\n"
    "pub fn printf(fmt: string) {}\n"
    "\n"
    "pub fn eprintf(fmt: string) {}\n"
    "\n"
    "pub fn format(fmt: string) -> string {\n"
    "  return fmt;\n"
    "}\n"
    "\n"
    "pub fn len(s: string) -> usize {\n"
    "  return 0 as usize;\n"
    "}\n"
    "\n"
    "pub fn make() -> usize { return 0 as usize; }\n"
    "\n"
    "pub fn panic(msg: string) {}\n"
    "\n"
    "pub fn assert(cond: bool, msg: string) {}\n"
    "\n"
    "pub fn exit(code: i32) {}\n";
      ;
}
