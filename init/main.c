/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *            Copyright (C) 2018 Institute of Computing Technology, CAS
 *               Author : Han Shukai (email : hanshukai@ict.ac.cn)
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *         The kernel's entry, where most of the initialization work is done.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * */

#include <common.h>
#include <os/irq.h>
#include <os/mm.h>
#include <os/sched.h>
#include <screen.h>
#include <sbi.h>
#include <stdio.h>
#include <os/time.h>
#include <os/syscall.h>
#include <os/futex.h>
#include <test.h>
#include <pgtable.h>
#include <os/elf.h>
#include <sdcard.h>
#include <fpioa.h>
#include <memlayout.h>
#include <user_programs.h>

#include <csr.h>
#include <asm.h>

extern void ret_from_exception();

static void init_pcb()
{
    current_running = &pid0_pcb;
    /* init shell */
    pcb_t *pcb_underinit = &pcb[0];
    ptr_t kernel_stack = allocPage() + NORMAL_PAGE_SIZE;
    ptr_t user_stack = USER_STACK_ADDR;

    pcb_underinit->preempt_count = 0;
    pcb_underinit->list.ptr = pcb_underinit;
    pcb_underinit->pid = process_id++;
    pcb_underinit->type = KERNEL_PROCESS;
    pcb_underinit->wait_list.next = &pcb_underinit->wait_list;pcb_underinit->wait_list.prev = &pcb_underinit->wait_list;
    pcb_underinit->status = TASK_READY;
    pcb_underinit->priority = 1;
    pcb_underinit->temp_priority = pcb_underinit->priority;
    pcb_underinit->cursor_x = 1; pcb_underinit->cursor_y = 1; 
    pcb_underinit->mask = 0xf;
    
    unsigned char *_elf_shell;
    int length;
    get_elf_file("shell",&_elf_shell,&length);
    uintptr_t pgdir = allocPage();
    clear_pgdir(pgdir);
    alloc_page_helper(user_stack - NORMAL_PAGE_SIZE,pgdir,_PAGE_ACCESSED|_PAGE_DIRTY|_PAGE_READ|_PAGE_WRITE|_PAGE_USER);
    uintptr_t test_shell = (uintptr_t)load_elf(_elf_shell,length,pgdir,alloc_page_helper);
    share_pgtable(pgdir,pa2kva(PGDIR_PA));

    init_pcb_stack(pgdir, kernel_stack, user_stack, test_shell, 0, NULL, pcb_underinit);
    list_add_tail(&pcb_underinit->list,&ready_queue);

    /* init pcb */
    for (int i = 1; i < NUM_MAX_TASK; ++i)
    {
        pcb[i].list.ptr = &pcb[i];
        pcb[i].pid = 0;
        pcb[i].wait_list.next = &pcb[i].wait_list;pcb[i].wait_list.prev = &pcb[i].wait_list;
        pcb[i].status = TASK_EXITED;
        list_add_tail(&pcb[i].list,&available_queue);
    }

    /* init pid0 */
    pid0_pcb.list.next = &pid0_pcb.list; pid0_pcb.list.prev = &pid0_pcb.list;pid0_pcb.list.ptr = &pid0_pcb;
    pid0_pcb.wait_list.next = &pid0_pcb.wait_list; pid0_pcb.wait_list.prev = &pid0_pcb.wait_list;
    pid0_pcb2.list.next = &pid0_pcb2.list; pid0_pcb2.list.prev = &pid0_pcb2.list;pid0_pcb2.list.ptr = &pid0_pcb2;
    pid0_pcb2.wait_list.next = &pid0_pcb2.wait_list; pid0_pcb2.wait_list.prev = &pid0_pcb2.wait_list;
}

static void init_syscall(void)
{
    // initialize system call table.
    syscall[SYSCALL_SLEEP] = &do_sleep;

    syscall[SYSCALL_FUTEX_WAIT] = &futex_wait;
    syscall[SYSCALL_FUTEX_WAKEUP] = &futex_wakeup;

    syscall[SYSCALL_WRITE] = &screen_write;
    // syscall[SYSCALL_READ] = ???;
    syscall[SYSCALL_CURSOR] = &screen_move_cursor;
    syscall[SYSCALL_REFLUSH] = &screen_reflush;

    syscall[SYSCALL_GET_TIMEBASE] = &sbi_read_fdt;
    syscall[SYSCALL_GET_TICK] = &get_ticks;
    syscall[SYSCALL_EXIT] = &do_exit;
}

// stop mapping boot_kernel
static void init_kpgtable()
{
    for (uint64_t i = BOOT_KERNEL; i < BOOT_KERNEL_END; i += LARGE_PAGE_SIZE)
    {
        uint64_t va = i, pgdir = pa2kva(PGDIR_PA);
        uint64_t vpn2 = (va&VA_MASK) >> VA_VPN2_SHIFT;
        uint64_t vpn1 = ((va&VA_MASK) >> VA_VPN1_SHIFT) & (NUM_PTE_ENTRY - 1);
        PTE *ptr1 = pgdir + vpn2*sizeof(PTE);
        PTE *ptr2 = pa2kva(get_pfn(*ptr1) << NORMAL_PAGE_SHIFT);
        clear_pgdir(ptr2);
        *ptr1 = 0lu;
    }
}

