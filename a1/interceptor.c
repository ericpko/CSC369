#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/current.h>
#include <asm/ptrace.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <asm/unistd.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/syscalls.h>
#include "interceptor.h"

MODULE_DESCRIPTION("Syscall hijacking");
MODULE_AUTHOR("Eric Koehli");
MODULE_LICENSE("GPL");

// ------------------- Helper function declarations -------------------------
int handle_stop_monitoring(int syscall, int pid);
int handle_start_monitoring(int syscall, int pid);
int handle_sysc_release(int syscall);
int handle_sysc_intercept(int syscall);

//----- System Call Table Stuff ------------------------------------
/* Symbol that allows access to the kernel system call table */
extern void *sys_call_table[];
/*  Note: when a userspace program makes a syscall, it will be placed in
    the %eax register (reg.ax), so the kernel can look in that register for the
    requested syscall by the user. After the user program puts the syscall
    number into %eax (and fills the other registers as needed), then the
    user process calls the software interupt 'int 0x80'.
    After each syscall, an integer is returned in %eax.
*/

/* The sys_call_table is read-only => must make it RW before replacing a syscall */
void set_addr_rw(unsigned long addr)
{

    unsigned int level;
    pte_t *pte = lookup_address(addr, &level); // pte = page table entry

    if (pte->pte & ~_PAGE_RW)
        pte->pte |= _PAGE_RW;
}

/* Restores the sys_call_table as read-only */
void set_addr_ro(unsigned long addr)
{

    unsigned int level;
    pte_t *pte = lookup_address(addr, &level);

    pte->pte = pte->pte & ~_PAGE_RW;
}
//-------------------------------------------------------------

//----- Data structures and bookkeeping -----------------------
/**
 * This block contains the data structures needed for keeping track of
 * intercepted system calls (including their original calls), pid monitoring
 * synchronization on shared data, etc.
 * It's highly unlikely that you will need any globals other than these.
 */

/* List structure - each intercepted syscall may have a list of monitored pids */
struct pid_list
{
    pid_t pid;
    struct list_head list;
};

/* Store info about intercepted/replaced system calls */
typedef struct
{
    /* Original system call */
    asmlinkage long (*f)(struct pt_regs);

    /* Status: 1=intercepted, 0=not intercepted */
    int intercepted;

    /* Are any PIDs being monitored for this syscall? */
    int monitored;

    /* List of monitored PIDs */
    int listcount;
    struct list_head my_list;
} MyTable;

/* An entry for each system call in this "metadata" table */
MyTable table[NR_syscalls];

/* Access to the system call table and your metadata table must be synchronized */
spinlock_t my_table_lock = SPIN_LOCK_UNLOCKED;
spinlock_t sys_call_table_lock = SPIN_LOCK_UNLOCKED;
//-------------------------------------------------------------

//----------LIST OPERATIONS------------------------------------
/**
 * These operations are meant for manipulating the list of pids
 * Nothing to do here, but please make sure to read over these functions
 * to understand their purpose, as you will need to use them!
 */

/**
 * Add a pid to a syscall's list of monitored pids.
 * Returns -ENOMEM if the operation is unsuccessful.
 */
static int add_pid_sysc(pid_t pid, int sysc)
{
    // kmalloc space for a single   struct pid_list
    struct pid_list *ple = (struct pid_list *)kmalloc(sizeof(struct pid_list), GFP_KERNEL);
    /*
        GFP = Get Free Pages = __get_free_pages.
        These flags are flags passed to functions that allocate memory,
        such as __get_free_pages and kmalloc, telling them what can and
        can't be done while allocating. For example, GFP_ATOMIC means
        no context-switch must happen while allocating
        (which means paging isn't possible).
    */


    if (!ple)
        return -ENOMEM; // out of memory

    INIT_LIST_HEAD(&ple->list); // &ple->list: pointer to struct list_head
    ple->pid = pid;

    // list_add(new, head); adds a pointer to new from my_list
    list_add(&ple->list, &(table[sysc].my_list));
    table[sysc].listcount++;

    return 0;
}

