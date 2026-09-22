// Minimal WebAssembly JavaScript API surface used by the WASI shim and the
// compiler worker. Declared locally because the usual source of the
// WebAssembly namespace (lib.dom / lib.webworker) is deliberately not
// enabled for this node package.

declare namespace WebAssembly {
  /** Constructor takes .wasm bytes; compiled modules are cacheable. */
  class Module {
    constructor(bytes: ArrayBufferView | ArrayBuffer);
    static customSections(module: Module, name: string): unknown[];
    static exports(module: Module): unknown[];
    static imports(module: Module): unknown[];
  }

  class Instance {
    constructor(module: Module, imports?: Imports);
    readonly exports: Record<string, unknown>;
  }

  class Memory {
    constructor(descriptor: { initial: number; maximum?: number; shared?: boolean });
    readonly buffer: ArrayBuffer;
    grow(delta: number): number;
  }

  function compile(bytes: ArrayBufferView | ArrayBuffer): Promise<Module>;
  function instantiate(
    module: Module,
    imports?: Imports,
  ): Promise<Instance>;
  function instantiate(
    bytes: ArrayBufferView | ArrayBuffer,
    imports?: Imports,
  ): Promise<{ module: Module; instance: Instance }>;

  interface Imports {
    [module: string]: Record<string, unknown> | undefined;
  }
}
