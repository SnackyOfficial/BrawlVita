#include "elf_loader.h"

#include <stdlib.h>
#include <string.h>

#define PAGE 0x1000u
#define ALIGN_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))
#define ALIGN_DOWN(x) ((x) & ~(PAGE - 1))

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_ARM_EXIDX 0x70000001u
#define PF_X 1u

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_INIT 12
#define DT_SONAME 14
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28

#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23

#define LOGF(ops, ...) do { if ((ops)->log) (ops)->log((ops)->ctx, __VA_ARGS__); } while (0)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

typedef struct {
    uint32_t type, off, vaddr, paddr, filesz, memsz, flags, align;
} phdr_t;

/* Thunk pool: 16 bytes per unresolved import, see elf_loader.h. */
typedef struct {
    uint8_t *host;
    uint32_t addr;
    uint32_t used, cap;
} thunk_pool;

static void write_img(so_module *m, so_ops *ops, uint32_t off, const void *src, size_t n) {
    if (off >= m->data_off) {
        memcpy(m->host + off, src, n);
        return;
    }
    /* text goes through a privileged copy; keep each call short */
    const uint8_t *s = (const uint8_t *)src;
    for (size_t done = 0; done < n;) {
        size_t c = n - done < 65536 ? n - done : 65536;
        ops->write_text(ops->ctx, m->host + off + done, s + done, c);
        done += c;
    }
}

static void write_img32(so_module *m, so_ops *ops, uint32_t off, uint32_t v) {
    write_img(m, ops, off, &v, 4);
}

static uint32_t make_thunk(so_ops *ops, thunk_pool *tp, uint32_t id) {
    if (tp->used == tp->cap) {
        tp->cap = 64;
        tp->used = 0;
        tp->host = ops->alloc_code(ops->ctx, tp->cap * 16, &tp->addr);
        if (!tp->host) {
            tp->cap = 0;
            return 0;
        }
    }
    uint32_t w[4] = {0xe59fc000u, 0xe59ff000u, id, ops->unresolved_handler};
    uint32_t off = tp->used * 16;
    ops->write_text(ops->ctx, tp->host + off, w, 16);
    ops->flush(ops->ctx, tp->host + off, 16);
    tp->used++;
    return tp->addr + off;
}

