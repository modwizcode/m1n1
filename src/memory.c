/* SPDX-License-Identifier: MIT */

#include "memory.h"
#include "assert.h"
#include "cpu_regs.h"
#include "fb.h"
#include "malloc.h"
#include "string.h"
#include "utils.h"

#define PAGE_SIZE       0x4000
#define CACHE_LINE_SIZE 64

#define CACHE_RANGE_OP(func, op)                                                                   \
    void func(void *addr, size_t length)                                                           \
    {                                                                                              \
        u64 p = (u64)addr;                                                                         \
        u64 end = p + length;                                                                      \
        while (p < end) {                                                                          \
            cacheop(op, p);                                                                        \
            p += CACHE_LINE_SIZE;                                                                  \
        }                                                                                          \
    }

CACHE_RANGE_OP(ic_ivau_range, "ic ivau")
CACHE_RANGE_OP(dc_ivac_range, "dc ivac")
CACHE_RANGE_OP(dc_zva_range, "dc zva")
CACHE_RANGE_OP(dc_cvac_range, "dc cvac")
CACHE_RANGE_OP(dc_cvau_range, "dc cvau")
CACHE_RANGE_OP(dc_civac_range, "dc civac")

static inline u64 read_sctlr(void)
{
    sysop("isb");
    return mrs(SCTLR_EL1);
}

static inline void write_sctlr(u64 val)
{
    msr(SCTLR_EL1, val);
    sysop("isb");
}

/*
 * https://armv8-ref.codingbelief.com/en/chapter_d4/d43_2_armv8_translation_table_level_3_descriptor_formats.html
 * PTE_TYPE:PTE_BLOCK indicates that the page table entry (PTE) points to a physical memory block
 * PTE_TYPE:PTE_TABLE indicates that the PTE points to another PTE
 * PTE_TYPE:PTE_PAGE indicates that the PTE points to a single page
 * PTE_FLAG_ACCESS is required to allow access to the memory region
 * PTE_MAIR_IDX sets the MAIR index to be used for this PTE
 */
#define PTE_VALID       BIT(0)
#define PTE_TYPE        BIT(1)
#define PTE_BLOCK       0
#define PTE_TABLE       1
#define PTE_PAGE        1
#define PTE_ACCESS      BIT(10)
#define PTE_MAIR_IDX(i) ((i & 7) << 2)
#define PTE_PXN         BIT(53)
#define PTE_UXN         BIT(54)
#define PTE_AP_RO       BIT(7)
#define PTE_AP_EL0      BIT(6)

#define PERM_RO_EL0  PTE_AP_EL0 | PTE_AP_RO | PTE_PXN | PTE_UXN
#define PERM_RW_EL0  PTE_AP_EL0 | PTE_PXN | PTE_UXN
#define PERM_RX_EL0  PTE_AP_EL0 | PTE_AP_RO
#define PERM_RWX_EL0 PTE_AP_EL0

#define PERM_RO  PTE_AP_RO | PTE_PXN | PTE_UXN
#define PERM_RW  PTE_PXN | PTE_UXN
#define PERM_RX  PTE_AP_RO | PTE_UXN
#define PERM_RWX 0

#define VADDR_L3_INDEX_BITS 11
#define VADDR_L2_INDEX_BITS 11
// We treat two concatenated L1 page tables as one
#define VADDR_L1_INDEX_BITS 12

#define VADDR_L3_OFFSET_BITS 14
#define VADDR_L2_OFFSET_BITS 25
#define VADDR_L1_OFFSET_BITS 36

#define VADDR_L1_ALIGN_MASK GENMASK(VADDR_L1_OFFSET_BITS - 1, VADDR_L2_OFFSET_BITS)
#define VADDR_L2_ALIGN_MASK GENMASK(VADDR_L2_OFFSET_BITS - 1, VADDR_L3_OFFSET_BITS)
#define PTE_TARGET_MASK     GENMASK(49, VADDR_L3_OFFSET_BITS)

