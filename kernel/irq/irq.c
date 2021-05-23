#include <os/irq.h>
#include <os/time.h>
#include <os/sched.h>
#include <os/string.h>
#include <stdio.h>
#include <assert.h>
#include <sbi.h>
#include <screen.h>
#include <pgtable.h>

#define SXLEN 64

int PREEMPT_FREQUENCY = 20000;

handler_t irq_table[IRQC_COUNT];
handler_t exc_table[EXCC_COUNT];
uintptr_t riscv_dtb;

void reset_irq_timer()
{
    // TODO clock interrupt handler.
    // TODO: call following functions when task4
    screen_reflush();
    timer_check();

    // note: use sbi_set_timer
    // remember to reschedule

    sbi_set_timer(get_ticks() + (time_base / PREEMPT_FREQUENCY));
    do_scheduler();
}

void interrupt_helper(regs_context_t *regs, uint64_t stval, uint64_t cause, uint64_t tp)
{
    // call corresponding handler by the value of `cause`
    int is_interrupt = (cause >> (SXLEN - 1));
    is_interrupt = !(is_interrupt == 0);
    int exception_code = (cause << 1) >> 1;
    if (is_interrupt)
        (*irq_table[exception_code])(regs,stval,exception_code);
    else
        (*exc_table[exception_code])(regs,stval,exception_code);
}

void handle_int(regs_context_t *regs, uint64_t interrupt, uint64_t cause)
{
    reset_irq_timer();
}

void init_exception()
{
    /* TODO: initialize irq_table and exc_table */
    /* note: handle_int, handle_syscall, handle_other, etc.*/
    irq_table[IRQC_U_SOFT] = &handle_software; 
    irq_table[IRQC_S_SOFT] = &handle_software;
    irq_table[IRQC_M_SOFT] = &handle_software;  
    irq_table[IRQC_U_TIMER] = &handle_other;
    irq_table[IRQC_S_TIMER] = &handle_int;
    irq_table[IRQC_M_TIMER] = &handle_other; 
    irq_table[IRQC_U_EXT] = &handle_other;   
    irq_table[IRQC_S_EXT] = &handle_other;   
    irq_table[IRQC_M_EXT] = &handle_other;

    exc_table[EXCC_SYSCALL] = &handle_syscall;
    exc_table[EXCC_INST_MISALIGNED] = &handle_other; 
    exc_table[EXCC_INST_ACCESS] = &handle_other;
    exc_table[EXCC_BREAKPOINT] = &handle_other;  
    exc_table[EXCC_LOAD_ACCESS] = &handle_other;
    exc_table[EXCC_STORE_ACCESS] = &handle_other; 
    exc_table[EXCC_INST_PAGE_FAULT] = &handle_pgfault; 
    exc_table[EXCC_LOAD_PAGE_FAULT] = &handle_pgfault;   
    exc_table[EXCC_STORE_PAGE_FAULT] = &handle_pgfault;
}

