/*
 * Based on arch/arm/kernel/traps.c
 *
 * Copyright (C) 1995-2009 Russell King
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/signal.h>
#include <linux/personality.h>
#include <linux/kallsyms.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/hardirq.h>
#include <linux/kdebug.h>
#include <linux/module.h>
#include <linux/kexec.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/syscalls.h>

#include <asm/atomic.h>
#include <asm/debug-monitors.h>
#include <asm/traps.h>
#include <asm/stacktrace.h>
#include <asm/exception.h>
#include <asm/system_misc.h>

static const char *handler[]= {
	"Synchronous Abort",
	"IRQ",
	"FIQ",
	"Error"
};

int show_unhandled_signals = 1;

/*
 * Dump out the contents of some memory nicely...
 */
static void dump_mem(const char *lvl, const char *str, unsigned long bottom,
		     unsigned long top)
{
	unsigned long first;
	mm_segment_t fs;
	int i;

	/*
	 * We need to switch to kernel mode so that we can use __get_user
	 * to safely read from kernel space.  Note that we now dump the
	 * code first, just in case the backtrace kills us.
	 */
	fs = get_fs();
	set_fs(KERNEL_DS);

	printk("%s%s(0x%016lx to 0x%016lx)\n", lvl, str, bottom, top);

	for (first = bottom & ~31; first < top; first += 32) {
		unsigned long p;
		char str[sizeof(" 12345678") * 8 + 1];

		memset(str, ' ', sizeof(str));
		str[sizeof(str) - 1] = '\0';

		for (p = first, i = 0; i < 8 && p < top; i++, p += 4) {
			if (p >= bottom && p < top) {
				unsigned int val;
				if (__get_user(val, (unsigned int *)p) == 0)
					sprintf(str + i * 9, " %08x", val);
				else
					sprintf(str + i * 9, " ????????");
			}
		}
		printk("%s%04lx:%s\n", lvl, first & 0xffff, str);
	}

	set_fs(fs);
}

static void dump_backtrace_entry(unsigned long where, unsigned long stack)
{
	print_ip_sym(where);
	if (in_exception_text(where))
		dump_mem("", "Exception stack", stack,
			 stack + sizeof(struct pt_regs));
}

static void dump_instr(const char *lvl, struct pt_regs *regs)
{
	unsigned long addr = instruction_pointer(regs);
	mm_segment_t fs;
	char str[sizeof("00000000 ") * 5 + 2 + 1], *p = str;
	int i;

	/*
	 * We need to switch to kernel mode so that we can use __get_user
	 * to safely read from kernel space.  Note that we now dump the
	 * code first, just in case the backtrace kills us.
	 */
	fs = get_fs();
	set_fs(KERNEL_DS);

	for (i = -4; i < 1; i++) {
		unsigned int val, bad;

		bad = __get_user(val, &((u32 *)addr)[i]);

		if (!bad)
			p += sprintf(p, i == 0 ? "(%08x) " : "%08x ", val);
		else {
			p += sprintf(p, "bad PC value");
			break;
		}
	}
	printk("%sCode: %s\n", lvl, str);

	set_fs(fs);
}

static void dump_backtrace(struct pt_regs *regs, struct task_struct *tsk)
{
	struct stackframe frame;
	const register unsigned long current_sp asm ("sp");

	pr_debug("%s(regs = %p tsk = %p)\n", __func__, regs, tsk);

	if (!tsk)
		tsk = current;

	if (regs) {
		frame.fp = regs->regs[29];
		frame.sp = regs->sp;
		frame.pc = regs->pc;
	} else if (tsk == current) {
		frame.fp = (unsigned long)__builtin_frame_address(0);
		frame.sp = current_sp;
		frame.pc = (unsigned long)dump_backtrace;
	} else {
		/*
		 * task blocked in __switch_to
		 */
		frame.fp = thread_saved_fp(tsk);
		frame.sp = thread_saved_sp(tsk);
		frame.pc = thread_saved_pc(tsk);
	}

	printk("Call trace:\n");
	while (1) {
		unsigned long where = frame.pc;
		int ret;

		ret = unwind_frame(&frame);
		if (ret < 0)
			break;
		dump_backtrace_entry(where, frame.sp);
	}
}

void show_stack(struct task_struct *tsk, unsigned long *sp)
{
	dump_backtrace(NULL, tsk);
	barrier();
}

#ifdef CONFIG_PREEMPT
#define S_PREEMPT " PREEMPT"
#else
#define S_PREEMPT ""
#endif
#ifdef CONFIG_SMP
#define S_SMP " SMP"
#else
#define S_SMP ""
#endif

static int __die(const char *str, int err, struct thread_info *thread,
		 struct pt_regs *regs)
{
	struct task_struct *tsk = thread->task;
	static int die_counter;
	int ret;

	pr_emerg("Internal error: %s: %x [#%d]" S_PREEMPT S_SMP "\n",
		 str, err, ++die_counter);

	/* trap and error numbers are mostly meaningless on ARM */
	ret = notify_die(DIE_OOPS, str, regs, err, 0, SIGSEGV);
	if (ret == NOTIFY_STOP)
		return ret;