/**
 * Remove a pid from a system call's list of monitored pids.
 * Returns -EINVAL if no such pid was found in the list.
 */
static int del_pid_sysc(pid_t pid, int sysc)
{
    struct list_head *i;
    struct pid_list *ple;

    list_for_each(i, &(table[sysc].my_list))
    {

        ple = list_entry(i, struct pid_list, list);
        if (ple->pid == pid) {

            list_del(i);
            kfree(ple);

            table[sysc].listcount--;
            /* If there are no more pids in sysc's list of pids, then
			 * stop the monitoring only if it's not for all pids (monitored=2) */
            if (table[sysc].listcount == 0 && table[sysc].monitored == 1) {
                table[sysc].monitored = 0;
            }

            return 0;
        }
    }

    // shouldn't get here
    return -EINVAL;
}

/**
 * Remove a pid from all the lists of monitored pids (for all intercepted syscalls).
 * Returns -1 if this process is not being monitored in any list.
 */
static int del_pid(pid_t pid)
{
    struct list_head *i, *n;
    struct pid_list *ple;
    int ispid = 0, s = 0;

    for (s = 1; s < NR_syscalls; s++) {

        list_for_each_safe(i, n, &(table[s].my_list)) {

            ple = list_entry(i, struct pid_list, list);

            if (ple->pid == pid) {

                list_del(i);
                ispid = 1;
                kfree(ple);

                table[s].listcount--;
                /* If there are no more pids in sysc's list of pids, then
				 * stop the monitoring only if it's not for all pids (monitored=2) */
                if (table[s].listcount == 0 && table[s].monitored == 1) {
                    table[s].monitored = 0;
                }
            }
        }
    }

    if (ispid)
        return 0;
    return -1;
}

/**
 * Clear the list of monitored pids for a specific syscall.
 */
static void destroy_list(int sysc)
{

    struct list_head *i, *n;
    struct pid_list *ple;

    list_for_each_safe(i, n, &(table[sysc].my_list))
    {

        ple = list_entry(i, struct pid_list, list);
        list_del(i);
        kfree(ple);
    }

    table[sysc].listcount = 0;
    table[sysc].monitored = 0;
}

/**
 * Check if two pids have the same owner - useful for checking if a pid
 * requested to be monitored is owned by the requesting process.
 * Remember that when requesting to start monitoring for a pid, only the
 * owner of that pid is allowed to request that.
 */
static int check_pids_same_owner(pid_t pid1, pid_t pid2)
{

    struct task_struct *p1 = pid_task(find_vpid(pid1), PIDTYPE_PID);
    struct task_struct *p2 = pid_task(find_vpid(pid2), PIDTYPE_PID);
    if (p1->real_cred->uid != p2->real_cred->uid)
        return -EPERM;
    return 0;
}

/**
 * Check if a pid is already being monitored for a specific syscall.
 * Returns 1 if it already is, or 0 if pid is not in sysc's list.
 */
static int check_pid_monitored(int sysc, pid_t pid)
{

    struct list_head *i;
    struct pid_list *ple;

    list_for_each(i, &(table[sysc].my_list))
    {

        ple = list_entry(i, struct pid_list, list);
        if (ple->pid == pid)
            return 1;
    }
    return 0;
}
//----------------------------------------------------------------

//----- Intercepting exit_group ----------------------------------
/**
 * Since a process can exit without its owner specifically requesting
 * to stop monitoring it, we must intercept the exit_group system call
 * so that we can remove the exiting process's pid from *all* syscall lists.
 */

/**
 * Stores original exit_group function - after all, we must restore it
 * when our kernel module exits.
 */
asmlinkage long (*orig_exit_group)(struct pt_regs reg);

/**
 * Our custom exit_group system call.
 *
 * TODO: When a process exits, make sure to remove that pid from all lists.
 * The exiting process's PID can be retrieved using the current variable (current->pid).
 * Don't forget to call the original exit_group.
 *
 * Note: using printk in this function will potentially result in errors!
 *
 */