#define ENTRIES_PER_L1_TABLE BIT(VADDR_L1_INDEX_BITS)
#define ENTRIES_PER_L2_TABLE BIT(VADDR_L2_INDEX_BITS)
#define ENTRIES_PER_L3_TABLE BIT(VADDR_L3_INDEX_BITS)

#define IS_PTE(pte) ((pte) && pte & PTE_VALID)

#define L1_IS_TABLE(pte) (IS_PTE(pte) && FIELD_GET(PTE_TYPE, pte) == PTE_TABLE)
#define L1_IS_BLOCK(pte) (IS_PTE(pte) && FIELD_GET(PTE_TYPE, pte) == PTE_BLOCK)
#define L2_IS_TABLE(pte) (IS_PTE(pte) && FIELD_GET(PTE_TYPE, pte) == PTE_TABLE)
#define L2_IS_BLOCK(pte) (IS_PTE(pte) && FIELD_GET(PTE_TYPE, pte) == PTE_BLOCK)
#define L3_IS_BLOCK(pte) (IS_PTE(pte) && FIELD_GET(PTE_TYPE, pte) == PTE_PAGE)

/*
 * We use 16KB pages which results in the following virtual address space:
 *
 * [L0 index]  [L1 index]  [L2 index]  [L3 index] [page offset]
 *   1 bit       11 bits     11 bits     11 bits    14 bits
 *
 * To simplify things we treat the L1 page table as a concatenated table,
 * which results in the following layout:
 *
 * [L1 index]  [L2 index]  [L3 index] [page offset]
 *   12 bits     11 bits     11 bits    14 bits
 *
 * We initalize one double-size L1 table which covers the entire virtual memory space,
 * point to the two halves in the single L0 table and then create L2/L3 tables on demand.
 */

/*
 * SPRR mappings interpret these bits as a 4-bit index as follows
 * [AP1][AP0][PXN][UXN]
 */
#define SPRR_INDEX(perm)                                                                           \
    (((PTE_AP_RO & (perm)) ? 0b1000 : 0) | ((PTE_AP_EL0 & (perm)) ? 0b0100 : 0) |                  \
     ((PTE_UXN & (perm)) ? 0b0010 : 0) | ((PTE_PXN & (perm)) ? 0b0001 : 0))

enum SPRR_val_t {
    EL0_GL0,
    ELrx_GL0,
    ELr_GL0,
    ELrw_GL0,
    EL0_GLrx,
    ELrx_GLrx,
    ELr_GLrx,
    EL0_GLrx_ALT,
    EL0_GLr,
    ELx_GLr,
    ELr_GLr,
    ELrw_GLr,
    EL0_GLrw,
    ELrx_GLrw,
    ELr_GLrw,
    ELrw_GLrw,
};

/*
 * With SPRR enabled, RWX mappings get downgraded to RW.
 */

#define SPRR_PERM(ap, val) (((u64)val) << (4 * SPRR_INDEX(ap)))

#define SPRR_DEFAULT_PERM_EL1                                                                      \
    SPRR_PERM(PERM_RO_EL0, ELrw_GLrw) | SPRR_PERM(PERM_RW_EL0, ELrw_GLrw) |                        \
        SPRR_PERM(PERM_RX_EL0, ELrx_GLrx) | SPRR_PERM(PERM_RWX_EL0, ELrw_GLrw) |                   \
        SPRR_PERM(PERM_RO, ELr_GLr) | SPRR_PERM(PERM_RW, ELrw_GLrw) |                              \
        SPRR_PERM(PERM_RX, ELrx_GLrx) | SPRR_PERM(PERM_RWX, ELrw_GLrw)

#define SPRR_DEFAULT_PERM_EL0                                                                      \
    SPRR_PERM(PERM_RO_EL0, ELr_GLr) | SPRR_PERM(PERM_RW_EL0, ELrw_GLrw) |                          \
        SPRR_PERM(PERM_RX_EL0, ELrx_GLrx) | SPRR_PERM(PERM_RWX_EL0, ELrx_GLrx) |                   \
        SPRR_PERM(PERM_RO, ELr_GLr) | SPRR_PERM(PERM_RW, ELrw_GLrw) |                              \
        SPRR_PERM(PERM_RX, ELrx_GLrx) | SPRR_PERM(PERM_RWX, ELrw_GLrw)

