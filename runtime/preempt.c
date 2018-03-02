/*
 * preempt.c - support for kthread preemption
 */

#include <signal.h>
#include <string.h>

#include "base/log.h"
#include "runtime/thread.h"
#include "runtime/preempt.h"

#include "defs.h"

/* the current preemption count */
volatile __thread unsigned int preempt_cnt = PREEMPT_NOT_PENDING;

/* set a flag to indicate a preemption request is pending */
static void set_preempt_needed(void)
{
	preempt_cnt &= ~PREEMPT_NOT_PENDING;
}

/* handles preemption signals from the iokernel */
static void handle_sigusr1(int s, siginfo_t *si, void *c)
{
	struct kthread *k = myk();
	ucontext_t *ctx = c;

	assert(ctx->uc_stack.ss_sp == (void*)signal_stack);

	STAT(PREEMPTIONS)++;

	/* resume execution if preemption is disabled */
	if (!preempt_enabled()) {
		set_preempt_needed();
		return;
	}

	/* save preempted state and park the kthread */
	spin_lock(&k->lock);
	k->preempted = true;
	memcpy(&k->preempted_uctx, ctx, sizeof(*ctx));
	k->preempted_th = thread_self();

	assert((uintptr_t)ctx->uc_mcontext.fpregs >= (uintptr_t)ctx);
	assert((uintptr_t)ctx->uc_mcontext.fpregs < (uintptr_t)ctx + sizeof(*ctx));
	k->fpstate_offset = (uintptr_t)ctx->uc_mcontext.fpregs - (uintptr_t)ctx;

	kthread_park(false);

	/* check if no other kthread stole our preempted work */
	if (k->preempted) {
		k->preempted = false;
		spin_unlock(&k->lock);
		return;
	}

	/* otherwise our context is executing elsewhere, return to scheduler */
	spin_unlock(&k->lock);
	sched_make_uctx(ctx);
	preempt_disable();
}

/**
 * preempt - entry point for preemption
 */
void preempt(void)
{
	assert(preempt_needed());
	clear_preempt_needed();
	thread_yield();
}

/**
 * preempt_reenter - jump back into a thread context that was preempted
 * @c: the ucontext of the thread
 */
void preempt_reenter(ucontext_t *c, size_t fpstate_offset)
{

	/* Re-arm with the correct local signal stack */
	c->uc_stack.ss_sp = (void*)signal_stack;
	c->uc_stack.ss_size =  sizeof(signal_stack->usable);
	c->uc_stack.ss_flags = 0;

	/* Verify a few assumptions made in __jmp_restore_sigctx */
	BUILD_ASSERT(sizeof(ucontext_t) == 936);
	BUILD_ASSERT(offsetof(ucontext_t, uc_mcontext) + offsetof(mcontext_t, fpregs) == 224);

	preempt_enable();
	__jmp_restore_sigctx(c, fpstate_offset);

	unreachable();
}

/**
 * preempt_init - global initializer for preemption support
 *
 * Returns 0 if successful. otherwise fail.
 */
int preempt_init(void)
{
	struct sigaction act;

	act.sa_sigaction = handle_sigusr1;
	act.sa_flags = SA_SIGINFO | SA_ONSTACK;

	if (sigemptyset(&act.sa_mask) != 0) {
		log_err("couldn't empty the signal handler mask");
		return -errno;
	}

	if (sigaddset(&act.sa_mask, SIGUSR1)) {
		log_err("couldn't set signal handler mask");
		return -errno;
	}

	if (sigaction(SIGUSR1, &act, NULL) == -1) {
		log_err("couldn't register signal handler");
		return -errno;
	}

	return 0;
}