// The beginning of everything >_< ~~~~~~~~~~~~~~
int main()
{
    // init_kpgtable();
    // // init Process Control Block (-_-!)
    init_pcb();
    printk("> [INIT] PCB initialization succeeded.\n\r");
    // // read CPU frequency
    time_base = sbi_read_fdt(TIMEBASE);
    // init interrupt (^_^)
    init_exception();
    printk("> [INIT] Interrupt processing initialization succeeded.\n\r");
    // init system call table (0_0)
    init_syscall();
    printk("> [INIT] System call initialized successfully.\n\r");

    // init sdcard
    fpioa_pin_init();
    printk("> [INIT] FPIOA initialized successfully.\n\r");
    // printk("fpioa is at %lx\n\r", get_kva_of(FPIOA_BASE_ADDR, pa2kva(PGDIR_PA)));

#ifdef K210
    ioremap(UARTHS, UARTHS, NORMAL_PAGE_SIZE);
#elif
    ioremap(UART0, UART0, NORMAL_PAGE_SIZE);
#endif

#ifndef K210
    // virtio mmio disk interface
    ioremap(VIRTIO0, VIRTIO0, NORMAL_PAGE_SIZE);
#endif

    ioremap(CLINT, CLINT, 0x10000);

    ioremap(PLIC, PLIC, 0x400000);

#ifdef K210
    ioremap(GPIOHS, GPIOHS, 0x1000);
    ioremap(GPIO, GPIO, 0x1000);
    ioremap(SPI_SLAVE, SPI_SLAVE, 0x1000);
    ioremap(GPIOHS, GPIOHS, 0x1000);
    ioremap(SPI0, SPI0, 0x1000);
    ioremap(SPI1, SPI1, 0x1000);
    ioremap(SPI2, SPI2, 0x1000);
#endif

    printk("> [INIT] IOREMAP initialized successfully.\n\r");

#ifdef K210
    sdcard_init();
    printk("> [INIT] SD card initialized successfully.\n\r");
    test_sdcard();
    print_cardInfo();
#endif
    // init screen (QAQ)
    init_screen();
    printk("> [INIT] SCREEN initialization succeeded.\n\r");
    // Setup timer interrupt and enable all interrupt
    sbi_set_timer(get_ticks() + (time_base / PREEMPT_FREQUENCY));
    // printk("ticks: %d\n", get_ticks());
    // printk("timebase: %d\n", time_base);
    /* setup exception */
    clear_interrupt();
    setup_exception();
    enable_interrupt();

    while (1) {
        // (QAQQQQQQQQQQQ)
        // If you do non-preemptive scheduling, you need to use it
        // to surrender control do_scheduler();
        __asm__ __volatile__("wfi\n\r":::);
        ;
    };
    return 0;
}

void test_sdcard() {
    uint8 *buffer = kalloc();
    uint8 *pre_buffer = kalloc();
    memset(buffer, 0, sizeof(buffer));
    if(sd_read_sector(pre_buffer, 0, sizeof(pre_buffer))) {
        printk("[test_sdcard]SD card read sector err\nr");
    } else {
        printk("[test_sdcard]SD card read sector succeed\nr");
    }
    printk("[test_sdcard]Buffer: %s\n", buffer);
    memmove(buffer, "Hello,sdcard", sizeof("Hello,sdcard"));
    printk("[test_sdcard]Buffer: %s\n", buffer);
    if(sd_write_sector(buffer, 0, sizeof(buffer))) {
        printk("[test_sdcard]SD card write sector err\n\r");
    } else {
        printk("[test_sdcard]SD card write sector succeed\n\r");
    }
    memset(buffer, 0, sizeof(buffer));
    if(sd_read_sector(buffer, 0, sizeof(buffer))) {
        printk("[test_sdcard]SD card read sector err\n\r");
    } else {
        printk("[test_sdcard]SD card read sector succeed\n\r");
    }
    printk("[test_sdcard]Buffer: %s\n", buffer);
    if(sd_write_sector(pre_buffer, 0, sizeof(pre_buffer))) {
        printk("[test_sdcard]SD card recover err\n\r");
    } else {
        printk("[test_sdcard]SD card recover succeed\n");
    }
    kfree(buffer);
    kfree(pre_buffer);
}

void print_cardInfo()
{
    printk("BlockSize: %d\n\r", cardinfo.CardBlockSize);
    printk("Capacity: %d\n\r", cardinfo.CardCapacity);
    printk("MinRd: %d\n\r", cardinfo.SD_csd.MaxRdCurrentVDDMin);   /*!< Max. read current @ VDD min */
    printk("MaxRd: %d\n\r", cardinfo.SD_csd.MaxRdCurrentVDDMax);   /*!< Max. read current @ VDD max */
    printk("MinWr: %d\n\r", cardinfo.SD_csd.MaxWrCurrentVDDMin);   /*!< Max. write current @ VDD min */
    printk("MaxWr: %d\n\r", cardinfo.SD_csd.MaxWrCurrentVDDMax);   /*!< Max. write current @ VDD max */
    printk("MaxWr: %d\n\r", cardinfo.SD_csd.MaxWrBlockLen);
    printk("Fileformat: %d\n\r", cardinfo.SD_csd.FileFormat);
}