/*
 * aarch64 allows to configure attribute sets for up to eight different memory
 * types. we need normal memory and two types of device memory (nGnRnE and
 * nGnRE) in m1n1.
 * The indexes here are selected arbitrarily: A page table entry
 * contains a field to select one of these which will then be used
 * to select the corresponding memory access flags from MAIR.
 */
#define MAIR_IDX_NORMAL        0
#define MAIR_IDX_DEVICE_nGnRnE 1
#define MAIR_IDX_DEVICE_nGnRE  2

#define MAIR_SHIFT_NORMAL        (MAIR_IDX_NORMAL * 8)
#define MAIR_SHIFT_DEVICE_nGnRnE (MAIR_IDX_DEVICE_nGnRnE * 8)
#define MAIR_SHIFT_DEVICE_nGnRE  (MAIR_IDX_DEVICE_nGnRE * 8)

/*
 * https://developer.arm.com/documentation/ddi0500/e/system-control/aarch64-register-descriptions/memory-attribute-indirection-register--el1
 *
 * MAIR_ATTR_NORMAL_DEFAULT sets Normal Memory, Outer Write-back non-transient,
 *                          Inner Write-back non-transient, R=1, W=1
 * MAIR_ATTR_DEVICE_nGnRnE  sets Device-nGnRnE memory
 * MAIR_ATTR_DEVICE_nGnRE   sets Device-nGnRE memory
 */
#define MAIR_ATTR_NORMAL_DEFAULT 0xffUL
#define MAIR_ATTR_DEVICE_nGnRnE  0x00UL
#define MAIR_ATTR_DEVICE_nGnRE   0x04UL

static u64 mmu_pt_L0[2] ALIGNED(PAGE_SIZE);
static u64 mmu_pt_L1[ENTRIES_PER_L1_TABLE] ALIGNED(PAGE_SIZE);

static u64 *mmu_pt_get_l2(u64 from)
{
    u64 l1idx = from >> VADDR_L1_OFFSET_BITS;
    assert(l1idx < ENTRIES_PER_L1_TABLE);
    u64 l1d = mmu_pt_L1[l1idx];

    if (L1_IS_TABLE(l1d))
        return (u64 *)(l1d & PTE_TARGET_MASK);

    u64 *l2 = (u64 *)memalign(PAGE_SIZE, ENTRIES_PER_L2_TABLE * sizeof(u64));
    assert(!IS_PTE(l1d));
    memset64(l2, 0, ENTRIES_PER_L2_TABLE * sizeof(u64));

    l1d = ((u64)l2) | FIELD_PREP(PTE_TYPE, PTE_TABLE) | PTE_VALID;
    mmu_pt_L1[l1idx] = l1d;
    return l2;
}

static void mmu_pt_map_l2(u64 from, u64 to, u64 size)
{
    assert((from & MASK(VADDR_L2_OFFSET_BITS)) == 0);
    assert((to & PTE_TARGET_MASK & MASK(VADDR_L2_OFFSET_BITS)) == 0);
    assert((size & MASK(VADDR_L2_OFFSET_BITS)) == 0);

    to |= FIELD_PREP(PTE_TYPE, PTE_BLOCK);

    for (; size; size -= BIT(VADDR_L2_OFFSET_BITS)) {
        u64 idx = (from >> VADDR_L2_OFFSET_BITS) & MASK(VADDR_L2_INDEX_BITS);
        u64 *l2 = mmu_pt_get_l2(from);

        if (L2_IS_TABLE(l2[idx]))
            free((void *)(l2[idx] & PTE_TARGET_MASK));

        l2[idx] = to;
        from += BIT(VADDR_L2_OFFSET_BITS);
        to += BIT(VADDR_L2_OFFSET_BITS);
    }
}

