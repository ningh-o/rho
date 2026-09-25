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
        ;; grow by ceil((need - current) / 64K) pages
        (drop (memory.grow
          (i32.div_u (i32.add (i32.sub (local.get $need)
                                        (i32.shl (memory.size) (i32.const 16)))
                               (i32.const 65535))
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
    ;; statics (data literals below the heap) are immortal
    (if (i32.lt_u (local.get $p) (i32.const 65536)) (then (return)))
    (if (i32.eqz (local.get $p)) (then (return)))
    (i32.store (i32.sub (local.get $p) (i32.const 24))
      (i32.add (i32.load (i32.sub (local.get $p) (i32.const 24)))
               (i32.const 1))))

  (func $rho_release (param $p i32)
    (local $rc i32) (local $drop i32)
    ;; statics (data literals below the heap) are immortal
    (if (i32.lt_u (local.get $p) (i32.const 65536)) (then (return)))
    (if (i32.eqz (local.get $p)) (then (return)))
    (local.set $rc (i32.load (i32.sub (local.get $p) (i32.const 24))))
    (local.set $rc (i32.sub (local.get $rc) (i32.const 1)))
    (i32.store (i32.sub (local.get $p) (i32.const 24)) (local.get $rc))
    ;; at rc==0: run the drop fn (payload pointer = p+24) when set,
    ;; then the block dies (freed to the allocator's list later; the
    ;; bump kernel keeps memory for 0.1.0 bring-up, wrc keeps headers)
    (if (i32.eqz (local.get $rc))
      (then
        (local.set $drop (i32.load (i32.sub (local.get $p)
                                             (i32.const 12))))
        (if (i32.ge_s (local.get $drop) (i32.const 0))
          (then (call_indirect (type $dropfn) (local.get $p)
                               (local.get $drop)))))))

  (type $dropfn (func (param i32)))
  ;; EMITTER-OWNS-BEGIN (standalone-test table; stripped from the embed)
  (table 4 funcref)
  (elem (i32.const 0) $rho_nodrop $rho_nodrop $rho_nodrop $rho_nodrop)
  ;; EMITTER-OWNS-END
  (func $rho_nodrop (param $p i32))

  ;; raw write to fd 1 with the scratch iov at 0
  (func $print_mem (param $p i32) (param $n i32)
    (i32.store (i32.const 0) (local.get $p))
    (i32.store (i32.const 4) (local.get $n))
    (drop (call $fd_write (i32.const 1) (i32.const 0)
                          (i32.const 1) (i32.const 16))))

  ;; write to stderr
  (func $eprint_mem (param $p i32) (param $n i32)
    (i32.store (i32.const 0) (local.get $p))
    (i32.store (i32.const 4) (local.get $n))
    (drop (call $fd_write (i32.const 2) (i32.const 0)
                          (i32.const 1) (i32.const 16))))

  ;; format-build scratch: [64, 4096); overflow = defined panic
  ;; (grew from 2048: the self-hosted compiler prints program-sized
  ;; WAT through it — the data literals start at 4096, so the scratch
  ;; fills the gap exactly)
  (global $fb (mut i32) (i32.const 64))
  (func $fb_reset
    (global.set $fb (i32.const 64)))
  (func $fb_push (param $p i32) (param $n i32)
    (local $i i32)
    (if (i32.gt_u (i32.add (global.get $fb) (local.get $n))
                  (i32.const 4096))
      (then (call $rho_panic (i32.const 2048) (i32.const 7))))
    (local.set $i (i32.const 0))
    (block $d (loop $c
      (br_if $d (i32.ge_u (local.get $i) (local.get $n)))
      (i32.store8 (i32.add (global.get $fb) (local.get $i))
        (i32.load8_u (i32.add (local.get $p) (local.get $i))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $c)))
    (global.set $fb (i32.add (global.get $fb) (local.get $n))))
  (func $fb_u64 (param $v i64)
    (local $i i32)
    (local.set $i (i32.const 0))
    (if (i64.eqz (local.get $v))
      (then
        (i32.store8 (i32.const 1023) (i32.const 48))
        (call $fb_push (i32.const 1023) (i32.const 1))
        (return)))
    (block $done (loop $d2
      (br_if $done (i64.eqz (local.get $v)))
      (i32.store8 (i32.sub (i32.const 1023) (local.get $i))
        (i32.add (i32.wrap_i64 (i64.rem_u (local.get $v) (i64.const 10)))
                 (i32.const 48)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (local.set $v (i64.div_u (local.get $v) (i64.const 10)))
      (br $d2)))
    (call $fb_push (i32.sub (i32.const 1024) (local.get $i))
                   (local.get $i)))
  (func $fb_i64 (param $v i64)
    (if (i64.lt_s (local.get $v) (i64.const 0))
      (then
        (i32.store8 (i32.const 1024) (i32.const 45))
        (call $fb_push (i32.const 1024) (i32.const 1))
        (call $fb_u64 (i64.sub (i64.const 0) (local.get $v))))
      (else (call $fb_u64 (local.get $v)))))

  ;; copy the current scratch contents to a block payload
  (func $fb_copy_to (param $dst i32)
    (local $i i32)
    (local.set $i (i32.const 0))
    (block $d (loop $c
      (br_if $d (i32.ge_u (i32.add (i32.const 64) (local.get $i))
                          (global.get $fb)))
      (i32.store8 (i32.add (local.get $dst) (local.get $i))
        (i32.load8_u (i32.add (i32.const 64) (local.get $i))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $c))))

  ;; weak handle: block stores the target's header ptr; wrc tracks
  (func $rho_weak_from (param $payload i32) (result i32)
    (local $p i32)
    ;; bump the target's weak count
    (i32.store (i32.sub (local.get $payload) (i32.const 16))
      (i32.add (i32.load (i32.sub (local.get $payload) (i32.const 16)))
               (i32.const 1)))
    (local.set $p (call $rho_alloc (i32.const 4)))
    (i32.store (local.get $p)
               (i32.sub (local.get $payload) (i32.const 24)))
    (local.get $p))

  ;; weak.get: ?*T — the target payload when alive, None when dead
  (func $rho_weak_alive (param $wp i32) (result i32)
    (i32.gt_s
      (i32.load (i32.add (i32.load (local.get $wp)) (i32.const 0)))
      (i32.const 0)))
  (func $rho_weak_payload (param $wp i32) (result i32)
    (i32.add (i32.load (local.get $wp)) (i32.const 24)))

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
