// See detail at /doc/Monitor.md
#[allow(unused)]
use crate::config::{MEMORY_END, PAGE_SIZE};


// GDB debug pin
pub const QEMU:usize = 1; // 1: open in qemu mode, 0: close in real world
pub const MEMORY_GDB_START:usize  = MEMORY_END - PAGE_SIZE;
pub const PROCESSOR_ENABLE:usize  = MEMORY_GDB_START + 0;
pub const EXIT_ENABLE:usize       = MEMORY_GDB_START + 1;

#[macro_export]
macro_rules! gdb_print {
    ($place:literal, $fmt: literal $(, $($arg: tt)+)?) => {
        unsafe{
            let enable:*mut u8 =  $place;
            if *enable > 0 && QEMU == 1{
                print!($fmt $(, $($arg)+)?);
            }
        }
    };

    ($place:expr, $fmt: literal $(, $($arg: tt)+)?) => {
        unsafe{
            let enable:*mut u8 =  $place as *mut u8;
            if *enable > 0 && QEMU == 1{
                print!($fmt $(, $($arg)+)?);
            }
        }
    };
}