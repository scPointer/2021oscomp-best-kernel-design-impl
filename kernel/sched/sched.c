#include <os/list.h>
#include <os/mm.h>
#include <os/lock.h>
#include <os/sched.h>
#include <os/time.h>
#include <os/irq.h>
#include <screen.h>
#include <stdio.h>
#include <assert.h>
#include <sbi.h>
#include <pgtable.h>
#include <csr.h>
#include <qemu.h>
#include <os/elf.h>
#include <os/fat32.h>
#include <os/pipe.h>

#define TOO_LARGE_PRIORITY 10000000
#define true 1
#define false 0

pcb_t pcb[NUM_MAX_TASK];
const ptr_t pid0_stack = INIT_KERNEL_STACK + 2*PAGE_SIZE;
const ptr_t pid0_stack2 = INIT_KERNEL_STACK + 4*PAGE_SIZE;
pcb_t pid0_pcb = {
    .pid = 0,
    .kernel_sp = (ptr_t)(pid0_stack - PAGE_SIZE),
    .user_sp = (ptr_t)(pid0_stack),
    .status = TASK_RUNNING,
    .preempt_count = 0,
    .priority = -1,
    .cursor_x = 1,
    .cursor_y = 1
};

// pcb_t pid0_pcbs[NR_CPUS];
pcb_t pid0_pcb2 = {
    .pid = 0, //modified
    .kernel_sp = (ptr_t)(pid0_stack2 - PAGE_SIZE),
    .user_sp = (ptr_t)(pid0_stack2),
    .status = TASK_RUNNING,
    .preempt_count = 0,
    .priority = -1,
    .cursor_x = 1,
    .cursor_y = 1
};

/* all kinds of queues */
LIST_HEAD(ready_queue);
LIST_HEAD(general_block_queue);
LIST_HEAD(available_queue);
LIST_HEAD(fileop_queue);

/* current running task PCB */
pcb_t * volatile current_running;

/* global process id */
pid_t process_id = 1;

/* do scheduler time counter */
int FORMER_TICKS_COUNTER;
int LATTER_TICKS_COUNTER;

int32_t debuger = 0;
void debuger_print()
{
    if (debuger)
        sbi_console_putchar('6');
}

/* decide who is the next running process */
/* current_running->exec could be 1, which means we come here from do_execve() */
void do_scheduler(void)
{
    // static int32_t cnt = 0;
    // prints("%d\n", cnt++);
    // Modify the current_running pointer.
    pcb_t *previous_running = current_running;
    // put previous running into queue
    if (previous_running->status == TASK_RUNNING)
    {        
        previous_running->status = TASK_READY;
        previous_running->temp_priority = previous_running->priority;
        list_add_tail(&previous_running->list,&ready_queue);
    }

    /* kernel time count */
    kernel_time_count(); // kernel trans?
    scheduler_switch_time_count();

    // choose next running
    /* priority schedule*/
    current_running = NULL;
    /*priority schedule*/
    list_node_t *pt_readyqueue = ready_queue.next;
    pcb_t *pt_pcb;

    int32_t max_priority = -1;

    while (pt_readyqueue != &ready_queue)
    {
        pt_pcb = pt_readyqueue->ptr;

        uint64_t mask = pt_pcb->mask, cpuID = 0;
        if ( ((mask >> cpuID) & 0x1) != 0 && max_priority < pt_pcb->temp_priority)
        {
            max_priority = pt_pcb->temp_priority;
            current_running = pt_pcb;
        }
        pt_readyqueue = pt_readyqueue->next;
    }

    if (current_running != NULL){
        current_running->status = TASK_RUNNING;
        list_del(&current_running->list);
    }

    // regs_context_t *pt_regs =
    //     (regs_context_t *)(0xffffffff80505000 - sizeof(regs_context_t));
    // prints("sepc: %lx \n", pt_regs->sepc);
    // prints("sstatus: %lx \n", pt_regs->sstatus);
    // prints("ra: %lx \n", pt_regs->regs[1]);
    // prints("sp: %lx \n", current_running->user_sp);
    // prints("gp: %lx \n", pt_regs->regs[3]);
    // prints("tp: %lx \n", pt_regs->regs[4]);
    // prints("scause:%lx\n", pt_regs->scause);
    // prints("sbadaddr: %lx\n", pt_regs->sbadaddr);
    // screen_move_cursor(1,1);

    // we have delete current running, now modify temp_priority
    pt_readyqueue = ready_queue.next;
    while (pt_readyqueue != &ready_queue)
    {
        pt_pcb = pt_readyqueue->ptr;
        pt_pcb->temp_priority++;
        pt_readyqueue = pt_readyqueue->next;
    }

    if (!current_running)
        current_running = &pid0_pcb;
    /* end schedule */
    // #ifndef K210
    // vt100_move_cursor(current_running->cursor_x,
    //                   current_running->cursor_y);
    // #endif
    if (previous_running->exec){
        previous_running->exec = 0;
        previous_running = NULL;
    }

    switch_to(previous_running,current_running);
}

/* process give up the processor */
void do_yield()
{
    yield_switch_time_count();
    do_scheduler();
}