static u64 *mmu_pt_get_l3(u64 from)
{
    u64 *l2 = mmu_pt_get_l2(from);
    u64 l2idx = (from >> VADDR_L2_OFFSET_BITS) & MASK(VADDR_L2_INDEX_BITS);
    assert(l2idx < ENTRIES_PER_L2_TABLE);
    u64 l2d = l2[l2idx];

    if (L2_IS_TABLE(l2d))
        return (u64 *)(l2d & PTE_TARGET_MASK);

    u64 *l3 = (u64 *)memalign(PAGE_SIZE, ENTRIES_PER_L3_TABLE * sizeof(u64));
    if (IS_PTE(l2d)) {
        u64 l3d = l2d;
        l3d &= ~PTE_TYPE;
        l3d |= FIELD_PREP(PTE_TYPE, PTE_PAGE);
        for (u64 idx = 0; idx < ENTRIES_PER_L3_TABLE; idx++, l3d += BIT(VADDR_L3_OFFSET_BITS))
            l3[idx] = l3d;
    } else {
        memset64(l3, 0, ENTRIES_PER_L3_TABLE * sizeof(u64));
    }

    l2d = ((u64)l3) | FIELD_PREP(PTE_TYPE, PTE_TABLE) | PTE_VALID;
    l2[l2idx] = l2d;
    return l3;
}

static void mmu_pt_map_l3(u64 from, u64 to, u64 size)
{
    assert((from & MASK(VADDR_L3_OFFSET_BITS)) == 0);
    assert((to & PTE_TARGET_MASK & MASK(VADDR_L3_OFFSET_BITS)) == 0);
    assert((size & MASK(VADDR_L3_OFFSET_BITS)) == 0);

    to |= FIELD_PREP(PTE_TYPE, PTE_PAGE);

    for (; size; size -= BIT(VADDR_L3_OFFSET_BITS)) {
        u64 idx = (from >> VADDR_L3_OFFSET_BITS) & MASK(VADDR_L3_INDEX_BITS);
        u64 *l3 = mmu_pt_get_l3(from);

        l3[idx] = to;
        from += BIT(VADDR_L3_OFFSET_BITS);
        to += BIT(VADDR_L3_OFFSET_BITS);
    }
}

int mmu_map(u64 from, u64 to, u64 size)
{
    u64 chunk;
    if (from & MASK(VADDR_L3_OFFSET_BITS) || size & MASK(VADDR_L3_OFFSET_BITS))
        return -1;

    // L3 mappings to boundary
    chunk = min(size, ALIGN_UP(from, MASK(VADDR_L2_OFFSET_BITS)) - from);
    if (chunk) {
        mmu_pt_map_l3(from, to, chunk);
        from += chunk;
        to += chunk;
        size -= chunk;
    }

    // L2 mappings
    chunk = ALIGN_DOWN(size, MASK(VADDR_L2_OFFSET_BITS));
    if (chunk && (to & VADDR_L2_ALIGN_MASK) == 0) {
        mmu_pt_map_l2(from, to, chunk);
        from += chunk;
        to += chunk;
        size -= chunk;
    }

    // L3 mappings to end
    if (size) {
        mmu_pt_map_l3(from, to, size);
    }

    return 0;
}

static u64 mmu_make_table_pte(u64 *addr)
{
    u64 pte = FIELD_PREP(PTE_TYPE, PTE_TABLE) | PTE_VALID;
    pte |= (uintptr_t)addr;
    pte |= PTE_ACCESS;
    return pte;
}

static void mmu_init_pagetables(void)
{
    memset64(mmu_pt_L0, 0, sizeof mmu_pt_L0);
    memset64(mmu_pt_L1, 0, sizeof mmu_pt_L1);

    mmu_pt_L0[0] = mmu_make_table_pte(&mmu_pt_L1[0]);
    mmu_pt_L0[1] = mmu_make_table_pte(&mmu_pt_L1[ENTRIES_PER_L1_TABLE >> 1]);
}

