#pragma once

/* Shared between elf_loader_app.c and elf_loader_sdk_symbols.c only (and,
 * transitively, modules/selftest via elf_loader_sdk_symbols_test.h, which
 * includes this header too) -- not part of any public contract.
 *
 * Backs bruce_elf__exit()/bruce_elf__abort() (see the doc comment above
 * their definitions in elf_loader_sdk_symbols.c): elf_loader_app.c's
 * elf_loader__entry() setjmp()s into a bruce_elf_exit_context_t immediately
 * before calling into a loaded ELF's own code and arms it via
 * process_registry__set_sandbox_exit_target() (core/process/process.h);
 * bruce_elf__exit()/bruce_elf__abort() fetch it back, stash the exit status,
 * and longjmp() to it -- unwinding out of whatever native call depth the
 * sandboxed code was at, the same escape a real process's exit() gets from
 * the kernel rather than by returning through its own call stack. */

#include <setjmp.h>

typedef struct {
    jmp_buf target;
    /* Modified by bruce_elf__exit()/bruce_elf__abort() after
     * elf_loader__entry()'s setjmp() and read by elf_loader__entry() only
     * after the matching longjmp() returns control there -- never modified
     * between those two points on elf_loader__entry()'s own stack, so this
     * doesn't strictly need `volatile` for that reason, but it's marked
     * anyway since another function is what actually writes it. */
    volatile int exit_code;
} bruce_elf_exit_context_t;

/* 128 + SIGABRT(6): the same exit status a POSIX shell reports for a process
 * that died to an uncaught fatal signal. There is no real signal delivery
 * for a sandboxed abort() to raise here (see bruce_elf__abort()'s comment),
 * but reusing this convention lets a caller that inspects the exit code
 * (app_runner__run()'s return value, a shell script's $?, ...) still tell an
 * abort() apart from a clean exit(N). */
#define BRUCE_ELF_ABORT_EXIT_CODE 134
