#ifndef PGTABLE_H
#define PGTABLE_H

#include <type.h>
#include <sbi.h>
#include <assert.h>
#include <qemu.h>

#define SATP_MODE_SV39 8
#define SATP_MODE_SV48 9

#define SATP_ASID_SHIFT 44lu
#define SATP_MODE_SHIFT 60lu

#define NORMAL_PAGE_SHIFT 12lu
#define NORMAL_PAGE_SIZE (1lu << NORMAL_PAGE_SHIFT)
#define LARGE_PAGE_SHIFT 21lu
#define LARGE_PAGE_SIZE (1lu << LARGE_PAGE_SHIFT)

#define START_ENTRYPOINT 0xffffffff80000000lu
#define KERNEL_ENTRYPOINT 0xffffffff80400000lu
#define KERNEL_END 0xffffffff80600000lu

#ifdef K210
#define BOOT_KERNEL 0x80020000lu
#else
#define BOOT_KERNEL 0x80200000lu
#endif

#define BOOT_KERNEL_END 0x80400000lu
/*
 * Flush entire local TLB.  'sfence.vma' implicitly fences with the instruction
 * cache as well, so a 'fence.i' is not necessary.
 */
static inline void local_flush_tlb_all(void)
{
    __asm__ __volatile__ ("fence\nfence.i\nsfence.vma\nfence\nfence.i" : : : "memory");
}

/* Flush one page from local TLB */
static inline void local_flush_tlb_page(unsigned long addr)
{
    __asm__ __volatile__ ("sfence.vma %0" : : "r" (addr) : "memory");
}

static inline void local_flush_icache_all(void)
{
    __asm__ __volatile__ ("fence.i" ::: "memory");
}

static inline void flush_icache_all(void)
{
    local_flush_icache_all();
    sbi_remote_fence_i(NULL);
}

static inline void flush_tlb_all(void)
{
    local_flush_tlb_all();
    sbi_remote_sfence_vma(NULL, 0, -1);
}
static inline void flush_tlb_page_all(unsigned long addr)
{
    local_flush_tlb_page(addr);
    sbi_remote_sfence_vma(NULL, 0, -1);
}

static inline void set_satp(
    unsigned mode, unsigned asid, unsigned long ppn)
{
    unsigned long __v =
        (unsigned long)(((unsigned long)mode << SATP_MODE_SHIFT) | ((unsigned long)asid << SATP_ASID_SHIFT) | ppn);
    __asm__ __volatile__("csrw satp, %0" : : "rK"(__v) : "memory");
}

// static inline void set_satp(
//     unsigned mode, unsigned asid, unsigned long ppn)
// {
//     unsigned long __v =
//         (unsigned long)(((unsigned long)asid << 38) | ppn);
//     __asm__ __volatile__("fence\nfence.i\nsfence.vm\nfence\nfence.i\ncsrw satp, %0\nfence\nfence.i\nsfence.vm\nfence\nfence.i" : : "rK"(__v) : "memory");
// }

#define PGDIR_PA 0x80300000lu  // use bootblock's page as PGDIR

/*
 * PTE format:
 * | XLEN-1  10 | 9             8 | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0
 *       PFN      reserved for SW   D   A   G   U   X   W   R   V
 */

#define _PAGE_ACCESSED_OFFSET 6

#define _PAGE_PRESENT (1 << 0)
#define _PAGE_READ (1 << 1)     /* Readable */
#define _PAGE_WRITE (1 << 2)    /* Writable */
#define _PAGE_EXEC (1 << 3)     /* Executable */
#define _PAGE_USER (1 << 4)     /* User */
#define _PAGE_GLOBAL (1 << 5)   /* Global */
#define _PAGE_ACCESSED (1 << 6) /* Set by hardware on any access \
                                 */
#define _PAGE_DIRTY (1 << 7)    /* Set by hardware on any write */
#define _PAGE_SWAP (1 << 8)     /* Swapped to SD-card */
#define _PAGE_SHARE (1 << 9)    /* Shared page */

#define _PAGE_PFN_SHIFT 10lu

#define VA_MASK ((1lu << 39) - 1)

