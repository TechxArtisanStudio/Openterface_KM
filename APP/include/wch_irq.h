/**
 * wch_irq.h — how an interrupt handler is declared, portably.
 *
 * WHY THIS EXISTS
 * ---------------
 * The V3 core in CH32V208 (and the rest of the WCH line) can stack the
 * caller-saved registers itself, so WCH's compiler ships a non-standard
 * attribute for it:
 *
 *     void SysTick_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
 *
 * which emits the body with no register save/restore and returns with `mret`.
 * Upstream GCC does not know that string.  From GCC 12 on it does not reject
 * the declaration either -- it prints
 *
 *     warning: argument to 'interrupt' attribute is not '"user"',
 *              '"supervisor"', or '"machine"' [-Wattributes]
 *
 * and then compiles the function as a *plain* function: no `mret`, no
 * register save.  The image links, flashes, verifies, and is dead: the first
 * interrupt (SysTick, ~1 ms after reset) runs off the end of the handler with
 * a normal `ret` and the core jumps into whatever the return address happens
 * to be.  USB never enumerates, so the symptom looks like a broken descriptor
 * or a power problem rather than a code-generation one.
 *
 * So: use WCH's fast form only where it is understood (the MRS 8.2 toolchain,
 * __GNUC__ < 12), and the standard `interrupt` form everywhere else.  The
 * standard form saves and restores registers in software -- a few cycles per
 * interrupt slower, and it works on both toolchains.
 *
 * Verified: a keymod-plus mode-8 image that was completely silent when built
 * with GCC 15.2 comes up (NIC + descriptors) with WCH_IRQ_FAST selecting the
 * standard form, with no other change.
 */
#ifndef WCH_IRQ_H
#define WCH_IRQ_H

#if defined(__GNUC__) && (__GNUC__ < 12)
/* WCH-patched GCC 8.2: fast interrupt, core stacks the registers. */
#define WCH_IRQ_FAST __attribute__((interrupt("WCH-Interrupt-fast")))
#else
/* Upstream GCC: standard interrupt, handler stacks the registers. */
#define WCH_IRQ_FAST __attribute__((interrupt))
#endif

#endif /* WCH_IRQ_H */
