/*
 * kernel/power/suspend.c - Suspend to RAM and standby functionality.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 * Copyright (c) 2009 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 *
 * This file is released under the GPLv2.
 */

#include <linux/string.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/cpu.h>
#include <linux/syscalls.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/ftrace.h>
#include <linux/rtc.h>
#include <trace/events/power.h>

#include "power.h"

//[+++]Debug for active wakelock before entering suspend
#include <linux/wakelock.h>
int pmsp_flag = 0;
bool g_resume_status;
int pm_stay_unattended_period = 0;
//[---]Debug for active wakelock before entering suspend
const char *const pm_states[PM_SUSPEND_MAX] = {
	[PM_SUSPEND_ON] = "on",
	[PM_SUSPEND_FREEZE]	= "freeze",
	[PM_SUSPEND_STANDBY]	= "standby",
	[PM_SUSPEND_MEM]	= "mem",
};

static const struct platform_suspend_ops *suspend_ops;

static bool need_suspend_ops(suspend_state_t state)
{
	return !!(state > PM_SUSPEND_FREEZE);
}

static DECLARE_WAIT_QUEUE_HEAD(suspend_freeze_wait_head);
static bool suspend_freeze_wake;

static void freeze_begin(void)
{
	suspend_freeze_wake = false;
}

static void freeze_enter(void)
{
	wait_event(suspend_freeze_wait_head, suspend_freeze_wake);
}

void freeze_wake(void)
{
	suspend_freeze_wake = true;
	wake_up(&suspend_freeze_wait_head);
}
EXPORT_SYMBOL_GPL(freeze_wake);

/**
 * suspend_set_ops - Set the global suspend method table.
 * @ops: Suspend operations to use.
 */
void suspend_set_ops(const struct platform_suspend_ops *ops)
{
	lock_system_sleep();
	suspend_ops = ops;
	unlock_system_sleep();
}
EXPORT_SYMBOL_GPL(suspend_set_ops);

bool valid_state(suspend_state_t state)
{
	if (state == PM_SUSPEND_FREEZE) {
#ifdef CONFIG_PM_DEBUG
		if (pm_test_level != TEST_NONE &&
		    pm_test_level != TEST_FREEZER &&
		    pm_test_level != TEST_DEVICES &&
		    pm_test_level != TEST_PLATFORM) {
			printk(KERN_WARNING "Unsupported pm_test mode for "
					"freeze state, please choose "
					"none/freezer/devices/platform.\n");
			return false;
		}
#endif
			return true;
	}
	/*
	 * PM_SUSPEND_STANDBY and PM_SUSPEND_MEMORY states need lowlevel
	 * support and need to be valid to the lowlevel
	 * implementation, no valid callback implies that none are valid.
	 */
	return suspend_ops && suspend_ops->valid && suspend_ops->valid(state);
}

/**
 * suspend_valid_only_mem - Generic memory-only valid callback.
 *
 * Platform drivers that implement mem suspend only and only need to check for
 * that in their .valid() callback can use this instead of rolling their own
 * .valid() callback.
 */
int suspend_valid_only_mem(suspend_state_t state)
{
	return state == PM_SUSPEND_MEM;
}
EXPORT_SYMBOL_GPL(suspend_valid_only_mem);

static int suspend_test(int level)
{
#ifdef CONFIG_PM_DEBUG
	if (pm_test_level == level) {
		printk(KERN_INFO "suspend debug: Waiting for 5 seconds.\n");
		mdelay(5000);
		return 1;
	}
#endif /* !CONFIG_PM_DEBUG */
	return 0;
}

extern int g_keycheck_abort;	//ASUS BSP Austin_T

/**
 * suspend_prepare - Prepare for entering system sleep state.
 *
 * Common code run for every system sleep state that can be entered (except for
 * hibernation).  Run suspend notifiers, allocate the "suspend" console and
 * freeze processes.
 */
static int suspend_prepare(suspend_state_t state)
{
	int error;

	if (need_suspend_ops(state) && (!suspend_ops || !suspend_ops->enter))
		return -EPERM;

	pm_prepare_console();

	error = pm_notifier_call_chain(PM_SUSPEND_PREPARE);
	if (error)
		goto Finish;

	g_keycheck_abort = 0;	//ASUS BSP Austin_T

	error = suspend_freeze_processes();
	if (!error)
		return 0;

	suspend_stats.failed_freeze++;
	dpm_save_failed_step(SUSPEND_FREEZE);
 Finish:
	pm_notifier_call_chain(PM_POST_SUSPEND);
	pm_restore_console();
	return error;
}