asmlinkage long my_exit_group(struct pt_regs reg)
{
    // lock access to modify MyTable
    spin_lock(&my_table_lock);

    // ---- Critical Section ----
    // remove the exiting process PID from all kernel module lists
    del_pid(current->pid);

    // unlock MyTable access
    spin_unlock(&my_table_lock);

    // call the original exit_group function and return
    orig_exit_group(reg);
    return 0;
}
//----------------------------------------------------------------

/**
 * This is the generic interceptor function.
 * It should just log a message and call the original syscall.
 *
 * TODO: Implement this function.
 * - Check first to see if the syscall is being monitored for the current->pid.
 * - Recall the convention for the "monitored" flag in the MyTable struct:
 *     monitored=0 => not monitored
 *     monitored=1 => some pids are monitored, check the corresponding my_list
 *     monitored=2 => all pids are monitored for this syscall
 * - Use the log_message macro, to log the system call parameters!
 *     Remember that the parameters are passed in the pt_regs registers.
 *     The syscall parameters are found (in order) in the
 *     ax, bx, cx, dx, si, di, and bp registers (see the pt_regs struct).
 * - Don't forget to call the original system call, so we allow processes to proceed as normal.
 */
asmlinkage long interceptor(struct pt_regs reg)
{
    int monitored;
    MyTable syscall_info;

    // set lock for critical section
    spin_lock(&my_table_lock);

    syscall_info = table[reg.ax];
    monitored = 0;

    // check current pid
    if (syscall_info.monitored == 1) {
        // then check if current pid is being monitored for this syscall
        monitored = check_pid_monitored(reg.ax, current->pid);

    } else if (syscall_info.monitored == 2) {
        // then all pid's are being monitored
        monitored = 1;
    }
    // if the syscall is being monitored for current->pid, then log message
    if (monitored) {
        log_message(current->pid, reg.ax, reg.bx, reg.cx, reg.dx, reg.si, reg.di, reg.bp);
    }
    // unlock critical section
    spin_unlock(&my_table_lock);

    // call the original system call, then return
    syscall_info.f(reg);
    return 0;
}


// ------------------------ Helper functions ------------------------

/*
 *
 */
int handle_stop_monitoring(int syscall, int pid) {

    // check monitoring a PID that is not being monitored and if syscall
    // has not been intercepted
    spin_lock(&my_table_lock);
    if ((check_pid_monitored(syscall, pid) == 0 || table[syscall].intercepted == 0)) {
        spin_unlock(&my_table_lock);
        return -EINVAL;
    }

    if (pid == 0) {
        // stop monitoring all PIDs
        table[syscall].monitored = 0;
        destroy_list(syscall);

    } else {
        // remove PID from this syscall's list of monotored PIDs
        del_pid_sysc(pid, syscall);

        // check if the listcount is at zero
        if (table[syscall].listcount == 0) {
            table[syscall].monitored = 0;
        } else {
            table[syscall].monitored = 1;
        }

    }
    spin_unlock(&my_table_lock);
    return 0;
}

/*
 *
 */
int handle_start_monitoring(int syscall, int pid) {

    spin_lock(&my_table_lock);

    // cannot monitor a pid that is already being monitored
    if (table[syscall].monitored == 2 || (table[syscall].monitored == 1 &&
    check_pid_monitored(syscall, pid))) {
        spin_unlock(&my_table_lock);
        return -EBUSY;
    }

    // add PID to the monitoring list for this syscall
    if (add_pid_sysc(pid, syscall) != 0) {
        spin_unlock(&my_table_lock);
        return -ENOMEM;
    }

    if (pid == 0) {
        table[syscall].monitored = 2;
    } else {
        table[syscall].monitored = 1;
    }

    spin_unlock(&my_table_lock);
    return 0;
}

/*
 *
 */
