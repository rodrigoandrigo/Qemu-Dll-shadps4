/*
 * Win32 coroutine initialization code
 *
 * Copyright (c) 2011 Kevin Wolf <kwolf@redhat.com>
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/coroutine_int.h"
#include "qemu/coroutine-tls.h"

typedef struct
{
    Coroutine base;

#ifdef CONFIG_UWP
    struct {
        uint64_t rbx, rbp, rdi, rsi, rsp, r12, r13, r14, r15;
        uint64_t reserved;
        uint8_t xmm[10][16];
        uint32_t mxcsr;
        uint16_t fpucw;
    } context;
    void *stack;
#else
    LPVOID fiber;
#endif
    CoroutineAction action;
} CoroutineWin32;

QEMU_DEFINE_STATIC_CO_TLS(CoroutineWin32, leader);
QEMU_DEFINE_STATIC_CO_TLS(Coroutine *, current);

#ifdef CONFIG_UWP
QEMU_BUILD_BUG_ON(offsetof(CoroutineWin32, context.rbx) !=
                  offsetof(CoroutineWin32, context));
QEMU_BUILD_BUG_ON(offsetof(CoroutineWin32, context.rsp) -
                  offsetof(CoroutineWin32, context) != 32);
QEMU_BUILD_BUG_ON(offsetof(CoroutineWin32, context.xmm) -
                  offsetof(CoroutineWin32, context) != 80);

/* Windows x64 callee-saved state, including the nonvolatile XMM registers. */
static void __attribute__((naked))
qemu_coroutine_switch_context(void *from, void *to)
{
    __asm__ volatile(
        "movq %rbx, 0(%rcx)\n"
        "movq %rbp, 8(%rcx)\n"
        "movq %rdi, 16(%rcx)\n"
        "movq %rsi, 24(%rcx)\n"
        "movq %rsp, 32(%rcx)\n"
        "movq %r12, 40(%rcx)\n"
        "movq %r13, 48(%rcx)\n"
        "movq %r14, 56(%rcx)\n"
        "movq %r15, 64(%rcx)\n"
        "movdqu %xmm6, 80(%rcx)\n"
        "movdqu %xmm7, 96(%rcx)\n"
        "movdqu %xmm8, 112(%rcx)\n"
        "movdqu %xmm9, 128(%rcx)\n"
        "movdqu %xmm10, 144(%rcx)\n"
        "movdqu %xmm11, 160(%rcx)\n"
        "movdqu %xmm12, 176(%rcx)\n"
        "movdqu %xmm13, 192(%rcx)\n"
        "movdqu %xmm14, 208(%rcx)\n"
        "movdqu %xmm15, 224(%rcx)\n"
        "stmxcsr 240(%rcx)\n"
        "fnstcw 244(%rcx)\n"
        "movq 0(%rdx), %rbx\n"
        "movq 8(%rdx), %rbp\n"
        "movq 16(%rdx), %rdi\n"
        "movq 24(%rdx), %rsi\n"
        "movq 32(%rdx), %rsp\n"
        "movq 40(%rdx), %r12\n"
        "movq 48(%rdx), %r13\n"
        "movq 56(%rdx), %r14\n"
        "movq 64(%rdx), %r15\n"
        "movdqu 80(%rdx), %xmm6\n"
        "movdqu 96(%rdx), %xmm7\n"
        "movdqu 112(%rdx), %xmm8\n"
        "movdqu 128(%rdx), %xmm9\n"
        "movdqu 144(%rdx), %xmm10\n"
        "movdqu 160(%rdx), %xmm11\n"
        "movdqu 176(%rdx), %xmm12\n"
        "movdqu 192(%rdx), %xmm13\n"
        "movdqu 208(%rdx), %xmm14\n"
        "movdqu 224(%rdx), %xmm15\n"
        "ldmxcsr 240(%rdx)\n"
        "fldcw 244(%rdx)\n"
        "ret\n");
}
#endif

/* This function is marked noinline to prevent GCC from inlining it
 * into coroutine_trampoline(). If we allow it to do that then it
 * hoists the code to get the address of the TLS variable "current"
 * out of the while() loop. This is an invalid transformation because
 * the SwitchToFiber() call may be called when running thread A but
 * return in thread B, and so we might be in a different thread
 * context each time round the loop.
 */
CoroutineAction __attribute__((noinline))
qemu_coroutine_switch(Coroutine *from_, Coroutine *to_,
                      CoroutineAction action)
{
    CoroutineWin32 *from = DO_UPCAST(CoroutineWin32, base, from_);
    CoroutineWin32 *to = DO_UPCAST(CoroutineWin32, base, to_);

    set_current(to_);

    to->action = action;
#ifdef CONFIG_UWP
    qemu_coroutine_switch_context(&from->context, &to->context);
#else
    SwitchToFiber(to->fiber);
#endif
    return from->action;
}

#ifdef CONFIG_UWP
static void coroutine_trampoline(void)
#else
static void CALLBACK coroutine_trampoline(void *co_)
#endif
{
#ifdef CONFIG_UWP
    Coroutine *co = get_current();
#else
    Coroutine *co = co_;
#endif

    while (true) {
        co->entry(co->entry_arg);
        qemu_coroutine_switch(co, co->caller, COROUTINE_TERMINATE);
    }
}

Coroutine *qemu_coroutine_new(void)
{
    const size_t stack_size = COROUTINE_STACK_SIZE;
    CoroutineWin32 *co;

    co = g_malloc0(sizeof(*co));
#ifdef CONFIG_UWP
    uintptr_t stack_top;

    co->stack = g_malloc(stack_size + 16);
    stack_top = QEMU_ALIGN_DOWN((uintptr_t)co->stack + stack_size + 16, 16);
    stack_top -= 16;
    *(uintptr_t *)stack_top = (uintptr_t)coroutine_trampoline;
    co->context.rsp = stack_top;
    __asm__ volatile("stmxcsr %0" : "=m" (co->context.mxcsr));
    __asm__ volatile("fnstcw %0" : "=m" (co->context.fpucw));
#else
    co->fiber = CreateFiber(stack_size, coroutine_trampoline, &co->base);
#endif
    return &co->base;
}

void qemu_coroutine_delete(Coroutine *co_)
{
    CoroutineWin32 *co = DO_UPCAST(CoroutineWin32, base, co_);

#ifdef CONFIG_UWP
    g_free(co->stack);
#else
    DeleteFiber(co->fiber);
#endif
    g_free(co);
}

Coroutine *qemu_coroutine_self(void)
{
    Coroutine *current = get_current();

    if (!current) {
        CoroutineWin32 *leader = get_ptr_leader();

        current = &leader->base;
        set_current(current);
#ifndef CONFIG_UWP
        leader->fiber = ConvertThreadToFiber(NULL);
#endif
    }
    return current;
}

bool qemu_in_coroutine(void)
{
    Coroutine *current = get_current();

    return current && current->caller;
}