/* default implementation */
void __attribute__ ((weak)) arch_suspend_disable_irqs(void)
{
	local_irq_disable();
}

/* default implementation */
void __attribute__ ((weak)) arch_suspend_enable_irqs(void)
{
	local_irq_enable();
}

/**
 * suspend_enter - Make the system enter the given sleep state.
 * @state: System sleep state to enter.
 * @wakeup: Returns information that the sleep state should not be re-entered.
 *
 * This function should be called after devices have been suspended.
 */
static int suspend_enter(suspend_state_t state, bool *wakeup)
{
	int error;

	if (need_suspend_ops(state) && suspend_ops->prepare) {
		error = suspend_ops->prepare();
		if (error)
			goto Platform_finish;
	}

	error = dpm_suspend_end(PMSG_SUSPEND);
	if (error) {
		printk(KERN_ERR "[PM] Some devices failed to power down\n");
		goto Platform_finish;
	}

	if (need_suspend_ops(state) && suspend_ops->prepare_late) {
		error = suspend_ops->prepare_late();
		if (error)
			goto Platform_wake;
	}

	if (suspend_test(TEST_PLATFORM))
		goto Platform_wake;

	/*
	 * PM_SUSPEND_FREEZE equals
	 * frozen processes + suspended devices + idle processors.
	 * Thus we should invoke freeze_enter() soon after
	 * all the devices are suspended.
	 */
	if (state == PM_SUSPEND_FREEZE) {
		freeze_enter();
		goto Platform_wake;
	}

	error = disable_nonboot_cpus();
	if (error || suspend_test(TEST_CPUS))
		goto Enable_cpus;

	arch_suspend_disable_irqs();
	BUG_ON(!irqs_disabled());

	error = syscore_suspend();
	if (!error) {
		*wakeup = pm_wakeup_pending();
		if (!(suspend_test(TEST_CORE) || *wakeup)) {
			error = suspend_ops->enter(state);
			events_check_enabled = false;
		}
		syscore_resume();
	}

	arch_suspend_enable_irqs();
	BUG_ON(irqs_disabled());

 Enable_cpus:
	enable_nonboot_cpus();

 Platform_wake:
	if (need_suspend_ops(state) && suspend_ops->wake)
		suspend_ops->wake();

	dpm_resume_start(PMSG_RESUME);

 Platform_finish:
	if (need_suspend_ops(state) && suspend_ops->finish)
		suspend_ops->finish();

	return error;
}

//[+++]Debug for active wakelock before entering suspend
extern void print_active_locks(void);
void unattended_timer_expired(unsigned long data);
DEFINE_TIMER(unattended_timer, unattended_timer_expired, 0, 0);

void unattended_timer_expired(unsigned long data)
{
	pr_info("[PM]unattended_timer_expired\n");
    ASUSEvtlog("[PM]unattended_timer_expired\n");
    pmsp_flag=1;
	//for dump cpuinfo purpose, it needs 30mins to timeout
	pm_stay_unattended_period += PM_UNATTENDED_TIMEOUT;
    print_active_locks();
	mod_timer(&unattended_timer, jiffies + msecs_to_jiffies(PM_UNATTENDED_TIMEOUT));
}
//[---]Debug for active wakelock before entering suspend
/**
 * suspend_devices_and_enter - Suspend devices and enter system sleep state.
 * @state: System sleep state to enter.
 */
int suspend_devices_and_enter(suspend_state_t state)
{
	int error;
	bool wakeup = false;

	if (need_suspend_ops(state) && !suspend_ops)
		return -ENOSYS;

	trace_machine_suspend(state);
	if (need_suspend_ops(state) && suspend_ops->begin) {
		error = suspend_ops->begin(state);
		if (error)
			goto Close;
	}
	//[+++]Debug for active wakelock before entering suspend
    pr_info("[PM]unattended_timer: del_timer\n");
    del_timer ( &unattended_timer );
	//reset period
	pm_stay_unattended_period = 0;
	//[---]Debug for active wakelock before entering suspend
	suspend_console();
	ftrace_stop();
	suspend_test_start();
	error = dpm_suspend_start(PMSG_SUSPEND);
	if (error) {
		printk(KERN_ERR "[PM] suspend_devices: Some devices failed to suspend\n");
		goto Recover_platform;
	}
	suspend_test_finish("suspend devices");
	if (suspend_test(TEST_DEVICES))
		goto Recover_platform;

	do {
		error = suspend_enter(state, &wakeup);
	} while (!error && !wakeup && need_suspend_ops(state)
		&& suspend_ops->suspend_again && suspend_ops->suspend_again());

	pm_pwrcs_ret = 1;//ASUS_BSP [Power] jeff_gu Add for wakeup debug

 Resume_devices:
	suspend_test_start();
	dpm_resume_end(PMSG_RESUME);
	suspend_test_finish("resume devices");
	ftrace_start();
	resume_console();
	//[+++]Debug for active wakelock before entering suspend
    pr_info("[PM]unattended_timer: mod_timer\n");
    mod_timer(&unattended_timer, jiffies + msecs_to_jiffies(PM_UNATTENDED_TIMEOUT));
    g_resume_status = true;
	//[---]Debug for active wakelock before entering suspend
 Close:
	if (need_suspend_ops(state) && suspend_ops->end)
		suspend_ops->end();
	trace_machine_suspend(PWR_EVENT_EXIT);
	return error;

 Recover_platform:
	if (need_suspend_ops(state) && suspend_ops->recover)
		suspend_ops->recover();
	goto Resume_devices;
}