int handle_sysc_release(int syscall) {

    // lock my table before modifying
    spin_lock(&my_table_lock);

    if (table[syscall].intercepted == 0) {
        spin_unlock(&my_table_lock);
        return -EINVAL;
    }

    table[syscall].monitored = 0;
    table[syscall].intercepted = 0;

    // clear the list of monitored pids for this specific syscall
    destroy_list(syscall);                      // modifies table

    // lock syscall table before modifying
    spin_lock(&sys_call_table_lock);
    set_addr_rw((unsigned long)sys_call_table);

    // restore original system call from table
    sys_call_table[syscall] = table[syscall].f;
    spin_unlock(&my_table_lock);
    set_addr_ro((unsigned long)sys_call_table);
    spin_unlock(&sys_call_table_lock);
    return 0;
}

/*
 *
 */
int handle_sysc_intercept(int syscall) {

    spin_lock(&my_table_lock);
    if (table[syscall].intercepted == 1) {
        spin_unlock(&my_table_lock);
        return -EBUSY;
    }

    // save the original system call
    spin_lock(&sys_call_table_lock);
    table[syscall].f = sys_call_table[syscall];
    table[syscall].intercepted = 1;

    // finished modifying table
    spin_unlock(&my_table_lock);

    // set the real syscall handler to our interceptor function
    set_addr_rw((unsigned long) sys_call_table);
    sys_call_table[syscall] = interceptor;
    set_addr_ro((unsigned long) sys_call_table);

    spin_unlock(&sys_call_table_lock);
    return 0;
}



/**
 * My system call - this function is called whenever a user issues a MY_CUSTOM_SYSCALL system call.
 * When that happens, the parameters for this system call indicate one of 4 actions/commands:
 *      - REQUEST_SYSCALL_INTERCEPT to intercept the 'syscall' argument
 *      - REQUEST_SYSCALL_RELEASE to de-intercept the 'syscall' argument
 *      - REQUEST_START_MONITORING to start monitoring for 'pid' whenever it issues 'syscall'
 *      - REQUEST_STOP_MONITORING to stop monitoring for 'pid'
 *      For the last two, if pid=0, that translates to "all pids".
 *
 * TODO: Implement this function, to handle all 4 commands correctly.
 *
 * - For each of the commands, check that the arguments are valid (-EINVAL):
 *   a) the syscall must be valid (not negative, not > NR_syscalls-1, and not MY_CUSTOM_SYSCALL itself)
 *   b) the pid must be valid for the last two commands. It cannot be a negative integer,
 *      and it must be an existing pid (except for the case when it's 0, indicating that we want
 *      to start/stop monitoring for "all pids").
 *      If a pid belongs to a valid process, then the following expression is non-NULL:
 *           pid_task(find_vpid(pid), PIDTYPE_PID)
 * - Check that the caller has the right permissions (-EPERM)
 *      For the first two commands, we must be root (see the current_uid() macro).
 *      For the last two commands, the following logic applies:
 *        - is the calling process root? if so, all is good, no doubts about permissions.
 *        - if not, then check if the 'pid' requested is owned by the calling process
 *        - also, if 'pid' is 0 and the calling process is not root, then access is denied
 *          (monitoring all pids is allowed only for root, obviously).
 *      To determine if two pids have the same owner, use the helper function provided above in this file.
 * - Check for correct context of commands (-EINVAL):
 *     a) Cannot de-intercept a system call that has not been intercepted yet.
 *     b) Cannot stop monitoring for a pid that is not being monitored, or if the
 *        system call has not been intercepted yet.
 * - Check for -EBUSY conditions:
 *     a) If intercepting a system call that is already intercepted.
 *     b) If monitoring a pid that is already being monitored.
 * - If a pid cannot be added to a monitored list, due to no memory being available,
 *   an -ENOMEM error code should be returned.
 *
 *   NOTE: The order of the checks may affect the tester, in case of several error conditions
 *   in the same system call, so please be careful!
 *
 * - Make sure to keep track of all the metadata on what is being intercepted and monitored.
 *   Use the helper functions provided above for dealing with list operations.
 *
 * - Whenever altering the sys_call_table, make sure to use the set_addr_rw/set_addr_ro functions
 *   to make the system call table writable, then set it back to read-only.
 *   For example: set_addr_rw((unsigned long)sys_call_table);
 *   Also, make sure to save the original system call (you'll need it for 'interceptor' to work correctly).
 *
 * - Make sure to use synchronization to ensure consistency of shared data structures.
 *   Use the sys_call_table_lock and my_table_lock to ensure mutual exclusion for accesses
 *   to the system call table and the lists of monitored pids. Be careful to unlock any spinlocks
 *   you might be holding, before you exit the function (including error cases!).
 */