#define PPN_BITS 9lu
#define NUM_PTE_ENTRY (1 << PPN_BITS)
#define NUM_USER_PTE_ENTRY 0x100

#define VA_VPN0_SHIFT 12lu
#define VA_VPN1_SHIFT 21lu
#define VA_VPN2_SHIFT 30lu

typedef uint64_t PTE;

static inline uintptr_t kva2pa(uintptr_t kva)
{
    return kva - 0xffffffff00000000;
}

static inline uintptr_t pa2kva(uintptr_t pa)
{
    return pa + 0xffffffff00000000;
}

static inline uint64_t get_pa(PTE entry)
{
    return 0;
}


/* Get/Set page frame number of the `entry` */
static inline uint64_t get_pfn(PTE entry)
{
    return entry >> _PAGE_PFN_SHIFT;
}

static inline void set_pfn(PTE *entry, uint64_t pfn)
{
    *entry = (*entry & ((1<<_PAGE_PFN_SHIFT)-1)) | (pfn << _PAGE_PFN_SHIFT);
}

/* Get/Set attribute(s) of the `entry` */
static inline long get_attribute(PTE entry, uint64_t mask)
{
    return entry & mask;
}
static inline void set_attribute(PTE *entry, uint64_t bits)
{
    *entry = *entry | bits;
}
static inline uintptr_t get_kva_of(uintptr_t va, uintptr_t pgdir_va)
{
    uint64_t vpn2 = (va&VA_MASK) >> VA_VPN2_SHIFT;
    uint64_t vpn1 = ((va&VA_MASK) >> VA_VPN1_SHIFT) & (NUM_PTE_ENTRY - 1);
    uint64_t vpn0 = ((va&VA_MASK) >> VA_VPN0_SHIFT) & (NUM_PTE_ENTRY - 1);
    uint64_t va_offset = (va&VA_MASK) & (NORMAL_PAGE_SIZE - 1);
    PTE *ptr = pgdir_va + vpn2*sizeof(PTE);
    if (!get_attribute(*ptr,_PAGE_PRESENT)){
        assert(0);
    }
    else if (!get_attribute(*ptr,_PAGE_READ)&!get_attribute(*ptr,_PAGE_WRITE)&!get_attribute(*ptr,_PAGE_EXEC))
        ptr = pa2kva(get_pfn(*ptr) << NORMAL_PAGE_SHIFT) + vpn1*sizeof(PTE);
    else
        return pa2kva(get_pfn(*ptr) << NORMAL_PAGE_SHIFT) + (vpn1 << VA_VPN1_SHIFT) + (vpn0 << VA_VPN0_SHIFT)+va_offset;

    if (!get_attribute(*ptr,_PAGE_PRESENT)){
        assert(0);
    }
    else if (!get_attribute(*ptr,_PAGE_READ)&!get_attribute(*ptr,_PAGE_WRITE)&!get_attribute(*ptr,_PAGE_EXEC))
        ptr = pa2kva(get_pfn(*ptr) << NORMAL_PAGE_SHIFT) + vpn0*sizeof(PTE);
    else
        return pa2kva(get_pfn(*ptr) << NORMAL_PAGE_SHIFT) + (vpn0 << VA_VPN0_SHIFT)+va_offset;

    if (!get_attribute(*ptr,_PAGE_PRESENT)){
        assert(0);
    }
    else if (!get_attribute(*ptr,_PAGE_READ)&!get_attribute(*ptr,_PAGE_WRITE)&!get_attribute(*ptr,_PAGE_EXEC)){
        assert(0);
    }
    else
        return pa2kva(get_pfn(*ptr) << NORMAL_PAGE_SHIFT) +va_offset;

}

static inline void clear_attribute(PTE *entry, uint64_t bits)
{
    *entry = *entry & ~bits;
}

static inline void clear_pgdir(uintptr_t pgdir_addr)
{
    PTE *ptr = pgdir_addr;
    for (int i = 0; i < NUM_PTE_ENTRY; ++i)
    {
        *ptr &= 0;
        ptr++;
    }
}

#endif  // PGTABLE_H