/* free resources, but it still cannot be reused until detached from its parent */
static void freeproc(pcb_t *pcb)
{
    // log2(0, "tid %d is freed by %d", pcb->tid, current_running->tid);
    pcb->status = TASK_EXITED;
    free_all_pages(pcb->pgdir, PAGE_ALIGN(pcb->kernel_sp));
    free_all_fds(pcb);
    // handle_memory_leak(pcb);
    list_add_tail(&pcb->list,&available_queue);
}

/* pid == -1: wait for any one of children */
/* pid > 0: wait for the child whose tid == pid */
int64_t do_wait4(pid_t pid, uint16_t *status, int32_t options)
{
    debug();
    uint64_t status_ker_va = NULL;
    if (status) status_ker_va = get_kva_of(status,current_running->pgdir);
    log(0, "pid %d is waiting for %d, kva is %lx", current_running->tid , pid, status_ker_va);
    int64_t ret;

    uint8_t child_still_running = 0;
    for (uint i = 0; i < NUM_MAX_TASK; ++i)
    {
        if (pcb[i].parent.parent == current_running){
            // confirm pid
            if (pid == pcb[i].tid){
                ret = pcb[i].tid;
                /* if not exited, need to wait */
                /* if already exited, need to detach it, avoiding repeating wait */
                if (!is_exited(&pcb[i])){
                    log(0, "waiting target hasn't exited, block");
                    do_block(&current_running->list, &pcb[i].wait_list);
                    do_scheduler();
                }
                log(0, "exit status %d", pcb[i].exit_status);                    
                log(0, "ret is %ld", ret);
                if (status_ker_va) WEXITSTATUS(status_ker_va,pcb[i].exit_status);
                log(0, "waiting target has exited");
                detach_from_parent(&pcb[i]);
                return ret;
            }
            else if (pid == -1){
                if (!is_exited(&pcb[i])){
                    child_still_running = 1;
                    continue;
                }
                else{
                    log(0, "found an exited child %d", pcb[i].tid);
                    ret = pcb[i].tid;
                    detach_from_parent(&pcb[i]);
                    return ret;
                }
            }
        }
    }
    if (pid > 0){
        log(0, "no child tid %d found", pid);
        return -1;
    }
    else if (pid == -1){
        if (child_still_running){
            log(0, "child_still_running");
            current_running->is_waiting_all_children = 1;
            do_block(&current_running->list, &general_block_queue);
            do_scheduler();
            detach_from_parent(current_running->unblock_child);
            return current_running->unblock_child->tid;
        }
        else{
            log(0, "no child_still_running");
            return -1;
        }
    }
    else
        assert(0);
}

void do_block(list_node_t *list, list_head *queue)
{
    debug();
    pcb_t *pcb = list_entry(list, pcb_t, list);
    pcb->status = TASK_BLOCKED;
    list_add_tail(list,queue);   
}

/* pcb_node is of type list_node_t */
void do_unblock(void *pcb_node)
{
    debug();
    // unblock the `pcb` from the block queue   
    list_del(pcb_node);
    pcb_t *pcb = (pcb_t *)((list_node_t *)pcb_node)->ptr;
    pcb->status = TASK_READY;
    list_add_tail(pcb_node,&ready_queue);
}

/* clone a child thread for current thread */
/* FOR NOW, use tls as entry point */
/* stack : ADDR OF CHILD STACK POINT */
/* success: child pid; fail: -1 */
pid_t do_clone(uint32_t flag, uint64_t stack, pid_t ptid, void *tls, pid_t ctid, void *nouse)
{
    debug();
    // log(0, "flag: %d, stack: %lx, ptid: %lx", flag, stack, ptid);
    // log(0, "tls: %lx, ctid: %lx, nouse: %lx", tls, ctid, nouse);
    for (uint i = 1; i < NUM_MAX_TASK; ++i)
        if (is_free_pcb(&pcb[i]))
        {

            pcb_t *pcb_underinit = &pcb[i];
            init_pcb_default(pcb_underinit, USER_THREAD);
            // log(0, "new tid is %d, pid_on_exec %d", pcb_underinit->tid, pcb_underinit->pid_on_exec);
            /* set some special properties */
            pcb_underinit->pid = current_running->pid; /* pid is current_running->pid before exec */

            /* init child pcb */
            ptr_t kernel_stack_top = allocPage() + NORMAL_PAGE_SIZE;  
            /* if stack = NULL, automatically set up */
            if (stack) assert(0);
            ptr_t user_stack_top = USER_STACK_ADDR;

            // pgdir
            uint64_t pgdir = allocPage();
            copy_page_table(pgdir, current_running->pgdir, current_running->user_stack_base);
            // /* user low space */
            // for (uint64_t i = current_running->elf.text_begin; i < current_running->edata; i += NORMAL_PAGE_SIZE){
            //     // log2(0, "111");
            //     uint64_t page_kva;
            //     if ((page_kva = probe_kva_of(i, current_running->pgdir)) != NULL){
            //         /* copy it */
            //         uint64_t clone_page_kva = alloc_page_helper(i, pgdir, _PAGE_ALL_MOD);
            //         // log2(0, "clone_page_kva is %lx", clone_page_kva);
            //         memcpy(clone_page_kva, get_kva_of(i, current_running->pgdir), NORMAL_PAGE_SIZE);
            //     }
            // }

            /* user stack */
            for (uint64_t i = current_running->user_stack_base; i < USER_STACK_ADDR; i += NORMAL_PAGE_SIZE){
                uint64_t clone_page_kva = alloc_page_helper(i, pgdir, _PAGE_READ|_PAGE_WRITE);
                memcpy(clone_page_kva, get_kva_of(i, current_running->pgdir), NORMAL_PAGE_SIZE);
            }

            /* set up pcb */
            init_clone_pcb(pgdir, pcb_underinit, kernel_stack_top, user_stack_top, flag);

            // add to ready queue
            list_del(&pcb_underinit->list);
            list_add_tail(&pcb_underinit->list,&ready_queue);
            // log(0, "child process usp is %lx", pcb_underinit->user_sp);
            // log(0, "child process usp base is %lx", pcb_underinit->user_stack_base);
            return pcb_underinit->tid;
        }
    return -1;
}