asmlinkage long my_syscall(int cmd, int syscall, int pid)
{
    // ------------------ Phase 1: Error checking ----------------------

    // -------------- check that the arguments are valid (-EINVAL) --------------
    // check cmd
    if (cmd != REQUEST_SYSCALL_INTERCEPT) {
        if (cmd != REQUEST_SYSCALL_RELEASE) {
            if (cmd != REQUEST_START_MONITORING) {
                if (cmd != REQUEST_STOP_MONITORING) {
                    return -EINVAL;
                }
            }
        }
    }
    // check (a)
    if ((syscall < 0) || (syscall > NR_syscalls - 1) || (syscall == MY_CUSTOM_SYSCALL)) {
        return -EINVAL;
    }
    // check (b)
    if ((cmd == REQUEST_START_MONITORING) || (cmd == REQUEST_STOP_MONITORING)) {
		if (pid < 0){
			return -EINVAL;

		} else if (pid > 0) {
			if (pid_task(find_vpid(pid), PIDTYPE_PID) == NULL) {
				return -EINVAL;
		    }
        }
    }

    // -------------- Check that the caller has the right permissions (-EPERM) --------------
    // check if caller has correct permissions for first two requests
	if ((cmd == REQUEST_SYSCALL_INTERCEPT) || (cmd ==  REQUEST_SYSCALL_RELEASE)) {
		if (current_uid() != 0) {
            // then we're not root
			return -EPERM;
		}
	}
    // check if caller has correct permissions for last two requests
    if ((cmd == REQUEST_START_MONITORING) || (cmd == REQUEST_STOP_MONITORING)) {
        if (current_uid() != 0) {
            // check if the 'pid' requested is owned by the calling process or is zero
            if (check_pids_same_owner(current->pid, pid) != 0 || pid == 0) {
                return -EPERM;
            }
        }
    }

    // -------------- Check for correct context of commands (-EINVAL) --------------

    // accessing table, so lock, but unlock before error return
    spin_lock(&my_table_lock);

    // a) Cannot de-intercept a system call that has not been intercepted yet
    if (cmd == REQUEST_SYSCALL_RELEASE) {
		if (table[syscall].intercepted == 0) {
            spin_unlock(&my_table_lock);
			return -EINVAL;
		}
	}

    // b) Cannot stop monitoring for a pid that is not being monitored, or if the
    // system call has not been intercepted yet
	if (cmd == REQUEST_STOP_MONITORING) {
		if (table[syscall].intercepted == 0) {
            // can't stop monitoring unintercepted syscall
            spin_unlock(&my_table_lock);
			return -EINVAL;

        } else if (table[syscall].monitored == 0) {
            // then there are no pids being monitored for this syscall
            spin_unlock(&my_table_lock);
			return -EINVAL;

        } else if (check_pid_monitored(syscall, pid) == 0) {
            // then there are pids being monitored, but this pid is not in sysc's list
            spin_unlock(&my_table_lock);
			return -EINVAL;
		}
    }

    // -------------- Check for -EBUSY conditions --------------
    // a) If intercepting a system call that is already intercepted
	if (cmd == REQUEST_SYSCALL_INTERCEPT) {
		if (table[syscall].intercepted == 1) {
            // syscall has already been intercepted
            spin_unlock(&my_table_lock);
			return -EBUSY;
		}
	}

    // b) If monitoring a pid that is already being monitored
    if (cmd == REQUEST_START_MONITORING){
		if (check_pid_monitored(syscall, pid) == 1) {
            // this pid is already being monitored by this syscall
            spin_unlock(&my_table_lock);
			return -EBUSY;
		}
	}
    spin_unlock(&my_table_lock);


    // -------------- Phase 2: Implementation --------------
    switch (cmd) {
        case REQUEST_SYSCALL_INTERCEPT:
            handle_sysc_intercept(syscall);
            break;

        case REQUEST_SYSCALL_RELEASE:
            handle_sysc_release(syscall);
            break;

        case REQUEST_START_MONITORING:
            handle_start_monitoring(syscall, pid);
            break;

        case REQUEST_STOP_MONITORING:
            handle_stop_monitoring(syscall, pid);
            break;
    }

    return 0;
}

