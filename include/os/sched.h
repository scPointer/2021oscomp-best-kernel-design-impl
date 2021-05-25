/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *            Copyright (C) 2018 Institute of Computing Technology, CAS
 *               Author : Han Shukai (email : hanshukai@ict.ac.cn)
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *        Process scheduling related content, such as: scheduler, process blocking,
 *                 process wakeup, process creation, process kill, etc.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
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

#ifndef INCLUDE_SCHEDULER_H_
#define INCLUDE_SCHEDULER_H_


/* TODO: just add these if you needed, other content is from p3 */
#include <type.h>
#include <os/list.h>
#include <os/mm.h>
#include <os/smp.h>

#define NUM_MAX_TASK 16

/* used to save register infomation */
typedef struct regs_context
{
    /* Saved main processor registers.*/
    reg_t regs[32];

    /* Saved special registers. */
    reg_t sstatus;
    reg_t sepc;
    reg_t sbadaddr;
    reg_t scause;
    reg_t satp;
} regs_context_t;

/* used to save register infomation in switch_to */
typedef struct switchto_context
{
    /* Callee saved registers.*/
    reg_t regs[14];
} switchto_context_t;

typedef enum {
    TASK_BLOCKED,
    TASK_RUNNING,
    TASK_READY,
    TASK_ZOMBIE,
    TASK_EXITED,
} task_status_t;

typedef enum {
    ENTER_ZOMBIE_ON_EXIT,
    AUTO_CLEANUP_ON_EXIT,
} spawn_mode_t;

typedef enum {
    KERNEL_PROCESS,
    KERNEL_THREAD,
    USER_PROCESS,
    USER_THREAD,
} task_type_t;

/* Process Control Block */
typedef struct pcb
{
    /* register context */
    // this must be this order!! The order is defined in regs.h
    reg_t kernel_sp;
    reg_t user_sp;

    // count the number of disable_preempt
    // enable_preempt enables CSR_SIE only when preempt_count == 0
    reg_t preempt_count;

    // stack base
    ptr_t kernel_stack_base;
    ptr_t user_stack_base;

    /* previous, next pointer */
    list_node_t list;
    list_head wait_list;

    /* process id */
    pid_t pid;

    /* kernel/user thread/process */
    task_type_t type;

    /* BLOCK | READY | RUNNING */
    task_status_t status;

    /* spawn mode */
    spawn_mode_t mode;
    uint32_t spawn_num;

    /* priority */
    int32_t priority;
    int32_t temp_priority;

    /* mask CPU */
    uint64_t mask;

    /* kva pgdir */
    uint64_t pgdir;

    /* cursor position */
    int32_t cursor_x;
    int32_t cursor_y;
} pcb_t;

/* task information, used to init PCB */
typedef struct task_info
{
    ptr_t entry_point;
    task_type_t type;
} task_info_t;

/* ready queue to run */
extern list_head ready_queue;
extern list_head general_block_queue;
extern list_head available_queue;
extern list_head fileop_queue;

/* current running task PCB */
extern pcb_t * volatile current_running;
extern pid_t process_id;

extern pcb_t pcb[NUM_MAX_TASK];
extern pcb_t pid0_pcb;
// extern pcb_t pid0_pcbs[NR_CPUS];
extern pcb_t pid0_pcb2;
extern const ptr_t pid0_stack;
extern const ptr_t pid0_stack2;

extern void __global_pointer$();

void init_pcb_stack(ptr_t pgdir, ptr_t kernel_stack, ptr_t user_stack, ptr_t entry_point,int argc, void* arg,pcb_t *pcb);
extern void ret_from_exception();
extern void switch_to(pcb_t *prev, pcb_t *next);
void do_scheduler(void);

pid_t do_spawn(task_info_t *task, void* arg, spawn_mode_t mode);
void do_exit(void);
void do_sleep(uint32_t);

void do_block(list_node_t *, list_head *queue);
void do_unblock(void *);

int do_kill(pid_t pid);
int do_waitpid(pid_t pid);
void do_process_show();
pid_t do_getpid();
int do_taskset(uint32_t pid,uint32_t mask);

void do_exit();
pid_t do_exec(const char* file_name, int argc, char* argv[], spawn_mode_t mode);

/* scheduler counter */
extern int FORMER_TICKS_COUNTER;
extern int LATTER_TICKS_COUNTER;
void start_counter();
void end_counter();

extern pid_t do_exec(const char* file_name, int argc, char* argv[], spawn_mode_t mode);
extern void do_show_exec();

#endif