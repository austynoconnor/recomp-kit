/* intrinsics.h - runtime intrinsics the translator substitutes for guest
 * functions by address (the intrinsic override map).
 *
 * Included by generated C, so this header stays C-compatible.
 *
 * Currently: the MSVC CRT's _setjmp/_longjmp pair (_longjmp is 0x0055db78).
 * Both are cdecl, so each intrinsic emulates the guest RET by popping only the
 * return address; the caller cleans up the arguments.
 *
 * There are two ways to substitute _setjmp, and the two-call form is the
 * correct one:
 *
 *   1. Two-call (preferred). The translator emits the host setjmp at the call
 *      site, inside the generated function whose frame the guest will return
 *      into:
 *
 *          { jmp_buf *b = recomp_setjmp_prepare(c);
 *            recomp_setjmp_return(c, setjmp(*b)); }
 *
 *      The host jmp_buf then belongs to a frame that is still live when
 *      _longjmp fires, which is what the C standard requires.
 *
 *   2. `recomp_setjmp(c)` exists only to catch a call site that was not
 *      translated into form 1. It cannot work: the host setjmp would belong to
 *      a frame that has returned by the time _longjmp fires. It logs what the
 *      call site should emit and aborts.
 */
#ifndef RECOMP_INTRINSICS_H
#define RECOMP_INTRINSICS_H

#include <setjmp.h>
#include <stdint.h>

#if __has_include("x86.h")
#include "x86.h"
#else
#include "x86.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Identity in the generated baseline; the native host overrides only the
 * audited visual phase reads, keeping the guest's render-frame ID intact. */
uint32_t recomp_visual_animation_tick(uint32_t original_tick);

/* Saves the guest register file, keyed by the guest jmp_buf at ESP+4, and
 * returns the host jmp_buf to pass to setjmp. */
jmp_buf *recomp_setjmp_prepare(X86 *c);

/* Completes a _setjmp call: sets EAX to `value` and emulates the cdecl RET.
 * Call it with the result of setjmp on the buffer prepare() returned. */
void recomp_setjmp_return(X86 *c, int value);

/* Not a working substitution: logs the required call-site form and aborts. */
void recomp_setjmp(X86 *c);

/* _longjmp(jmp_buf, value): restores the guest registers saved by the matching
 * _setjmp and transfers control back to it. Does not return. */
void recomp_longjmp(X86 *c);

/* The MSVC CRT's _ftol2: ST(0) truncated toward zero into EDX:EAX, then
 * popped. The CRT gets there with FISTP in the current rounding mode and a
 * one-unit correction from the sign of the remainder; the answer is a plain
 * truncation whatever the control word says. A value outside the int64 range,
 * or a NaN, gives the integer indefinite 0x8000000000000000 and raises the
 * invalid-operation flag, as the CRT's FISTP does. ECX and EFLAGS, which the
 * CRT clobbers, are left alone; no caller can depend on either.
 *
 * recomp_ftol2_value is the whole effect apart from CALL/RET, so a direct
 * call site can run it in line. recomp_ftol2 is the body for a call that
 * reaches it through a pointer: the same, then the cdecl RET. */
static inline void recomp_ftol2_value(X86 *c) {
    int64_t r;
    if (c->st_exact[c->fpu_top]) {
        r = (int64_t)c->st_bits[c->fpu_top];
    } else {
        double v = ST(c, 0);
        if (v >= -9223372036854775808.0 && v < 9223372036854775808.0) {
            r = (int64_t)v;
        } else {
            r = INT64_MIN;
            c->fpu_sw |= 0x0001u;
        }
    }
    fdrop(c);
    c->r[R_EAX] = (uint32_t)(uint64_t)r;
    c->r[R_EDX] = (uint32_t)((uint64_t)r >> 32);
}
static inline void recomp_ftol2(X86 *c) {
    recomp_ftol2_value(c);
    c->eip = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4;
    recomp_return(c);
}

/* The return address the runtime pushes when it calls a guest callback
 * (WNDPROC, thread body, multimedia timer). Executing it is a bug and the
 * runtime reports it by name. */
#define RECOMP_CALLBACK_RETURN 0x0fdfff00u

#ifdef __cplusplus
}
#endif
#endif /* RECOMP_INTRINSICS_H */