/**
 *
 */
long (*orig_custom_syscall)(void);

/**
 * Module initialization.
 *
 * TODO: Make sure to:
 * - Hijack MY_CUSTOM_SYSCALL and save the original in orig_custom_syscall.
 * - Hijack the exit_group system call (__NR_exit_group) and save the original
 *   in orig_exit_group.
 * - Make sure to set the system call table to writable when making changes,
 *   then set it back to read only once done.
 * - Perform any necessary initializations for bookkeeping data structures.
 *   To initialize a list, use
 *        INIT_LIST_HEAD (&some_list);
 *   where some_list is a "struct list_head".
 * - Ensure synchronization as needed.
 */
static int init_function(void)
{
    int i;
    // first lock syscall table before writing
	spin_lock(&sys_call_table_lock);

    // set syscall table to RW
	set_addr_rw((unsigned long) sys_call_table);

	// Hijack MY_CUSTOM_SYSCALL and save the original in orig_custom_syscall
    orig_custom_syscall = sys_call_table[MY_CUSTOM_SYSCALL];
	sys_call_table[MY_CUSTOM_SYSCALL] = my_syscall;

    // Hijack the exit_group system call (__NR_exit_group) and save the original
    orig_exit_group = sys_call_table[__NR_exit_group];
    sys_call_table[__NR_exit_group] = my_exit_group;

    // done editing - set syscall table back to read only and unlock
    set_addr_ro((unsigned long) sys_call_table);
    spin_unlock(&sys_call_table_lock);

    // set up bookkeeping data structures on my table
    spin_lock(&my_table_lock);
    // for loop declarations are only allowed in C99 mode

    for (i = 0; i < NR_syscalls; i++) {
		table[i].f = NULL;
		table[i].intercepted = 0;
		table[i].monitored = 0;
		table[i].listcount = 0;
		INIT_LIST_HEAD(&(table[i].my_list));
	}
	spin_unlock(&my_table_lock);

    return 0;
}

/**
 * Module exits.
 *
 * TODO: Make sure to:
 * - Restore MY_CUSTOM_SYSCALL to the original syscall.
 * - Restore __NR_exit_group to its original syscall.
 * - Make sure to set the system call table to writable when making changes,
 *   then set it back to read only once done.
 * - Ensure synchronization, if needed.
 */
static void exit_function(void)
{
    int i;

    // spin_lock sys_call_table before writing and change to RW
	spin_lock(&sys_call_table_lock);
	set_addr_rw((unsigned long)sys_call_table);

	// restore MY_CUSTOM_SYSCALL to the original syscall
	sys_call_table[MY_CUSTOM_SYSCALL] = orig_custom_syscall;

    // restore __NR_exit_group to its original syscall
	sys_call_table[__NR_exit_group] = orig_exit_group;

    // clear table and restore real syscall table
    spin_lock(&my_table_lock);
	for (i = 0; i < NR_syscalls; i++) {
		if (table[i].f != NULL) {
			sys_call_table[i] = table[i].f;
			table[i].f = NULL;
		}
		table[i].intercepted = 0;
		table[i].monitored = 0;
		table[i].listcount = 0;
		destroy_list(i);
	}
    spin_unlock(&my_table_lock);

    // set table back to read only and unlock
	set_addr_ro((unsigned long) sys_call_table);
	spin_unlock(&sys_call_table_lock);
}