int so_load(so_module *m, const char *name, const uint8_t *file, size_t size, so_ops *ops,
            so_unresolved_tab *unres, uint32_t module_id) {
    memset(m, 0, sizeof(*m));
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->id = module_id;

    if (size < 52 || memcmp(file, "\x7f" "ELF", 4) != 0 || file[4] != 1 || file[5] != 1) {
        LOGF(ops, "%s: not a little-endian ELF32 file", name);
        return -1;
    }
    uint16_t etype = rd16(file + 16), machine = rd16(file + 18);
    if (etype != 3 || machine != 40) {
        LOGF(ops, "%s: need ET_DYN/EM_ARM, got type %u machine %u", name, etype, machine);
        return -2;
    }
    uint32_t phoff = rd32(file + 28);
    uint16_t phentsize = rd16(file + 42), phnum = rd16(file + 44);
    if (phentsize < 32 || phoff + (uint32_t)phnum * phentsize > size) {
        LOGF(ops, "%s: bad program header table", name);
        return -3;
    }

    phdr_t ph[32];
    if (phnum > 32) {
        LOGF(ops, "%s: too many program headers (%u)", name, phnum);
        return -3;
    }
    uint32_t text_end = 0, data_start = 0xffffffffu, span_end = 0;
    uint32_t dyn_vaddr = 0, dyn_size = 0;
    for (int i = 0; i < phnum; i++) {
        const uint8_t *p = file + phoff + (size_t)i * phentsize;
        ph[i].type = rd32(p); ph[i].off = rd32(p + 4); ph[i].vaddr = rd32(p + 8); ph[i].paddr = rd32(p + 12);
        ph[i].filesz = rd32(p + 16); ph[i].memsz = rd32(p + 20); ph[i].flags = rd32(p + 24); ph[i].align = rd32(p + 28);
        if (ph[i].type == PT_LOAD) {
            if ((size_t)ph[i].off + ph[i].filesz > size) {
                LOGF(ops, "%s: segment %d runs past the end of the file", name, i);
                return -3;
            }
            uint32_t end = ph[i].vaddr + ph[i].memsz;
            if (end > span_end) span_end = end;
            if (ph[i].flags & PF_X) {
                if (end > text_end) text_end = end;
            } else if (ph[i].vaddr < data_start) {
                data_start = ph[i].vaddr;
            }
        } else if (ph[i].type == PT_DYNAMIC) {
            dyn_vaddr = ph[i].vaddr;
            dyn_size = ph[i].memsz;
        } else if (ph[i].type == PT_ARM_EXIDX) {
            m->exidx = ph[i].vaddr;
            m->exidx_size = ph[i].memsz;
        }
    }
    if (!text_end || data_start == 0xffffffffu || !dyn_vaddr) {
        LOGF(ops, "%s: unsupported segment layout (text_end %x data_start %x dyn %x)", name, text_end, data_start, dyn_vaddr);
        return -4;
    }
    m->text_size = ALIGN_UP(text_end);
    m->data_off = ALIGN_DOWN(data_start);
    m->span = ALIGN_UP(span_end);
    if (m->data_off < m->text_size) {
        LOGF(ops, "%s: text (0x%x) and data (0x%x) share a page", name, m->text_size, m->data_off);
        return -4;
    }

    m->host = ops->map(ops->ctx, m->text_size, m->data_off, m->span, &m->load);
    if (!m->host) {
        LOGF(ops, "%s: could not reserve 0x%x bytes", name, m->span);
        return -5;
    }
    LOGF(ops, "%s: mapped span 0x%x, text 0x0-0x%x, data 0x%x-0x%x at run-time base 0x%08x", name, m->span,
         m->text_size, m->data_off, m->span, m->load);

    /* copy segments; bss must be zeroed explicitly (blocks are not guaranteed to be clean) */
    for (int i = 0; i < phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        write_img(m, ops, ph[i].vaddr, file + ph[i].off, ph[i].filesz);
        if (ph[i].memsz > ph[i].filesz) {
            uint32_t n = ph[i].memsz - ph[i].filesz;
            uint8_t zeros[256];
            memset(zeros, 0, sizeof zeros);
            for (uint32_t o = 0; o < n; o += sizeof zeros) {
                uint32_t c = n - o < sizeof zeros ? n - o : (uint32_t)sizeof zeros;
                write_img(m, ops, ph[i].vaddr + ph[i].filesz + o, zeros, c);
            }
        }
    }

    /* dynamic section */
    uint32_t t_rel = 0, t_relsz = 0, t_jmprel = 0, t_pltrelsz = 0, t_symtab = 0, t_strtab = 0, t_hash = 0;
    uint32_t t_needed[SO_MAX_NEEDED], n_needed = 0;
    for (uint32_t o = 0; o + 8 <= dyn_size; o += 8) {
        int32_t tag = (int32_t)rd32(m->host + dyn_vaddr + o);
        uint32_t val = rd32(m->host + dyn_vaddr + o + 4);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_NEEDED: if (n_needed < SO_MAX_NEEDED) t_needed[n_needed++] = val; break;
        case DT_PLTRELSZ: t_pltrelsz = val; break;
        case DT_HASH: t_hash = val; break;
        case DT_STRTAB: t_strtab = val; break;
        case DT_SYMTAB: t_symtab = val; break;
        case DT_RELA:
            LOGF(ops, "%s: DT_RELA is not supported", name);
            return -6;
        case DT_INIT: m->init_func = val; break;
        case DT_REL: t_rel = val; break;
        case DT_RELSZ: t_relsz = val; break;
        case DT_JMPREL: t_jmprel = val; break;
        case DT_INIT_ARRAY: m->init_array = val; break;
        case DT_INIT_ARRAYSZ: m->init_array_n = val / 4; break;
        case DT_FINI_ARRAY: m->fini_array = val; break;
        case DT_FINI_ARRAYSZ: m->fini_array_n = val / 4; break;
        default: break;
        }
    }
    if (!t_symtab || !t_strtab || !t_hash) {
        LOGF(ops, "%s: missing DT_SYMTAB/STRTAB/HASH", name);
        return -6;
    }
    m->symtab = m->host + t_symtab;
    m->strtab = (const char *)(m->host + t_strtab);
    m->hash = (const uint32_t *)(m->host + t_hash);
    m->nsyms = rd32(m->host + t_hash + 4);
    for (uint32_t i = 0; i < n_needed; i++) {
        strncpy(m->needed[i], m->strtab + t_needed[i], 31);
        m->needed[i][31] = 0;
    }
    m->n_needed = (int)n_needed;

    /* symbol resolution cache */
    uint32_t *symaddr = (uint32_t *)calloc(m->nsyms + 1, sizeof(uint32_t));
    uint8_t *symdone = (uint8_t *)calloc(m->nsyms + 1, 1);
    if (!symaddr || !symdone) {
        free(symaddr);
        free(symdone);
        return -7;
    }
    thunk_pool tp;
    memset(&tp, 0, sizeof tp);

    const uint32_t tabs[2][2] = {{t_rel, t_relsz}, {t_jmprel, t_pltrelsz}};
    for (int t = 0; t < 2; t++) {
        uint32_t base = tabs[t][0], sz = tabs[t][1];
        if (!base) continue;
        for (uint32_t o = 0; o + 8 <= sz; o += 8) {
            uint32_t r_off = rd32(m->host + base + o);
            uint32_t r_info = rd32(m->host + base + o + 4);
            uint32_t type = r_info & 0xff, si = r_info >> 8;
            if (r_off + 4 > m->span) {
                LOGF(ops, "%s: relocation outside the image (0x%x)", name, r_off);
                free(symaddr); free(symdone);
                return -8;
            }
            uint32_t S = 0;
            if (type != R_ARM_RELATIVE && si != 0) {
                if (si > m->nsyms) {
                    LOGF(ops, "%s: bad symbol index %u", name, si);
                    free(symaddr); free(symdone);
                    return -8;
                }
                if (!symdone[si]) {
                    const uint8_t *s = m->symtab + (size_t)si * 16;
                    uint32_t n_off = rd32(s), value = rd32(s + 4);
                    uint8_t info = s[12];
                    uint16_t shndx = rd16(s + 14);
                    const char *sname = m->strtab + n_off;
                    if (shndx != 0) {
                        symaddr[si] = m->load + value;
                    } else {
                        uint32_t a = ops->resolve ? ops->resolve(ops->ctx, sname) : 0;
                        if (!a && (info >> 4) != 2) {
                            /* strong undefined and unknown: route to a logging thunk */
                            if (unres && unres->n < SO_MAX_UNRESOLVED) {
                                uint32_t id = unres->n++;
                                strncpy(unres->name[id], sname, 63);
                                unres->name[id][63] = 0;
                                unres->module[id] = module_id;
                                a = make_thunk(ops, &tp, id);
                            }
                            m->n_unresolved++;
                        }
                        symaddr[si] = a;
                    }
                    symdone[si] = 1;
                }
                S = symaddr[si];
            }
            uint32_t cur = rd32(m->host + r_off);
            switch (type) {
            case R_ARM_RELATIVE:
                write_img32(m, ops, r_off, cur + m->load);
                m->n_reloc[0]++;
                break;
            case R_ARM_ABS32:
                write_img32(m, ops, r_off, S + cur);
                m->n_reloc[1]++;
                break;
            case R_ARM_GLOB_DAT:
                write_img32(m, ops, r_off, S);
                m->n_reloc[2]++;
                break;
            case R_ARM_JUMP_SLOT:
                write_img32(m, ops, r_off, S);
                m->n_reloc[3]++;
                break;
            default:
                LOGF(ops, "%s: unsupported relocation type %u at 0x%x", name, type, r_off);
                free(symaddr); free(symdone);
                return -9;
            }
        }
    }
    free(symaddr);
    free(symdone);
    ops->flush(ops->ctx, m->host, m->text_size);
    LOGF(ops, "%s: relocated (RELATIVE %u, ABS32 %u, GLOB_DAT %u, JUMP_SLOT %u), %u imports unresolved",
         name, m->n_reloc[0], m->n_reloc[1], m->n_reloc[2], m->n_reloc[3], m->n_unresolved);
    return 0;
}