static void mmu_add_mapping(u64 from, u64 to, size_t size, u8 attribute_index, u64 perms)
{
    if (mmu_map(from, to | PTE_MAIR_IDX(attribute_index) | PTE_ACCESS | PTE_VALID | perms, size) <
        0)
        panic("Failed to add MMU mapping 0x%lx -> 0x%lx (0x%lx)\n", from, to, size);
}

static void mmu_add_default_mappings(void)
{
    /*
     * Create MMIO mappings. PCIe has to be mapped as nGnRE while MMIO needs nGnRnE.
     * see https://lore.kernel.org/linux-arm-kernel/c1bc2a087747c4d9@bloch.sibelius.xs4all.nl/
     */
    mmu_add_mapping(0x0200000000, 0x0200000000, 0x0200000000, MAIR_IDX_DEVICE_nGnRnE, PERM_RW_EL0);
    mmu_add_mapping(0x0400000000, 0x0400000000, 0x0100000000, MAIR_IDX_DEVICE_nGnRE, PERM_RW_EL0);
    mmu_add_mapping(0x0500000000, 0x0500000000, 0x0080000000, MAIR_IDX_DEVICE_nGnRnE, PERM_RW_EL0);
    mmu_add_mapping(0x0580000000, 0x0580000000, 0x0100000000, MAIR_IDX_DEVICE_nGnRE, PERM_RW_EL0);
    mmu_add_mapping(0x0680000000, 0x0680000000, 0x0020000000, MAIR_IDX_DEVICE_nGnRnE, PERM_RW_EL0);
    mmu_add_mapping(0x06a0000000, 0x06a0000000, 0x0060000000, MAIR_IDX_DEVICE_nGnRE, PERM_RW_EL0);

    /*
     * Create identity mapping for 16GB RAM from 0x08_0000_0000 to 0x0c_0000_0000
     * With SPRR enabled, this becomes RW.
     */
    mmu_add_mapping(0x0800000000, 0x0800000000, 0x0400000000, MAIR_IDX_NORMAL, PERM_RWX);

    /*
     * Remap m1n1 executable code as RX.
     */
    mmu_add_mapping((u64)_base, (u64)_base, (u64)_rodata_end - (u64)_base, MAIR_IDX_NORMAL,
                    PERM_RX_EL0);

    /*
     * Create mapping for 16GB RAM from 0x88_0000_0000 to 0x8c_0000_0000,
     * read/writable/exec by EL0 (but not executable by EL1)
     * With SPRR enabled, this becomes RX_EL0.
     */
    mmu_add_mapping(0x0800000000 | REGION_RWX_EL0, 0x0800000000, 0x0400000000, MAIR_IDX_NORMAL,
                    PERM_RWX_EL0);
    /*
     * Create mapping for 16GB RAM from 0x98_0000_0000 to 0x9c_0000_0000,
     * read/writable by EL0 (but not executable by EL1)
     * With SPRR enabled, this becomes RW_EL0.
     */
    mmu_add_mapping(0x0800000000 | REGION_RW_EL0, 0x0800000000, 0x0400000000, MAIR_IDX_NORMAL,
                    PERM_RW_EL0);
    /*
     * Create mapping for 16GB RAM from 0xa8_0000_0000 to 0xac_0000_0000,
     * read/executable by EL1
     * This allows executing from dynamic regions in EL1
     */
    mmu_add_mapping(0x0800000000 | REGION_RX_EL1, 0x0800000000, 0x0400000000, MAIR_IDX_NORMAL,
                    PERM_RX_EL0);

    /*
     * Create two seperate nGnRnE and nGnRE full mappings of MMIO space
     */
    mmu_add_mapping(0xe000000000, 0x0000000000, 0x0800000000, MAIR_IDX_DEVICE_nGnRnE, PERM_RW_EL0);
    mmu_add_mapping(0xf000000000, 0x0000000000, 0x0800000000, MAIR_IDX_DEVICE_nGnRE, PERM_RW_EL0);
}

