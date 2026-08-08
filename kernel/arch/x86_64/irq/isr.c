#include <stdint.h>
#include "drivers/char/serial.h"
#include "arch/x86_64/cpu/spinlock.h"
#include "process/process.h"   /* current_thread, struct process/thread */
#include "process/debug.h"     /* debug_on_exception (§6.6 exception routing) */
#include "lib/ksym.h"          /* the panic symbolizer (§7) */
#include "arch/x86_64/syscall/usercopy.h"   /* access_ok, for the ring-3 walk */

/* Serializes the exception dump so two faulting cores don't interleave their
 * reports byte-by-byte over the lockless UART (observed directly as two
 * simultaneous double faults under -smp 4 producing an unreadable dump).
 *
 * Held with two DIFFERENT lifetimes depending on how the fault ends:
 *  - RECOVERABLE (a ring-3 fault -> kill just that process): released after the
 *    dump, because the kernel keeps running and the NEXT app crash must be able
 *    to take this lock too. Never-releasing it here would deadlock the second
 *    crash.
 *  - TERMINAL (a ring-0/kernel fault -> halt): never released. A second core
 *    that faults while this one is dumping/halted just waits here forever rather
 *    than corrupt the one crash report that matters. */
static spinlock_t panic_lock = SPINLOCK_INIT;


// structure to hold the CPU register state during an interrupt
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code; // Interrupt vector number and error code (if applicable)
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));


static const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved (15)",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved (22)",
    "Reserved (23)",
    "Reserved (24)",
    "Reserved (25)",
    "Reserved (26)",
    "Reserved (27)",
    "Reserved (28)",
    "Reserved (29)",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
};


/* The register + fault-cause + symbolized-backtrace dump. Factored out of
 * isr_handler so both the recoverable (kill-the-process) and terminal (halt)
 * paths report identically. Caller holds panic_lock; this does no locking. */
static void dump_fault(struct registers *regs) {
    serial_write_string("\n=== Exception ===\n");
    serial_write_string("Vector: ");
    serial_write_hex(regs->vector);
    serial_write_string(" (");

    if (regs->vector < 32) {
        serial_write_string(exception_messages[regs->vector]);
    } else {
        serial_write_string("Unknown Exception");
        }

    serial_write_string(")\n");

    serial_write_string("Error code: ");
    serial_write_hex(regs->error_code);
    serial_write_string("\n");

    serial_write_string("RIP: ");
    serial_write_hex(regs->rip);
    serial_write_string("\n");

    serial_write_string("CS: ");
    serial_write_hex(regs->cs);
    serial_write_string("\n");

    serial_write_string("RSP: ");
    serial_write_hex(regs->rsp);
    serial_write_string("\n");

    serial_write_string("SS: ");
    serial_write_hex(regs->ss);
    serial_write_string("\n");

    serial_write_string("RBP: ");
    serial_write_hex(regs->rbp);
    serial_write_string("\n");

    // Page Fault (vector 14): CR2 holds the faulting address, and the
    // error code's low bits explain the cause.
    if (regs->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        serial_write_string("CR2 (fault addr): ");
        serial_write_hex(cr2);
        serial_write_string("\n");

        // Decode the error code bits (Intel SDM Vol 3A, 4.7 "Page-Fault
        // Exceptions"). Each bit explains one aspect of the fault.
        uint64_t e = regs->error_code;
        serial_write_string("Cause: ");
        serial_write_string((e & 0x1) ? "protection-violation" : "not-present");
        serial_write_string((e & 0x2) ? ", write" : ", read");
        serial_write_string((e & 0x4) ? ", user-mode" : ", kernel-mode");
        if (e & 0x8)  serial_write_string(", reserved-bit-set");
        if (e & 0x10) serial_write_string(", instruction-fetch");
        serial_write_string("\n");
    }

    /* Symbolized RIP + rbp-chain backtrace (EMBDBG_Specification.md §7). Once
     * the kernel's own .embdbg is loaded, a hex RIP becomes func (file:line)
     * and the call path is named frame by frame — the whole point of the
     * format's cheapest consumer. Guarded to kernel addresses; the walk reads
     * only kernel VAs and is capped, so a bad frame ends the walk rather than
     * faulting the panic path into a second fault. */
    if (ksym_ready() && regs->rip >= 0xffffffff80000000ULL) {
        char sym[176];
        ksym_symbolize(regs->rip, sym, sizeof sym);
        serial_write_string("\n--- backtrace ---\n  ");
        serial_write_string(sym);
        serial_write_string("   <-- faulting\n");
        uint64_t rbp = regs->rbp;
        for (int i = 0; i < 24; i++) {
            if (rbp < 0xffff800000000000ULL || (rbp & 0x7)) break;
            uint64_t ret  = *(volatile uint64_t *)(uintptr_t)(rbp + 8);
            uint64_t next = *(volatile uint64_t *)(uintptr_t)(rbp);
            if (ret < 0xffffffff80000000ULL) break;   /* left kernel .text */
            ksym_symbolize(ret, sym, sizeof sym);
            serial_write_string("  ");
            serial_write_string(sym);
            serial_write_string("\n");
            if (next <= rbp) break;                    /* chain must climb */
            rbp = next;
        }
        serial_write_string("--- end backtrace ---\n");
    }

    /* A RING-3 backtrace, in raw addresses.
     *
     * The kernel walk above stops at the kernel's own .text, which is exactly
     * no help when the faulting code is an application -- and "RIP=0" says a
     * call or return went through a null pointer without saying whose. The
     * user stack is mapped right now (same CR3), so the rbp chain is readable;
     * every read goes through access_ok so a corrupt frame ends the walk
     * instead of faulting the fault handler.
     *
     * Addresses, not symbols: the kernel has its own .embdbg and knows nothing
     * about an application's. `nm build/<app>.elf` turns these into names, and
     * an ET_EXEC app is loaded at bias 0 so they match directly. */
    if ((regs->cs & 0x3) == 0x3) {
        serial_write_string("\n--- ring-3 backtrace (addresses; nm the app) ---\n");
        serial_write_string("  rip  "); serial_write_hex(regs->rip); serial_write_string("\n");
        /* The return address the faulting frame would use, if rsp still points
         * at it -- the usual shape when a CALL through a null pointer faults
         * on the very first instruction fetch. */
        if (access_ok((const void *)(uintptr_t)regs->rsp, 8)) {
            serial_write_string("  ret@rsp  ");
            serial_write_hex(*(volatile uint64_t *)(uintptr_t)regs->rsp);
            serial_write_string("\n");
        }
        uint64_t rbp = regs->rbp;
        for (int i = 0; i < 24; i++) {
            if ((rbp & 0x7) || !access_ok((const void *)(uintptr_t)rbp, 16)) break;
            uint64_t next = *(volatile uint64_t *)(uintptr_t)(rbp);
            uint64_t ret  = *(volatile uint64_t *)(uintptr_t)(rbp + 8);
            if (!ret) break;
            serial_write_string("  frame "); serial_write_hex(ret); serial_write_string("\n");
            if (next <= rbp) break;                    /* chain must climb */
            rbp = next;
        }
        serial_write_string("--- end ring-3 backtrace ---\n");
    }
}