	print_modules();
	__show_regs(regs);
	pr_emerg("Process %.*s (pid: %d, stack limit = 0x%p)\n",
		 TASK_COMM_LEN, tsk->comm, task_pid_nr(tsk), thread + 1);

	if (!user_mode(regs) || in_interrupt()) {
		dump_mem(KERN_EMERG, "Stack: ", regs->sp,
			 THREAD_SIZE + (unsigned long)task_stack_page(tsk));
		dump_backtrace(regs, tsk);
		dump_instr(KERN_EMERG, regs);
	}

	return ret;
}

static DEFINE_RAW_SPINLOCK(die_lock);

/*
 * This function is protected against re-entrancy.
 */
void die(const char *str, struct pt_regs *regs, int err)
{
	struct thread_info *thread = current_thread_info();
	int ret;

	oops_enter();

	raw_spin_lock_irq(&die_lock);
	console_verbose();
	bust_spinlocks(1);
	ret = __die(str, err, thread, regs);

	if (regs && kexec_should_crash(thread->task))
		crash_kexec(regs);

	bust_spinlocks(0);
	add_taint(TAINT_DIE, LOCKDEP_NOW_UNRELIABLE);
	raw_spin_unlock_irq(&die_lock);
	oops_exit();

	if (in_interrupt())
		panic("Fatal exception in interrupt");
	if (panic_on_oops)
		panic("Fatal exception");
	if (ret != NOTIFY_STOP)
		do_exit(SIGSEGV);
}

void arm64_notify_die(const char *str, struct pt_regs *regs,
		      struct siginfo *info, int err)
{
	if (user_mode(regs))
		force_sig_info(info->si_signo, info, current);
	else
		die(str, regs, err);
}

static LIST_HEAD(undef_hook);

void register_undef_hook(struct undef_hook *hook)
{
	list_add(&hook->node, &undef_hook);
}

static int call_undef_hook(struct pt_regs *regs, unsigned int instr)
{
	struct undef_hook *hook;
	int (*fn)(struct pt_regs *regs, unsigned int instr) = NULL;

	list_for_each_entry(hook, &undef_hook, node)
		if ((instr & hook->instr_mask) == hook->instr_val &&
		    (regs->pstate & hook->pstate_mask) == hook->pstate_val)
			fn = hook->fn;

	return fn ? fn(regs, instr) : 1;
}

asmlinkage void __exception do_undefinstr(struct pt_regs *regs)
{
	u32 instr;
	siginfo_t info;
	void __user *pc = (void __user *)instruction_pointer(regs);

	/* check for AArch32 breakpoint instructions */
	if (!aarch32_break_handler(regs))
		return;
	if (user_mode(regs)) {
		if (compat_thumb_mode(regs)) {
			if (get_user(instr, (u16 __user *)pc))
				goto die_sig;
			if (is_wide_instruction(instr)) {
				u32 instr2;
				if (get_user(instr2, (u16 __user *)pc+1))
					goto die_sig;
				instr <<= 16;
				instr |= instr2;
			}
		} else if (get_user(instr, (u32 __user *)pc)) {
			goto die_sig;
		}
	} else {
		/* kernel mode */
		instr = *((u32 *)pc);
	}

	if (call_undef_hook(regs, instr) == 0)
		return;

die_sig:
	if (show_unhandled_signals && unhandled_signal(current, SIGILL) &&
	    printk_ratelimit()) {
		pr_info("%s[%d]: undefined instruction: pc=%p\n",
			current->comm, task_pid_nr(current), pc);
		dump_instr(KERN_INFO, regs);
	}

	info.si_signo = SIGILL;
	info.si_errno = 0;
	info.si_code  = ILL_ILLOPC;
	info.si_addr  = pc;

	arm64_notify_die("Oops - undefined instruction", regs, &info, 0);
}

long compat_arm_syscall(struct pt_regs *regs);

asmlinkage long do_ni_syscall(struct pt_regs *regs)
{
#ifdef CONFIG_COMPAT
	long ret;
	if (is_compat_task()) {
		ret = compat_arm_syscall(regs);
		if (ret != -ENOSYS)
			return ret;
	}
#endif

	if (show_unhandled_signals && printk_ratelimit()) {
		pr_info("%s[%d]: syscall %d\n", current->comm,
			task_pid_nr(current), (int)regs->syscallno);
		dump_instr("", regs);
		if (user_mode(regs))
			__show_regs(regs);
	}

	return sys_ni_syscall();
}

