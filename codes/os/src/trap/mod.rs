mod context;

use riscv::register::{
    mtvec::TrapMode,
    stvec,
    scause::{
        self,
        Trap,
        Exception,
        Interrupt,
    },
    sepc,
    stval,
    sie,
};
use crate::mm::{
    VirtAddr,
    VirtPageNum,
};
use crate::syscall::syscall;
use crate::task::{
    exit_current_and_run_next,
    suspend_current_and_run_next,
    current_user_token,
    current_trap_cx,
    get_core_id,
    current_task,
};
use crate::timer::set_next_trigger;
use crate::config::{TRAP_CONTEXT, TRAMPOLINE};

global_asm!(include_str!("trap.S"));

pub fn init() {
    set_kernel_trap_entry();
}

fn set_kernel_trap_entry() {
    unsafe {
        stvec::write(trap_from_kernel as usize, TrapMode::Direct);
    }
}

fn set_user_trap_entry() {
    unsafe {
        stvec::write(TRAMPOLINE as usize, TrapMode::Direct);
    }
}

pub fn enable_timer_interrupt() {
    unsafe { sie::set_stimer(); }
}

#[no_mangle]
pub fn trap_handler() -> ! {
    set_kernel_trap_entry();
    let scause = scause::read();
    let stval = stval::read();
    match scause.cause() {
        Trap::Exception(Exception::UserEnvCall) => {
            // jump to next instruction anyway
            let mut cx = current_trap_cx();
            cx.sepc += 4;
            // get system call return value
            let result = syscall(cx.x[17], [cx.x[10], cx.x[11], cx.x[12], cx.x[13], cx.x[14], cx.x[15]]);
            // cx is changed during sys_exec, so we have to call it again
            cx = current_trap_cx();
            cx.x[10] = result as usize;
            // println!{"cx written..."}
        }
        Trap::Exception(Exception::StoreFault) |
        Trap::Exception(Exception::InstructionFault) |
        Trap::Exception(Exception::InstructionPageFault) |
        Trap::Exception(Exception::LoadFault) => {
            println!(
                "[kernel] {:?} in application, bad addr = {:#x}, bad instruction = {:#x}, core dumped.",
                scause.cause(),
                stval,
                current_trap_cx().sepc,
            );
            // page fault exit code
            exit_current_and_run_next(-2);
        }
        Trap::Exception(Exception::StorePageFault) |
        Trap::Exception(Exception::LoadPageFault) => {
            let va: VirtAddr = (stval as usize).into();
            // The boundary decision
            if va > TRAMPOLINE.into() {
                panic!("VirtAddr out of range!");
            }
            let vpn: VirtPageNum = va.floor();
            // Get the task inner of current
            // let mut pcb_inner = current_task().unwrap().acquire_inner_lock();
            // get the PageTableEntry that faults
            let pte = current_task().unwrap().acquire_inner_lock().translate_vpn(va.floor());
            println!{"PageTableEntry: {}", pte.bits};
            // if the virtPage is a CoW
            if pte.is_cow() {
                current_task().unwrap().acquire_inner_lock().cow_alloc(vpn);
            } else {
                println!(
                    "[kernel] {:?} in application, bad addr = {:#x}, bad instruction = {:#x}, core dumped.",
                    scause.cause(),
                    stval,
                    current_trap_cx().sepc,
                );
                // page fault exit code
                exit_current_and_run_next(-2);
            }
        }
        Trap::Exception(Exception::IllegalInstruction) => {
            println!("[kernel] IllegalInstruction in application, core dumped.");
            // illegal instruction exit code
            exit_current_and_run_next(-3);
        }
        Trap::Interrupt(Interrupt::SupervisorTimer) => {
            set_next_trigger();
            suspend_current_and_run_next();
        }
        _ => {
            panic!("Unsupported trap {:?}, stval = {:#x}!", scause.cause(), stval);
        }
    }
    //println!("before trap_return");
    trap_return();
}

#[no_mangle]
pub fn trap_return() -> ! {
    set_user_trap_entry();
    // println!("core:{} trap return ",get_core_id());
    let trap_cx_ptr = TRAP_CONTEXT;
    let user_satp = current_user_token();
    extern "C" {
        fn __alltraps();
        fn __restore();
    }
    let restore_va = __restore as usize - __alltraps as usize + TRAMPOLINE;
    unsafe {
        llvm_asm!("fence.i" :::: "volatile");
        llvm_asm!("jr $0" :: "r"(restore_va), "{a0}"(trap_cx_ptr), "{a1}"(user_satp) :: "volatile");
    }
    panic!("Unreachable in back_to_user!");
}

#[no_mangle]
pub fn trap_from_kernel() -> ! {
    panic!("a trap {:?} from kernel! Stvec:{:x}", scause::read().cause(), stvec::read().bits());
}

pub use context::{TrapContext};
