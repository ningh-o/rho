;; kernel.wat — the emitter-level runtime kernel (allocator, rc glue,
;; decimal printing, wasi tail). Standalone: assembled and smoke-tested
;; on its own; the emitter embeds this text at module build.
(module
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit"
    (func $proc_exit (param i32)))

  ;; memory layout: [0,64) scratch/iov, [64, 64+FMT_CAP) fmt buffer,
  ;; data literals follow, then the bump heap.
  (memory (export "memory") 1)
  (global $heap (mut i32) (i32.const 65536))

  ;; bump allocation, zeroed, with the 24-byte header
  ;; {rc,sz,wrc,drop} at block start; payload at +24
  (func $rho_alloc (param $sz i32) (result i32)
    (local $p i32) (local $i i32) (local $need i32)
    ;; grow memory when the block would cross the end
    (local.set $need (i32.add (i32.add (global.get $heap) (local.get $sz))
                              (i32.const 65535)))
    (if (i32.gt_u (local.get $need) (i32.shl (memory.size) (i32.const 16)))
      (then
        (drop (memory.grow
          (i32.div_u (i32.sub (local.get $need)
                              (i32.shl (memory.size) (i32.const 16)))
                     (i32.const 65536))))))
    (local.set $p (global.get $heap))
    (global.set $heap
      (i32.add (global.get $heap)
               (i32.and (i32.add (i32.add (local.get $sz) (i32.const 31))
                                 (i32.const 7))
                        (i32.const -8))))
    (local.set $i (i32.const 0))
    (block $done
      (loop $z
        (br_if $done
          (i32.ge_u (local.get $i) (i32.add (local.get $sz) (i32.const 24))))
        (i32.store8 (i32.add (local.get $p) (local.get $i))
                    (i32.const 0))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $z)))
    (i32.store (local.get $p) (i32.const 1))                       ;; rc
    (i32.store (i32.add (local.get $p) (i32.const 4)) (local.get $sz)) ;; sz
    (i32.store (i32.add (local.get $p) (i32.const 8)) (i32.const 0)) ;; wrc
    (i32.store (i32.add (local.get $p) (i32.const 12)) (i32.const -1)) ;; drop
    (local.get $p))

  ;; rc glue
  (func $rho_retain (param $p i32)
    (if (i32.eqz (local.get $p)) (then (return)))
    (i32.store (local.get $p)
      (i32.add (i32.load (local.get $p)) (i32.const 1))))

  (func $rho_release (param $p i32)
    (local $rc i32) (local $drop i32)
    (if (i32.eqz (local.get $p)) (then (return)))
    (local.set $rc (i32.load (local.get $p)))
    (local.set $rc (i32.sub (local.get $rc) (i32.const 1)))
    (i32.store (local.get $p) (local.get $rc))
    ;; at rc==0: run the drop fn (payload pointer = p+24) when set,
    ;; then the block dies (freed to the allocator's list later; the
    ;; bump kernel keeps memory for 0.1.0 bring-up, wrc keeps headers)
    (if (i32.eqz (local.get $rc))
      (then
        (local.set $drop (i32.load (i32.add (local.get $p) (i32.const 12))))
        (if (i32.ge_s (local.get $drop) (i32.const 0))
          (then (call_indirect (type $dropfn) (i32.add (local.get $p)
                                                       (i32.const 24))
                               (local.get $drop)))))))

  (table 4 funcref)
  (elem (i32.const 0) $rho_nodrop $rho_nodrop $rho_nodrop $rho_nodrop)
  (type $dropfn (func (param i32)))
  (func $rho_nodrop (param $p i32))

  ;; raw write to fd 1 with the scratch iov at 0
  (func $print_mem (param $p i32) (param $n i32)
    (i32.store (i32.const 0) (local.get $p))
    (i32.store (i32.const 4) (local.get $n))
    (drop (call $fd_write (i32.const 1) (i32.const 0)
                          (i32.const 1) (i32.const 16))))

  ;; decimal u64 into the fmt buffer end [fmt_lo, fmt_hi)
  (func $fmt_u64 (param $v i64)
    (local $i i32)
    (local.set $i (i32.const 0))
    (if (i64.eqz (local.get $v))
      (then
        (i32.store8 (i32.const 1023) (i32.const 48))
        (call $print_mem (i32.const 1023) (i32.const 1))
        (return)))
    (block $done
      (loop $d
        (br_if $done (i64.eqz (local.get $v)))
        (i32.store8 (i32.sub (i32.const 1023) (local.get $i))
          (i32.add (i32.wrap_i64 (i64.rem_u (local.get $v) (i64.const 10)))
                   (i32.const 48)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (local.set $v (i64.div_u (local.get $v) (i64.const 10)))
        (br $d)))
    (call $print_mem (i32.sub (i32.const 1024) (local.get $i))
                     (local.get $i)))

  (func $print_u64 (param $v i64)
    (call $fmt_u64 (local.get $v)))

  (func $print_i64 (param $v i64)
    (if (i64.lt_s (local.get $v) (i64.const 0))
      (then
        (i32.store8 (i32.const 1024) (i32.const 45))
        (call $print_mem (i32.const 1024) (i32.const 1))
        ;; careful: MIN/-1 wraps; magnitude via u64 arithmetic is exact
        (call $fmt_u64 (i64.sub (i64.const 0) (local.get $v))))
      (else (call $fmt_u64 (local.get $v)))))

  ;; panic: "panic: <msg>\n" to stderr, exit 101
  (func $rho_panic (param $p i32) (param $n i32)
    (i32.store (i32.const 0) (i32.const 2048))
    (i32.store (i32.const 4) (i32.const 7))
    (i32.store (i32.const 8) (local.get $p))
    (i32.store (i32.const 12) (local.get $n))
    (i32.store8 (i32.const 2056) (i32.const 10))
    (i32.store (i32.const 16) (i32.const 2056))
    (i32.store (i32.const 20) (i32.const 1))
    (drop (call $fd_write (i32.const 2) (i32.const 0)
                          (i32.const 3) (i32.const 24)))
    (call $proc_exit (i32.const 101)))

  (data (i32.const 2048) "panic: ")

  ;; smoke: _start prints a few values (kept out of production builds
  ;; by the emitter — this text is embedded piecewise, not wholesale)
  (func $kernel_smoke (export "kernel_smoke")
    (call $print_mem (i32.const 2048) (i32.const 7))
    (call $print_i64 (i64.const 0))
    (call $print_i64 (i64.const 42))
    (call $print_i64 (i64.const -7))
    (call $print_i64 (i64.const 9223372036854775807))
    (call $print_i64 (i64.const -9223372036854775808))
    (i32.store8 (i32.const 100) (i32.const 10))
    (call $print_mem (i32.const 100) (i32.const 1)))
)