/**
 * suspend_finish - Clean up before finishing the suspend sequence.
 *
 * Call platform code to clean up, restart processes, and free the console that
 * we've allocated. This routine is not called for hibernation.
 */
static void suspend_finish(void)
{
	suspend_thaw_processes();
	pm_notifier_call_chain(PM_POST_SUSPEND);
	pm_restore_console();
}

/**
 * enter_state - Do common work needed to enter system sleep state.
 * @state: System sleep state to enter.
 *
 * Make sure that no one else is trying to put the system into a sleep state.
 * Fail if that's not the case.  Otherwise, prepare for system suspend, make the
 * system enter the given sleep state and clean up after wakeup.
 */
static int enter_state(suspend_state_t state)
{
	int error;

	if (!valid_state(state))
		return -ENODEV;

	if (!mutex_trylock(&pm_mutex))
		return -EBUSY;

	if (state == PM_SUSPEND_FREEZE)
		freeze_begin();

	printk(KERN_INFO "[PM] enter_state: Syncing filesystems ...\n");
	sys_sync();
	printk("[PM] Syncing done.\n");

	printk("[PM] enter_state: Preparing system for %s sleep\n", pm_states[state]);
	error = suspend_prepare(state);
	if (error)
		goto Unlock;

	if (suspend_test(TEST_FREEZER))
		goto Finish;

	printk("[PM] enter_state: suspend devices, entering %s sleep\n", pm_states[state]);
	pm_restrict_gfp_mask();
	error = suspend_devices_and_enter(state);
	pm_restore_gfp_mask();

 Finish:
	printk("[PM] enter_state: Finishing wakeup.\n");
	suspend_finish();
 Unlock:
	mutex_unlock(&pm_mutex);
	return error;
}

static void pm_suspend_marker(char *annotation)
{
	struct timespec ts;
	struct rtc_time tm;

	getnstimeofday(&ts);
	rtc_time_to_tm(ts.tv_sec, &tm);
	pr_info("[PM] marker: suspend %s %d-%02d-%02d %02d:%02d:%02d.%09lu UTC\n",
		annotation, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
}

//ASUS_BSP+++ Landice "[ZE500KL][USBH][Spec] Register early suspend notification for none mode switch"
extern void asus_otg_host_power_off(void);
//ASUS_BSP--- Landice "[ZE500KL][USBH][Spec] Register early suspend notification for none mode switch"

/**
 * pm_suspend - Externally visible function for suspending the system.
 * @state: System sleep state to enter.
 *
 * Check if the value of @state represents one of the supported states,
 * execute enter_state() and update system suspend statistics.
 */
int pm_suspend(suspend_state_t state)
{
	int error;

	printk("[PM] ++pm_suspend\n");
	if (state <= PM_SUSPEND_ON || state >= PM_SUSPEND_MAX)
		return -EINVAL;

//ASUS_BSP+++ Landice "[ZE500KL][USBH][Spec] Register early suspend notification for none mode switch"
	asus_otg_host_power_off();
//ASUS_BSP--- Landice "[ZE500KL][USBH][Spec] Register early suspend notification for none mode switch"

	pm_suspend_marker("entry");
	printk("[PM] entering_state: %d\n", state);
	error = enter_state(state);
	if (error) {
		suspend_stats.fail++;
		dpm_save_failed_errno(error);
		printk("[PM] pm_suspend failed, cnt: %d\n", suspend_stats.fail);
	} else {
		suspend_stats.success++;
		printk("[PM] pm_suspend success, cnt: %d\n", suspend_stats.success);
	}
	pm_suspend_marker("exit");
	printk("[PM] --pm_suspend\n");
	return error;
}
EXPORT_SYMBOL(pm_suspend);