void isr_handler(struct registers *regs) {
    /* EMBDBG_Specification.md §6.6 — the debug pre-dispatch, BEFORE either final
     * path below. For the debug-relevant vectors, a fault from a thread whose
     * process has a debug session is delivered to the debugger as a STOP EVENT:
     * the faulting thread is parked, the debugger is woken, and when it resumes
     * us debug_on_exception returns 1 and we iretq back to ring 3. struct
     * registers has the same layout as struct regs, so the cast is exact. */
    switch (regs->vector) {
    case 0: case 1: case 3: case 6: case 13: case 14: {
        struct thread *t = current_thread;
        if (t && t->proc && t->proc->debug_session &&
            debug_on_exception(t, (struct regs *)regs)) {
            return;   /* handled: parked + resumed -> iretq back to userspace */
        }
        break;
    }
    default: break;
    }

    /* A fault from ring 3 (CPL == 3) with no debugger attached is the faulting
     * PROCESS's bug, not the kernel's: kill just that process and let the kernel
     * keep running -- a userspace crash must never take down the machine. The
     * faulting instruction was user code, so this thread holds no kernel locks;
     * process_exit_self() zombies it and schedules away, exactly as an abnormal
     * sys_exit would. A ring-0 fault still halts below: a corrupt kernel cannot
     * safely continue. EMBDBG_Specification.md §6.6, the non-debugged arm. */
    int user_fault = ((regs->cs & 0x3) == 0x3) && current_thread && current_thread->proc;

    spin_lock(&panic_lock);
    dump_fault(regs);

    if (user_fault) {
        serial_write_string("ring-3 fault -> terminating pid ");
        serial_write_hex(current_thread->proc->pid);
        serial_write_string("; kernel continues.\n");
        spin_unlock(&panic_lock);     /* recoverable: MUST release (see panic_lock) */
        process_exit_self(PROCESS_EXIT_FAULT(regs->vector));   /* noreturn */
    }

    serial_write_string("kernel-mode fault -- system halted.\n");
    while (1) {
        __asm__ volatile ("cli; hlt");
    }
}