module_init(init_function);
module_exit(exit_function);














// ************** old code *****************
// static int init_function(void)
// {

//   orig_custom_syscall = sys_call_table[MY_CUSTOM_SYSCALL];
//   orig_exit_group = sys_call_table[__NR_exit_group];
//    spin_lock(&sys_call_table_lock);
//   set_addr_rw((unsigned long) sys_call_table);

//   sys_call_table[MY_CUSTOM_SYSCALL] = my_syscall;
//   sys_call_table[MY_CUSTOM_SYSCALL] = my_exit_group;

//   set_addr_ro((unsigned long) sys_call_table);
//   spin_unlock(&sys_call_table_lock);

//   spin_lock(&my_table_lock);
//   for (int i = 0; i < NR_syscalls; i ++){
//     table[i].intercepted = 0;
//     table[i].monitored = 0;
//     table[i].listcount = 0;
//     INIT_LIST_HEAD(&table[i].my_list);
//   }
//   spin_unlock(&my_table_lock);

//     return 0;
// }



// static void exit_function(void)
// {
//   spin_lock(&sys_call_table_lock);
//   set_addr_rw((unsigned long) sys_call_table);
//   sys_call_table[MY_CUSTOM_SYSCALL] = orig_custom_syscall;
//   sys_call_table[__NR_exit_group] = orig_exit_group;
//   set_addr_ro((unsigned long) sys_call_table);
//   spin_unlock(&sys_call_table_lock);
// }





// asmlinkage long my_syscall(int cmd, int syscall, int pid)
// {
//   if (syscall < 0 || syscall > NR_syscalls -1 || syscall == MY_CUSTOM_SYSCALL )
//     return -EINVAL;

//   if(cmd == REQUEST_SYSCALL_INTERCEPT){
//     if( pid != current_uid()) return -EPERM;
//     if(table[syscall].intercepted == 1) return -EBUSY;
//     spin_lock(&my_table_lock);
//     // do somehting _intercept
//     spin_unlock(&my_table_lock);
//   } else if( cmd == REQUEST_SYSCALL_RELEASE){
//     if(pid != current_uid()) return -EPERM;
//     if(table[syscall].intercepted == 0) return -EINVAL;
//     spin_lock(&my_table_lock);
//     // do some thing here
//     spin_unlock(&my_table_lock);
//   } else if (cmd == REQUEST_START_MONITORING){
//     if (pid < 0 ||(syscall != 0 && check_pid_monitored(syscall, pid) == 0)) return -EINVAL;
//     if (pid == current_uid()) { //we're  good
//     } else if (check_pids_same_owner(pid, current_uid()) != 0) return -EPERM;
//     if(table[syscall].monitored > 0 && check_pid_monitored(syscall, pid) == 1) return -EBUSY;
//     if (add_pid_sysc(syscall, pid) != 0) return -ENOMEM;
//     spin_lock(&sys_call_table_lock);
//     set_addr_rw((unsigned long) sys_call_table);
//     // do something here - not sure what exactly at the moment
//     set_addr_ro((unsigned long) sys_call_table);
//     spin_unlock(&sys_call_table_lock);
//   } else if (cmd == REQUEST_STOP_MONITORING){
//     if (pid < 0 || (syscall != 0 && check_pid_monitored(syscall, pid) == 0)) return -EINVAL;
//     if (check_pids_same_owner(pid, current_uid()) != 0) return -EPERM;
//     if (check_pid_monitored(syscall, pid) == 0 && table[syscall].intercepted == 1) return -EINVAL;
//     spin_lock(&sys_call_table_lock);
//     set_addr_rw((unsigned long) sys_call_table);
//   // do something here
//     set_addr_ro((unsigned long) sys_call_table);
//     spin_unlock(&sys_call_table_lock);


//   } else {
//   }

//     return 0;
// }