void handle_pgfault(regs_context_t *regs, uint64_t stval, uint64_t cause)
{
    if (stval >= 0xffffffff00000000lu)
        handle_other(regs,stval,cause);
    uint64_t satp = read_satp();
    uint64_t va = stval;
    uint64_t pgdir = (satp&0xffffffffffflu) << NORMAL_PAGE_SHIFT;

    uint64_t vpn2 = (va&VA_MASK) >> VA_VPN2_SHIFT;
    uint64_t vpn1 = ((va&VA_MASK) >> VA_VPN1_SHIFT) & (NUM_PTE_ENTRY - 1);
    uint64_t vpn0 = ((va&VA_MASK) >> VA_VPN0_SHIFT) & (NUM_PTE_ENTRY - 1);
    PTE *ptr = pa2kva(pgdir) + vpn2*sizeof(PTE);
    // 2
    if (!get_attribute(*ptr,_PAGE_PRESENT))
    {
        uintptr_t pgdir2 = allocPage();
        clear_pgdir(pgdir2);
        uint64_t pfn2 = (kva2pa(pgdir2)&VA_MASK) >> NORMAL_PAGE_SHIFT;        
        set_pfn(ptr,pfn2);
        set_attribute(ptr,_PAGE_PRESENT|_PAGE_USER);
        ptr = pgdir2 + vpn1*sizeof(PTE);
    }
    else
        ptr = pa2kva(get_pfn(*ptr) << NORMAL_PAGE_SHIFT) + vpn1*sizeof(PTE);
    // 1
    if (!get_attribute(*ptr,_PAGE_PRESENT))
    {
        uintptr_t pgdir2 = allocPage();
        clear_pgdir(pgdir2);
        uint64_t pfn2 = (kva2pa(pgdir2)&VA_MASK) >> NORMAL_PAGE_SHIFT;        
        set_pfn(ptr,pfn2);
        set_attribute(ptr,_PAGE_PRESENT|_PAGE_USER);
        ptr = pgdir2 + vpn0*sizeof(PTE);
    }
    else
        ptr = pa2kva(get_pfn(*ptr) << NORMAL_PAGE_SHIFT) + vpn0*sizeof(PTE);

    // 0
    if (!get_attribute(*ptr,_PAGE_PRESENT))
    {
        if (!get_attribute(*ptr,_PAGE_SWAP)){
            uintptr_t pgdir2 = allocPage();
            // clear_pgdir(pgdir2);
            uint64_t pfn2 = (kva2pa(pgdir2)&VA_MASK) >> NORMAL_PAGE_SHIFT;        
            set_pfn(ptr,pfn2);
        }
        else{
            uint64_t prev_pageva = pa2kva(get_pfn(*ptr)<<NORMAL_PAGE_SHIFT);
            for (list_node_t *i = swapPageList.next; i != &swapPageList; i=i->next){
                swappage_node_t *temp = i->ptr;                
                if (temp->page_basekva == prev_pageva ){
                    uint64_t page_basekva = allocPage();
                    clear_pgdir(page_basekva);
                    sbi_sd_read(kva2pa(page_basekva),BLOCKS_PER_PAGE,temp->block_id);
                    set_pfn(ptr,(kva2pa(page_basekva)&VA_MASK)>>NORMAL_PAGE_SHIFT);
                    clear_attribute(ptr,_PAGE_SWAP);
                    list_del(&temp->list);list_add_tail(&temp->list,&availableSwapSpace);
                    break;
                }
            }
        }
        set_attribute(ptr,_PAGE_PRESENT|_PAGE_USER);
        switch(cause){
            case 12: set_attribute(ptr,_PAGE_EXEC);break;
            case 13: set_attribute(ptr,_PAGE_READ|_PAGE_WRITE);break;
            case 15: set_attribute(ptr,_PAGE_READ|_PAGE_WRITE);break;
            default: printk("page fault handler error!\n");break;
        }
    }
    else if (cause == 15 && !get_attribute(*ptr,_PAGE_WRITE))
    {
        printk("Segmentation fault\n");        assert(0);
    }
    else if (cause == 13 && !get_attribute(*ptr,_PAGE_READ)) // read/write on inst
    {
        printk("Segmentation fault\n");        assert(0);
    }
    else if (cause == 12 && !get_attribute(*ptr,_PAGE_EXEC)) // inst on read/write
    {
        printk("Segmentation fault\n");        assert(0);
    }
    else
    {
        if (cause == 12 || cause == 13)
            set_attribute(ptr,_PAGE_ACCESSED);
        else if (cause == 15)
            set_attribute(ptr,_PAGE_ACCESSED|_PAGE_DIRTY);
    }
}

void handle_software()
{
    printk("666\n");
    while(1);
}

void handle_other(regs_context_t *regs, uint64_t stval, uint64_t cause)
{
    char* reg_name[] = {
        "zero "," ra  "," sp  "," gp  "," tp  ",
        " t0  "," t1  "," t2  ","s0/fp"," s1  ",
        " a0  "," a1  "," a2  "," a3  "," a4  ",
        " a5  "," a6  "," a7  "," s2  "," s3  ",
        " s4  "," s5  "," s6  "," s7  "," s8  ",
        " s9  "," s10 "," s11 "," t3  "," t4  ",
        " t5  "," t6  "
    };
    for (int i = 0; i < 32; i += 3) {
        for (int j = 0; j < 3 && i + j < 32; ++j) {
            printk("%s : %016lx ",reg_name[i+j], regs->regs[i+j]);
        }
        printk("\n\r");
    }
    printk("sstatus: 0x%lx sbadaddr: 0x%lx scause: %lu\n\r",
           regs->sstatus, regs->sbadaddr, regs->scause);
    printk("sepc: 0x%lx\n\r", regs->sepc);
    assert(0);
}