#ifdef CONFIG_DENVER_CPU
/*
 * MCA assert register dump
*/
void dump_mca_debug(void)
{
	unsigned long cap;

	unsigned long serri_ctrl, serri_status, serri_addr,
		serri_misc1, serri_misc2;

	pr_crit("Machine Check Architecture assert failed:\n");

	asm volatile("mrs %0, s3_0_c15_c3_0" : "=r" (cap) : );
	cap = cap & 0xff;

	switch(cap)
	{
	case 11:
		asm volatile("mrs %0, s3_0_c15_c11_4" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c11_5" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c11_6" : "=r" (serri_addr) : );
		pr_crit("[Bank 10] ctrl:0x%016lx status:0x%016lx addr:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr);
	case 10:
		asm volatile("mrs %0, s3_0_c15_c10_6" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c10_7" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c11_0" : "=r" (serri_addr) : );
		pr_crit("[Bank 9] ctrl:0x%016lx status:0x%016lx addr:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr);
	case 9:
		asm volatile("mrs %0, s3_0_c15_c10_0" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c10_1" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c10_2" : "=r" (serri_addr) : );
		pr_crit("[Bank 8] ctrl:0x%016lx status:0x%016lx addr:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr);
	case 8:
		asm volatile("mrs %0, s3_0_c15_c9_2" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c9_3" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c9_4" : "=r" (serri_addr) : );
		pr_crit("[Bank 7] ctrl:0x%016lx status:0x%016lx addr:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr);
	case 7:
		asm volatile("mrs %0, s3_0_c15_c8_4" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c8_5" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c8_6" : "=r" (serri_addr) : );
		asm volatile("mrs %0, s3_0_c15_c8_7" : "=r" (serri_misc1) : );
		asm volatile("mrs %0, s3_0_c15_c9_0" : "=r" (serri_misc2) : );
		pr_crit("[Bank 6] ctrl:0x%016lx status:0x%016lx addr:0x%016lx \
misc1:0x%016lx, misc2:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr, serri_misc1, serri_misc2);
	case 6:
		asm volatile("mrs %0, s3_0_c15_c7_6" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c7_7" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c8_0" : "=r" (serri_addr) : );
		pr_crit("[Bank 5] ctrl:0x%016lx status:0x%016lx addr:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr);
	case 5:
		asm volatile("mrs %0, s3_0_c15_c7_0" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c7_1" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c7_2" : "=r" (serri_addr) : );
		pr_crit("[Bank 4] ctrl:0x%016lx status:0x%016lx addr:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr);
	case 4:
		asm volatile("mrs %0, s3_0_c15_c6_2" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c6_3" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c6_4" : "=r" (serri_addr) : );
		pr_crit("[Bank 3] ctrl:0x%016lx status:0x%016lx addr:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr);
	case 3:
		asm volatile("mrs %0, s3_0_c15_c5_4" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c5_5" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c5_6" : "=r" (serri_addr) : );
		pr_crit("[Bank 2] ctrl:0x%016lx status:0x%016lx addr:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr);
	case 2:
		asm volatile("mrs %0, s3_0_c15_c4_6" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c4_7" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c5_0" : "=r" (serri_addr) : );
		pr_crit("[Bank 1] ctrl:0x%016lx status:0x%016lx addr:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr);
	case 1:
		asm volatile("mrs %0, s3_0_c15_c4_0" : "=r" (serri_ctrl) : );
		asm volatile("mrs %0, s3_0_c15_c4_1" : "=r" (serri_status) : );
		asm volatile("mrs %0, s3_0_c15_c4_2" : "=r" (serri_addr) : );
		pr_crit("[Bank 0] ctrl:0x%016lx status:0x%016lx addr:0x%016lx\n",
			serri_ctrl, serri_status, serri_addr);
		break;
	default:
		if(!cap)
			pr_crit("no MCA banks implemented\n");
		else
			pr_crit("unknown MCA bank configuration\n");
		break;
	}

	return;
}
#endif /* CONFIG_DENVER_CPU */

/*
 * bad_mode handles the impossible case in the exception vector.
 */
asmlinkage void bad_mode(struct pt_regs *regs, int reason, unsigned int esr)
{
	siginfo_t info;
#ifdef CONFIG_DENVER_CPU
	unsigned long serr_status;
#endif
	void __user *pc = (void __user *)instruction_pointer(regs);
	console_verbose();

#ifdef CONFIG_DENVER_CPU
	/* check for MCA assert */
	asm volatile("mrs %0, s3_0_c15_c3_1" : "=r" (serr_status));
	if(serr_status & 4)
	{
		serr_status = 0;
		asm volatile("msr s3_0_c15_c3_1, %0" : : "r" (serr_status));
		dump_mca_debug();
	}
#endif

	pr_crit("Bad mode in %s handler detected, code 0x%08x\n",
		handler[reason], esr);
	__show_regs(regs);

	info.si_signo = SIGILL;
	info.si_errno = 0;
	info.si_code  = ILL_ILLOPC;
	info.si_addr  = pc;

	arm64_notify_die("Oops - bad mode", regs, &info, 0);
}

void __pte_error(const char *file, int line, unsigned long val)
{
	printk("%s:%d: bad pte %016lx.\n", file, line, val);
}

void __pmd_error(const char *file, int line, unsigned long val)
{
	printk("%s:%d: bad pmd %016lx.\n", file, line, val);
}

void __pgd_error(const char *file, int line, unsigned long val)
{
	printk("%s:%d: bad pgd %016lx.\n", file, line, val);
}

void __init trap_init(void)
{
	return;
}