static uint32_t elf_hash(const char *s) {
    uint32_t h = 0, g;
    while (*s) {
        h = (h << 4) + (uint8_t)*s++;
        g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

uint32_t so_find_symbol(const so_module *m, const char *name) {
    if (!m->hash) return 0;
    uint32_t nbucket = rd32((const uint8_t *)m->hash);
    if (!nbucket) return 0;
    const uint8_t *buckets = (const uint8_t *)m->hash + 8;
    const uint8_t *chains = buckets + 4 * nbucket;
    uint32_t i = rd32(buckets + 4 * (elf_hash(name) % nbucket));
    while (i) {
        const uint8_t *s = m->symtab + (size_t)i * 16;
        if (rd16(s + 14) != 0 && strcmp(m->strtab + rd32(s), name) == 0) return m->load + rd32(s + 4);
        i = rd32(chains + 4 * i);
    }
    return 0;
}

int so_export(const so_module *m, int idx, const char **name, uint32_t *addr) {
    int n = 0;
    for (uint32_t i = 1; i < m->nsyms; i++) {
        const uint8_t *s = m->symtab + (size_t)i * 16;
        if (rd16(s + 14) == 0 || rd32(s) == 0) continue;
        if (n++ == idx) {
            *name = m->strtab + rd32(s);
            *addr = m->load + rd32(s + 4);
            return 1;
        }
    }
    return 0;
}

int so_ctor(const so_module *m, int idx, uint32_t *addr) {
    int n = (m->init_func ? 1 : 0) + (int)m->init_array_n;
    if (idx >= n) return 0;
    uint32_t v;
    if (m->init_func && idx == 0)
        v = m->init_func + m->load;
    else
        v = rd32(m->host + m->init_array + 4u * (uint32_t)(idx - (m->init_func ? 1 : 0)));
    *addr = v;
    return (v == 0 || v == 0xffffffffu) ? 2 : 1;
}

int so_contains(const so_module *m, uint32_t pc) {
    return pc >= m->load && pc < m->load + m->text_size;
}
