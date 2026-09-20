// The static Mach-O writer for arm64-mac images: a fixed-address
// executable (header, __PAGEZERO, __TEXT with __text/__rhostr, __DATA with
// __data/__heap zero-fill, LC_MAIN) plus the ad-hoc code signature Apple
// Silicon requires — SHA-256 page hashes in a SuperBlob, written in-tree.
// No linker, no codesign tool, nothing external.

#include "ir.h"

#include <sys/stat.h>

// the zero-fill bump heap: its own trailing segment, fixed address
#define HEAP_BYTES 0x1000000ull // 16 MiB
#define HEAP_VADDR_OFF 0x1000000ull // heap vmaddr = VM_BASE + this

// ---------------------------------------------------------------- sha256 --

typedef struct {
  uint32_t h[8];
  uint64_t len;
  unsigned char buf[64];
  size_t n;
} Sha256;

static uint32_t shr(uint32_t x, int n) { return x >> n; }
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_init(Sha256 *s) {
  static const uint32_t H0[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
  memcpy(s->h, H0, sizeof(H0));
  s->len = 0;
  s->n = 0;
}

static void sha256_block(Sha256 *s, const unsigned char *p) {
  static const uint32_t K[64] = {
      0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1,
      0x923F82A4, 0xAB1C5ED5, 0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
      0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174, 0xE49B69C1, 0xEFBE4786,
      0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
      0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147,
      0x06CA6351, 0x14292967, 0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
      0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85, 0xA2BFE8A1, 0xA81A664B,
      0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
      0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A,
      0x5B9CCA4F, 0x682E6FF3, 0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
      0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2};
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
           (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ shr(w[i - 15], 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ shr(w[i - 2], 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
  s->h[5] += f;
  s->h[6] += g;
  s->h[7] += h;
}

static void sha256_update(Sha256 *s, const void *data, size_t n) {
  const unsigned char *p = data;
  s->len += n;
  while (n) {
    size_t take = 64 - s->n < n ? 64 - s->n : n;
    memcpy(s->buf + s->n, p, take);
    s->n += take;
    p += take;
    n -= take;
    if (s->n == 64) {
      sha256_block(s, s->buf);
      s->n = 0;
    }
  }
}

static void sha256_final(Sha256 *s, unsigned char out[32]) {
  uint64_t bits = s->len * 8;
  unsigned char pad = 0x80;
  sha256_update(s, &pad, 1);
  unsigned char z = 0;
  while (s->n != 56)
    sha256_update(s, &z, 1);
  unsigned char lenb[8];
  for (int i = 0; i < 8; i++)
    lenb[i] = (unsigned char)(bits >> (56 - i * 8));
  sha256_update(s, lenb, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (unsigned char)(s->h[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
    out[i * 4 + 3] = (unsigned char)s->h[i];
  }
}

// ---------------------------------------------------------------- macho ---

static const uint64_t VM_BASE = 0x100000000ull; // main-image load address
static void put32(SB *b, uint32_t v) {
  sb_printf(b, "%c%c%c%c", (int)(v & 0xFF), (int)((v >> 8) & 0xFF),
            (int)((v >> 16) & 0xFF), (int)((v >> 24) & 0xFF));
}

static void put64(SB *b, uint64_t v) {
  for (int i = 0; i < 8; i++)
    sb_printf(b, "%c", (int)((v >> (8 * i)) & 0xFF));
}

static void put_be32(SB *b, uint32_t v) {
  sb_printf(b, "%c%c%c%c", (int)((v >> 24) & 0xFF), (int)((v >> 16) & 0xFF),
            (int)((v >> 8) & 0xFF), (int)(v & 0xFF));
}

static void put_be64(SB *b, uint64_t v) {
  for (int i = 7; i >= 0; i--)
    sb_printf(b, "%c", (int)((v >> (8 * i)) & 0xFF));
}

static void put_name(SB *b, const char *name) {
  size_t n = strlen(name);
  for (int i = 0; i < 16; i++)
    sb_printf(b, "%c", i < (int)n ? name[i] : 0);
}

// writes a section_64 entry
static void sect(SB *b, const char sect_name[16], const char seg_name[16],
                 uint64_t addr, uint64_t size, uint32_t offset, uint32_t align_exp,
                 uint32_t flags) {
  put_name(b, sect_name);
  put_name(b, seg_name);
  put64(b, addr);
  put64(b, size);
  put32(b, offset);
  put32(b, align_exp); // 2^align
  put32(b, 0);         // reloff
  put32(b, 0);         // nreloc
  put32(b, flags);
  put32(b, 0); // reserved1
  put32(b, 0); // reserved2
  put32(b, 0); // reserved3
}

int macho64_write(const SB secs[3], uint64_t text_vaddr,
                  uint64_t data_vaddr, uint64_t heap_vaddr, const char *out_path) {
  const uint32_t page = 4096;   // signature page size (log2 12) — Apple's
                                // own ld signs arm64 images at 4K too
  const uint64_t segpage = 0x4000; // arm64 segments map on 16K boundaries;
                                   // the kernel's strict validation rejects
                                   // any other segment size

  uint64_t text_size = secs[0].n;
  uint64_t str_size = secs[1].n;
  uint64_t data_size = secs[2].n;

  uint64_t hdr = text_vaddr - VM_BASE;
  uint64_t text_end = data_vaddr - VM_BASE; // 16K-aligned (asm64 lays it out)
  uint64_t data_fileoff = data_vaddr - VM_BASE;
  // segment plan (one page discipline, no segment ever overlaps another):
  //   __TEXT  [VM_BASE, +text_end)                    r-x, file-backed
  //   __DATA  [VM_BASE+text_end, +data_fs)            rw-, file-backed
  //   __LINKEDIT [.., +fixups+trie+sig)               r--, file-backed
  //   __HEAP  fixed VM_BASE+HEAP_VADDR_OFF            rw-, zero-fill (BSS)
  // The heap is its own trailing segment: a vmsize>filesize tail on __DATA
  // pushes __LINKEDIT past it, and the kernel chokes on a vaddr-fileoff
  // delta that far out. A fixed offset keeps every delta small; the heap
  // starts zeroed exactly as the prelude's reference counting expects.
  uint64_t data_fs = (data_size + segpage - 1) & ~(segpage - 1);
  // __LINKEDIT = minimal chained-fixups table + empty exports trie + the
  // signature (AppleSystemPolicy kills MH_DYLDLINK images whose fixups or
  // trie commands are absent or empty — dyld must find parseable blobs)
  const uint32_t FIXUPS_BYTES = 56; // ld's exact shape: header, pad, 5-seg table
  const uint32_t TRIE_BYTES = 8;    // one terminal byte + pad
  uint64_t le_off = data_fileoff + data_fs; // still 16K-aligned
  uint64_t fixups_off = le_off;
  uint64_t trie_off = le_off + FIXUPS_BYTES;
  uint64_t sig_off = le_off + FIXUPS_BYTES + TRIE_BYTES; // codeLimit
  uint32_t sig_nslots = (uint32_t)((sig_off + page - 1) / page);
  uint32_t sig_size = 20 + 88 + 4 + sig_nslots * 32;
  uint64_t le_fs = FIXUPS_BYTES + TRIE_BYTES + sig_size;

  SB b = {0};
  put32(&b, 0xFEEDFACF);
  put32(&b, 0x0100000C);         // cputype ARM64
  put32(&b, 0);                  // cpusubtype
  put32(&b, 2);                  // MH_EXECUTE
  put32(&b, 14);                 // ncmds
  put32(&b, ARM64_MAC_HDR - 32); // sizeofcmds
  put32(&b, 0x00200085);         // flags: NOUNDEFS|DYLDLINK|TWOLEVEL|PIE
  put32(&b, 0);                  // reserved

  // LC_SEGMENT_64 __PAGEZERO
  put32(&b, 0x19);
  put32(&b, 72);
  put_name(&b, "__PAGEZERO");
  put64(&b, 0);
  put64(&b, VM_BASE);
  put64(&b, 0);
  put64(&b, 0);
  put32(&b, 0);
  put32(&b, 0);
  put32(&b, 0);
  put32(&b, 0);

  // LC_SEGMENT_64 __TEXT (r-x)
  put32(&b, 0x19);
  put32(&b, 152);
  put_name(&b, "__TEXT");
  put64(&b, VM_BASE);
  put64(&b, text_end);
  put64(&b, 0);
  put64(&b, text_end);
  put32(&b, 5);
  put32(&b, 5);
  put32(&b, 1);
  put32(&b, 0);
  sect(&b, "__text", "__TEXT", text_vaddr, text_size, (uint32_t)hdr, 3, 0x80000400);

  // LC_SEGMENT_64 __DATA (rw-): file-backed, vmsize == filesize
  put32(&b, 0x19);
  put32(&b, 152);
  put_name(&b, "__DATA");
  put64(&b, data_vaddr);
  put64(&b, data_fs);
  put64(&b, data_fileoff);
  put64(&b, data_fs);
  put32(&b, 3);
  put32(&b, 3);
  put32(&b, 1);
  put32(&b, 0);
  sect(&b, "__data", "__DATA", data_vaddr, data_size, (uint32_t)data_fileoff, 3, 0);

  // LC_SEGMENT_64 __HEAP (rw-): the 16 MiB bump heap as a pure zero-fill
  // segment at a fixed address — nothing in the file, fresh zero pages
  put32(&b, 0x19);
  put32(&b, 152);
  put_name(&b, "__HEAP");
  put64(&b, VM_BASE + HEAP_VADDR_OFF);
  put64(&b, HEAP_BYTES);
  put64(&b, 0);
  put64(&b, 0);
  put32(&b, 3);
  put32(&b, 3);
  put32(&b, 1);
  put32(&b, 0);
  sect(&b, "__rhoheap", "__HEAP", heap_vaddr, HEAP_BYTES, 0, 4, 1); // S_ZEROFILL

  // LC_SEGMENT_64 __LINKEDIT (r--): fixups + trie + the code signature.
  // Its vmaddr sits PAST the BSS heap — a segment overlapping __DATA's
  // zero-fill tail (where it would land if it followed the file offsets)
  // is a load-time segment overlap the kernel kills on sight; vaddr and
  // fileoff only need to agree modulo the 16K page, and both are aligned
  put32(&b, 0x19);
  put32(&b, 72);
  put_name(&b, "__LINKEDIT");
  put64(&b, VM_BASE + le_off);
  put64(&b, (le_fs + segpage - 1) & ~(segpage - 1));
  put64(&b, le_off);
  put64(&b, le_fs);
  put32(&b, 1);
  put32(&b, 1);
  put32(&b, 0);
  put32(&b, 0);

  // LC_DYLD_CHAINED_FIXUPS: a well-formed, entirely-empty fixups table —
  // three segments, no starts, no imports
  put32(&b, 0x80000034);
  put32(&b, 16);
  put32(&b, (uint32_t)fixups_off);
  put32(&b, FIXUPS_BYTES);

  // LC_DYLD_EXPORTS_TRIE: a single terminal byte — no exported symbols
  put32(&b, 0x80000033);
  put32(&b, 16);
  put32(&b, (uint32_t)trie_off);
  put32(&b, TRIE_BYTES);

  // LC_UUID: deterministic
  put32(&b, 0x1B);
  put32(&b, 24);
  for (int i = 0; i < 16; i++)
    sb_printf(&b, "%c", (int)((i * 37 + 11) & 0xFF));

  // LC_BUILD_VERSION: platform macOS, minOS/SDK 13.0.0
  put32(&b, 0x32);
  put32(&b, 24);
  put32(&b, 1);
  put32(&b, 0x000D0000);
  put32(&b, 0x000D0000);
  put32(&b, 0);

  // LC_SOURCE_VERSION: 0.0
  put32(&b, 0x2A);
  put32(&b, 16);
  put_be64(&b, 0);

  // LC_LOAD_DYLIB libSystem: present-but-unused — a MH_DYLDLINK main image
  // with zero dylibs is killed by the kernel's system policy; loading it
  // costs one mmap, nothing in the image calls it (the RT blob svc-exits)
  put32(&b, 0xC);
  put32(&b, 56); // cmdsize
  put32(&b, 24); // name offset
  put32(&b, 0);  // timestamp
  put32(&b, 0);  // current_version
  put32(&b, 0);  // compatibility_version
  sb_append_c(&b, "/usr/lib/libSystem.B.dylib");
  while (b.n % 8)
    sb_printf(&b, "%c", 0);

  // LC_LOAD_DYLINKER: on arm64 macOS every executable is entered via dyld —
  // a static image without this command is killed at exec
  put32(&b, 0xE);
  put32(&b, 32); // cmdsize: 12 + name, 8-aligned
  put32(&b, 12);
  sb_append_c(&b, "/usr/lib/dyld");
  while (b.n % 8)
    sb_printf(&b, "%c", 0);

  // LC_MAIN: dyld jumps to _rho_rt_start (the first text byte)
  put32(&b, 0x80000028);
  put32(&b, 24);
  put64(&b, hdr);
  put64(&b, 0);

  // LC_CODE_SIGNATURE — size deterministic, written before hashing
  put32(&b, 0x1D);
  put32(&b, 16);
  put32(&b, (uint32_t)sig_off);
  put32(&b, sig_size);

  // __text starts 8-aligned: pad the command block after the last command
  while (b.n % 8)
    sb_printf(&b, "%c", 0);

  if (b.n != hdr) {
    fprintf(stderr, "rho: macho64: header block %zu != expected %u\n", b.n,
            ARM64_MAC_HDR);
    return 1;
  }

  sb_append(&b, str_from_len(secs[0].buf, secs[0].n));
  sb_append(&b, str_from_len(secs[1].buf, secs[1].n));
  while (b.n < data_fileoff)
    sb_printf(&b, "%c", 0);
  sb_append(&b, str_from_len(secs[2].buf, secs[2].n));
  while (b.n < le_off) // zero pad __DATA out to its rounded size
    sb_printf(&b, "%c", 0);

  // ---- __LINKEDIT: chained fixups (LE), ld's exact shape ----
  put32(&b, 0);          // fixups_version
  put32(&b, 32);         // starts_offset
  put32(&b, FIXUPS_BYTES); // imports_offset (empty table)
  put32(&b, FIXUPS_BYTES); // symbols_offset (empty table)
  put32(&b, 0);          // imports_count
  put32(&b, 1);          // imports_format: DYLD_CHAINED_IMPORT — dyld
                         // rejects 0 ("unknown imports format") even with
                         // no imports
  put32(&b, 0);          // symbols_format
  put32(&b, 0);          // 4 bytes of pad, starts table 4-aligned at 32
  put32(&b, 5);          // segCount (every segment, incl. PAGEZERO/HEAP)
  put32(&b, 0);          // segInfoOffset[0] — no fixups anywhere
  put32(&b, 0);          // segInfoOffset[1]
  put32(&b, 0);          // segInfoOffset[2]
  put32(&b, 0);          // segInfoOffset[3]
  put32(&b, 0);          // segInfoOffset[4]
  // ---- exports trie: one terminal node, no children ----
  sb_printf(&b, "%c", 0);
  for (uint32_t i = 1; i < TRIE_BYTES; i++)
    sb_printf(&b, "%c", 0);

  // ---- ad-hoc code signature (CS_ADHOC | CS_LINKER_SIGNED) ----
  const char *ident = "rho";
  uint32_t ident_len = 4; // "rho" + NUL
  SB sig = {0};
  put_be32(&sig, 0xFADE0CC0);
  put_be32(&sig, sig_size);
  put_be32(&sig, 1);
  put_be32(&sig, 0);
  put_be32(&sig, 20);

  put_be32(&sig, 0xFADE0C02);
  put_be32(&sig, sig_size - 20);
  put_be32(&sig, 0x00020400);
  put_be32(&sig, 0x00000002); // CS_ADHOC — LINKER_SIGNED without a real provenance chain is what the kernel's system policy kills
  put_be32(&sig, 88 + ident_len);
  put_be32(&sig, 88);
  put_be32(&sig, 0);
  put_be32(&sig, sig_nslots);
  put_be32(&sig, (uint32_t)sig_off);
  sig.buf[sig.n++] = 32;
  sig.buf[sig.n++] = 2;
  sig.buf[sig.n++] = 0;
  sig.buf[sig.n++] = 12;
  put_be32(&sig, 0);
  put_be32(&sig, 0);
  put_be32(&sig, 0);
  put_be32(&sig, 0);
  put_be64(&sig, 0);
  put_be64(&sig, 0);
  put_be64(&sig, text_end);
  put_be64(&sig, 1); // CS_EXECSEG_MAIN_BINARY
  sb_append(&sig, str_from_len(ident, ident_len));
  for (uint32_t p = 0; p < sig_nslots; p++) {
    uint32_t start = p * page;
    uint32_t take = sig_off - start < page ? (uint32_t)(sig_off - start) : page;
    Sha256 sha;
    sha256_init(&sha);
    sha256_update(&sha, b.buf + start, take);
    unsigned char digest[32];
    sha256_final(&sha, digest);
    sb_append(&sig, str_from_len((char *)digest, 32));
  }

  if (sig.n != sig_size) {
    fprintf(stderr, "rho: macho64: signature %zu != expected %u\n", sig.n, sig_size);
    return 1;
  }

  FILE *f = fopen(out_path, "wb");
  if (!f) {
    fprintf(stderr, "rho: cannot write %s\n", out_path);
    return 1;
  }
  fwrite(b.buf, 1, b.n, f);
  fwrite(sig.buf, 1, sig.n, f);
  // the file ends exactly at dataoff+datasize — anything after the
  // signature fails the kernel's strict validation
  fclose(f);
  chmod(out_path, 0755);
  return 0;
}
