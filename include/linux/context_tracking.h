/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CONTEXT_TRACKING_H
#define _LINUX_CONTEXT_TRACKING_H

#include <linux/sched.h>
#include <linux/vtime.h>
#include <linux/context_tracking_state.h>
#include <linux/instrumentation.h>

#include <asm/ptrace.h>


static inline void user_enter(void) { }
static inline void user_exit(void) { }
static inline void user_enter_irqoff(void) { }
static inline void user_exit_irqoff(void) { }
static inline enum ctx_state exception_enter(void) { return 0; }
static inline void exception_exit(enum ctx_state prev_ctx) { }
static inline enum ctx_state ct_state(void) { return CONTEXT_DISABLED; }

#define CT_WARN_ON(cond) WARN_ON(context_tracking_enabled() && (cond))

#ifdef CONFIG_CONTEXT_TRACKING_FORCE
extern void context_tracking_init(void);
#else
static inline void context_tracking_init(void) { }
#endif /* CONFIG_CONTEXT_TRACKING_FORCE */


#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
/* must be called with irqs disabled */
static __always_inline void guest_enter_irqoff(void)
{
	instrumentation_begin();
	if (vtime_accounting_enabled_this_cpu())
		vtime_guest_enter(current);
	else
		current->flags |= PF_VCPU;
	instrumentation_end();

	if (context_tracking_enabled())
		__context_tracking_enter(CONTEXT_GUEST);

	/* KVM does not hold any references to rcu protected data when it
	 * switches CPU into a guest mode. In fact switching to a guest mode
	 * is very similar to exiting to userspace from rcu point of view. In
	 * addition CPU may stay in a guest mode for quite a long time (up to
	 * one time slice). Lets treat guest mode as quiescent state, just like
	 * we do with user-mode execution.
	 */
	if (!context_tracking_enabled_this_cpu()) {
		instrumentation_begin();
		rcu_virt_note_context_switch(smp_processor_id());
		instrumentation_end();
	}
}

static __always_inline void context_tracking_guest_exit(void)
{
	if (context_tracking_enabled())
		__context_tracking_exit(CONTEXT_GUEST);
}

static __always_inline void vtime_account_guest_exit(void)
{
	if (vtime_accounting_enabled_this_cpu())
		vtime_guest_exit(current);
	else
		current->flags &= ~PF_VCPU;
}

static __always_inline void guest_exit_irqoff(void)
{
	context_tracking_guest_exit();

	instrumentation_begin();
	vtime_account_guest_exit();
	instrumentation_end();
}

#else
static __always_inline void guest_enter_irqoff(void)
{
	/*
	 * This is running in ioctl context so its safe
	 * to assume that it's the stime pending cputime
	 * to flush.
	 */
	instrumentation_begin();
	vtime_account_kernel(current);
	current->flags |= PF_VCPU;
	rcu_virt_note_context_switch(smp_processor_id());
	instrumentation_end();
}

static __always_inline void context_tracking_guest_exit(void) { }

static __always_inline void vtime_account_guest_exit(void)
{
	vtime_account_kernel(current);
	current->flags &= ~PF_VCPU;
}

static __always_inline void guest_exit_irqoff(void)
{
	instrumentation_begin();
	/* Flush the guest cputime we spent on the guest */
	vtime_account_guest_exit();
	instrumentation_end();
}
#endif /* CONFIG_VIRT_CPU_ACCOUNTING_GEN */

static inline void guest_exit(void)
{
	unsigned long flags;

	local_irq_save(flags);
	guest_exit_irqoff();
	local_irq_restore(flags);
}

#endif