static void mmu_configure(void)
{
    msr(MAIR_EL1, (MAIR_ATTR_NORMAL_DEFAULT << MAIR_SHIFT_NORMAL) |
                      (MAIR_ATTR_DEVICE_nGnRnE << MAIR_SHIFT_DEVICE_nGnRnE) |
                      (MAIR_ATTR_DEVICE_nGnRE << MAIR_SHIFT_DEVICE_nGnRE));
    msr(TCR_EL1, FIELD_PREP(TCR_IPS, TCR_IPS_1TB) | FIELD_PREP(TCR_TG1, TCR_TG1_16K) |
                     FIELD_PREP(TCR_SH1, TCR_SH1_IS) | FIELD_PREP(TCR_ORGN1, TCR_ORGN1_WBWA) |
                     FIELD_PREP(TCR_IRGN1, TCR_IRGN1_WBWA) | FIELD_PREP(TCR_T1SZ, TCR_T1SZ_48BIT) |
                     FIELD_PREP(TCR_TG0, TCR_TG0_16K) | FIELD_PREP(TCR_SH0, TCR_SH0_IS) |
                     FIELD_PREP(TCR_ORGN0, TCR_ORGN0_WBWA) | FIELD_PREP(TCR_IRGN0, TCR_IRGN0_WBWA) |
                     FIELD_PREP(TCR_T0SZ, TCR_T0SZ_48BIT));

    msr(TTBR0_EL1, (uintptr_t)mmu_pt_L0);
    msr(TTBR1_EL1, (uintptr_t)mmu_pt_L0);

    // Armv8-A Address Translation, 100940_0101_en, page 28
    sysop("dsb ishst");
    sysop("tlbi vmalle1is");
    sysop("dsb ish");
    sysop("isb");
}

static void mmu_init_sprr(void)
{
    msr_sync(SYS_IMP_APL_SPRR_CONFIG_EL1, 1);
    msr_sync(SYS_IMP_APL_SPRR_PERM_EL0, SPRR_DEFAULT_PERM_EL0);
    msr_sync(SYS_IMP_APL_SPRR_PERM_EL1, SPRR_DEFAULT_PERM_EL1);
    msr_sync(SYS_IMP_APL_SPRR_CONFIG_EL1, 0);
}

void mmu_init(void)
{
    printf("MMU: Initializing...\n");

    if (read_sctlr() & SCTLR_M) {
        printf("MMU: already intialized.\n");
        return;
    }

    mmu_init_pagetables();
    mmu_add_default_mappings();
    mmu_configure();
    //mmu_init_sprr();

    // Enable EL0 memory access by EL1
    msr(PAN, 0);

    // RES1 bits
    u64 sctlr = SCTLR_LSMAOE | SCTLR_nTLSMD | SCTLR_TSCXT | SCTLR_ITD;
    // Configure translation
    sctlr |= SCTLR_I | SCTLR_C | SCTLR_M | SCTLR_SPAN;

    printf("MMU: SCTLR_EL1: %lx -> %lx\n", mrs(SCTLR_EL1), sctlr);
    write_sctlr(sctlr);
    printf("MMU: running with MMU and caches enabled!\n");
}

void mmu_shutdown(void)
{
    fb_console_reserve_lines(3);
    printf("MMU: shutting down...\n");
    write_sctlr(read_sctlr() & ~(SCTLR_I | SCTLR_C | SCTLR_M));
    printf("MMU: shutdown successful, clearing caches\n");
    dcsw_op_all(DCSW_OP_DCCISW);
}

u64 mmu_disable(void)
{
    u64 sctlr_old = read_sctlr();
    if (!(sctlr_old & SCTLR_M))
        return sctlr_old;

    write_sctlr(sctlr_old & ~(SCTLR_I | SCTLR_C | SCTLR_M));
    dcsw_op_all(DCSW_OP_DCCISW);

    return sctlr_old;
}

void mmu_restore(u64 state)
{
    write_sctlr(state);
}
