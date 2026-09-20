// The static ELF64 writer for the Linux targets (amd64-linux, arm64-linux):
// an ET_EXEC image with two PT_LOAD segments — r-x covering the ELF header,
// program headers, text and the rodata strings folded into it; rw- covering
// the data image with a zero-fill tail that is the bump-allocator heap —
// plus PT_GNU_STACK so the stack stays non-executable. No interpreter, no
// relocations, no section table: the kernel maps and jumps.
//
// Layout contract (must mirror what the assemblers compute): text starts at
// `text_vaddr` = ELF64_HDR past the 0x400000 base, the data image starts at
// `data_vaddr` with file offset data_vaddr - 0x400000, and the heap begins
// at `heap_vaddr`, page-aligned past the data image. The entry point is the
// first text byte — every image prepends its runtime blob, whose first label
// is the entry.

#include "ir.h"

#include <sys/stat.h>

// the zero-fill bump heap lives in BSS: p_memsz runs past p_filesz and the
// kernel hands out zero pages, so it costs nothing in the file
#define HEAP_BYTES 0x1000000ull // 16 MiB

static void put16(SB *b, uint32_t v) {
  sb_printf(b, "%c%c", (int)(v & 0xFF), (int)((v >> 8) & 0xFF));
}

static void put32(SB *b, uint32_t v) {
  sb_printf(b, "%c%c%c%c", (int)(v & 0xFF), (int)((v >> 8) & 0xFF),
            (int)((v >> 16) & 0xFF), (int)((v >> 24) & 0xFF));
}

static void put64(SB *b, uint64_t v) {
  for (int i = 0; i < 8; i++)
    sb_printf(b, "%c", (int)((v >> (8 * i)) & 0xFF));
}

static void phdr(SB *b, uint32_t type, uint32_t flags, uint64_t off,
                 uint64_t vaddr, uint64_t filesz, uint64_t memsz,
                 uint64_t align) {
  put32(b, type);
  put32(b, flags);
  put64(b, off);
  put64(b, vaddr);
  put64(b, vaddr); // p_paddr
  put64(b, filesz);
  put64(b, memsz);
  put64(b, align);
}

int elf64_write(const SB secs[3], int machine, uint64_t text_vaddr,
                uint64_t data_vaddr, uint64_t heap_vaddr, const char *out_path) {
  const uint64_t base = 0x400000ull; // fixed non-PIE load address
  const uint64_t hdr = ELF64_HDR;    // 64-byte Ehdr + 3 program headers

  uint64_t text_size = secs[0].n + secs[1].n; // strings fold into the r-x page
  uint64_t data_size = secs[2].n;
  uint64_t off_data = data_vaddr - base; // congruent: vaddr - offset = 0x400000

  (void)text_size;
  if (text_vaddr != base + hdr || heap_vaddr < data_vaddr + data_size) {
    fprintf(stderr, "rho: elf64: layout mismatch (text %#llx data %#llx heap %#llx)\n",
            (unsigned long long)text_vaddr, (unsigned long long)data_vaddr,
            (unsigned long long)heap_vaddr);
    return 1;
  }

  SB b = {0};

  // ---- Ehdr ----
  static const char ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
  sb_append(&b, str_from_len(ident, 16));
  put16(&b, 2);          // e_type = ET_EXEC
  put16(&b, (uint32_t)machine);
  put32(&b, 1);          // e_version
  put64(&b, text_vaddr); // e_entry — first text byte (the runtime blob)
  put64(&b, 64);         // e_phoff
  put64(&b, 0);          // e_shoff — no section table
  put32(&b, 0);          // e_flags
  put16(&b, 64);         // e_ehsize
  put16(&b, 56);         // e_phentsize
  put16(&b, 3);          // e_phnum
  put16(&b, 0);          // e_shentsize
  put16(&b, 0);          // e_shnum
  put16(&b, 0);          // e_shstrndx

  // ---- Phdr[0]: r-x text (header block + text + folded strings) ----
  phdr(&b, 1, 5, 0, base, off_data, off_data, 0x10000);

  // ---- Phdr[1]: rw- data, BSS tail = the bump heap ----
  phdr(&b, 1, 6, off_data, data_vaddr, data_size,
       heap_vaddr + HEAP_BYTES - data_vaddr, 0x10000);

  // ---- Phdr[2]: PT_GNU_STACK — non-executable stack ----
  phdr(&b, 0x6474E551u, 6, 0, 0, 0, 0, 0x10);

  if (b.n != hdr) {
    fprintf(stderr, "rho: elf64: header block %zu != expected %llu\n", b.n,
            (unsigned long long)hdr);
    return 1;
  }

  sb_append(&b, str_from_len(secs[0].buf, secs[0].n));
  sb_append(&b, str_from_len(secs[1].buf, secs[1].n));
  while (b.n < off_data)
    sb_printf(&b, "%c", 0);
  sb_append(&b, str_from_len(secs[2].buf, secs[2].n));

  FILE *f = fopen(out_path, "wb");
  if (!f) {
    fprintf(stderr, "rho: cannot write %s\n", out_path);
    return 1;
  }
  fwrite(b.buf, 1, b.n, f);
  fclose(f);
  chmod(out_path, 0755);
  return 0;
}