/* exit a program */
void do_exit(int32_t exit_status)
{
    debug();
    log(0, "tid %d is exiting", current_running->tid);
    // check if some other thread is waiting
    // if there is, unblock them

    pcb_t *pt_pcb = NULL, *q = NULL;
    list_for_each_entry_safe(pt_pcb, q, &current_running->wait_list, list){
        if (pt_pcb->status != TASK_EXITED){
            log(0, "unblock %d", pt_pcb->tid);
            if (pt_pcb->tid == 5)
                debuger = 1;
            do_unblock(&pt_pcb->list);
        }
    }

    if (current_running->parent.parent && current_running->parent.parent->is_waiting_all_children){
        log(0, "unblock %d", current_running->parent.parent->tid);
        do_unblock(&(current_running->parent.parent->list));
        current_running->parent.parent->is_waiting_all_children = 0;
        current_running->parent.parent->unblock_child = current_running;
    }
    current_running->exit_status = exit_status;
    freeproc(current_running);
    /* check if there are child process who is terminated and its source is waiting to be free */
    for (int i = 0; i < NUM_MAX_TASK; ++i)
        if (is_my_child(&pcb[i])){
            log(0, "tid %d parent exited\n", pcb[i].tid);
            if (is_exited(&pcb[i]))
                /* pcb[i].parent is of no use, should be freed */
                detach_from_parent(&pcb[i]);
            else{
                log(0, "tid %d should ENTER_ZOMBIE_ON_EXIT", pcb[i].tid);
                pcb[i].mode = ENTER_ZOMBIE_ON_EXIT;
            }
        }
    // decide terminal state by mode
    if (current_running->mode == ENTER_ZOMBIE_ON_EXIT){
        log(0, "tid %d is detaching\n", current_running->tid);
        detach_from_parent(current_running);
    }
    log(0, "exited finish");
    do_scheduler();
}

/* send signal to pid */
/* pid > 0: to the process whose process id is pid */
/* pid = 0: to all processes */
/* pid = -1: to all processes that can be sent signals by current running */
/* pid < -1: to all processes whose process id is pid */
/* sig = 0: no signal is sent, but permission check still goes */
/* success return 0, fail return -1 */
int32_t do_kill(pid_t pid, int32_t sig)
{
    debug();
    log(0, "tid %d send signum %d to tid %d", current_running->tid, sig, pid);
    uint8_t ret = 0;
    for (uint32_t i = 0; i < NUM_MAX_TASK; i++){
        if (pid > 0 && pcb[i].tid == pid){
            send_signal(sig, &pcb[i]);
            return 0;
        }
        else if (pid == 0 && !is_exited(&pcb[i])){
            send_signal(sig, &pcb[i]);
            ret++;
            continue;
        }
        else if (pid == -1 && !is_exited(&pcb[i])){
            send_signal(sig, &pcb[i]);
            ret++;
            continue;
        } /* FOR NOW */
        else if (pid < -1 && pcb[i].tid == -pid){
            send_signal(sig, &pcb[i]);
            return 0;
        }
    }
    if (ret > 0)
        return SYSCALL_SUCCESSED;
    else{
        log(0, "kill failed, no process found");
        return -1;
    }
}

/* kill a thread in thread group */
int do_tgkill(int tgid, int tid, int sig)
{
    debug();
    log(0, "tgid is %d, tid is %d, sig is %d", tgid, tid, sig);
    do_kill(tid, sig);
}

/* exit all threads */
/* same as exit FOR NOW */
void do_exit_group(int32_t exit_status)
{
    debug();
    do_exit(exit_status);
}

/***************/
//DEBUG FUNCTION
void show_ready_queue()
{
    list_node_t *test_readyqueue = ready_queue.next;
    while (test_readyqueue != &ready_queue)
    {
        pcb_t *a = test_readyqueue->ptr;
        printk_port("id[ %d ] is in ready_queue;\n",a->pid);
        test_readyqueue = test_readyqueue->next;
    }
    printk_port("\n\nid[ %d ] is running;\n",current_running->pid);
}