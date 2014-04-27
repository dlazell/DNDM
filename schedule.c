/* MODIFIED 4-25-14 */

/* This file contains the scheduling policy for SCHED
 *
 * The entry points are:
 *   do_noquantum:        Called on behalf of process' that run out of quantum
 *   do_start_scheduling  Request to start scheduling a proc
 *   do_stop_scheduling   Request to stop scheduling a proc
 *   do_nice              Request to change the nice level on a proc
 *   init_scheduling      Called from main.c to set up/prepare scheduling
 */
#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include "kernel/proc.h" /* for queue constants */

/* CHANGE START */
#define HOLDING_Q       14
                        /* this should be the queue in which processes are in
                           when they have not won the lottery */
#define WINNING_Q       13
                        /* this should be the queue in which processes are in
                        when they HAVE won the lottery */
#define STARTING_TICKETS 20 /* the number of tickets each process starts with */

#define MAX_TICKETS     100 /* the max number of tickets a process can have */

#define MIN_TICKETS     1   /* the min number of tickets a process can have */
/* CHANGE END */

PRIVATE timer_t sched_timer;
PRIVATE unsigned balance_timeout;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */

FORWARD _PROTOTYPE(int schedule_process, (struct schedproc * rmp));
FORWARD _PROTOTYPE(void balance_queues, (struct timer *tp));

/* CHANGE START */
/*===========================================================================*
*              do_lottery                     *
*===========================================================================*/

PUBLIC int do_lottery() {
    register struct schedproc *rmp;
    int rv, proc_nr_n;
    int total_tickets = 0;
    u64_t tsc;
    int winner;

    /* count the total number of tickets in all processes */
    for (proc_nr_n = 0; proc_nr_n < NR_PROCS; ++proc_nr_n) {
        rmp = &schedproc[proc_nr_n];
        if (rmp->priority == HOLDING_Q && rmp->flags == IN_USE) /* can we win? */
            total_tickets += rmp->tickets;
    }
    /* generate a "random" winning ticket */
    read_tsc_64(&tsc);
    winner = tsc.lo % total_tickets + 1;

    /* now find the process with the winning ticket */
    for (proc_nr_n = 0; proc_nr_n < NR_PROCS; ++proc_nr_n) {
        rmp = &schedproc[proc_nr_n];
        if (rmp->priority == HOLDING_Q && rmp->flags == IN_USE) /* can we win? */
            winner -= rmp->tickets;
        if (winner <= 0)
            break;
    }

    printf("Process %d won with %d of %d tickets\n", proc_nr_n, rmp->tickets, total_tickets);
    /* schedule new winning process */
    rmp->priority = WINNING_Q;
    rmp->time_slice = USER_QUANTUM;
    if ((rv = schedule_process(rmp)) != OK) /* move process to winner queue */
        return rv;
    return OK;
}
/* CHANGE END */

/*===========================================================================*
 *              do_noquantum                     *
 *===========================================================================*/

 PUBLIC int do_noquantum(message *m_ptr) {
    register struct schedproc *rmp;
    int rv, proc_nr_n;

    if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
        printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
        m_ptr->m_source);
        return EBADEPT;
    }

    rmp = &schedproc[proc_nr_n];
/* CHANGE START */
    if (rmp->priority == 15)
        printf("Error, process has priority of 15\n");
    if (rmp->priority == WINNING_Q) /* user process */
        rmp->priority = HOLDING_Q;
    else if (rmp->priority < WINNING_Q - 1) /* kernel process */
        rmp->priority++;

    if ((rv = schedule_process(rmp)) != OK) /* move process to holding queue */
        return rv;

    if (rv = do_lottery() != OK) /* schedule a new winner */
        return rv;
/* CHANGE END */

    return OK;
}

/*===========================================================================*
 *              do_stop_scheduling               *
 *===========================================================================*/
PUBLIC int do_stop_scheduling(message *m_ptr)
{
    register struct schedproc *rmp;
    int rv, proc_nr_n;

    /* check who can send you requests */
    if (!accept_message(m_ptr))
        return EPERM;

    if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
        printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
        "%ld\n", m_ptr->SCHEDULING_ENDPOINT);
        return EBADEPT;
    }

    rmp = &schedproc[proc_nr_n];
    rmp->flags = 0; /*&= ~IN_USE;*/

/* CHANGE START */
    /* a process stopped, so we need to schedule a new one */
    if (rv = do_lottery() != OK)
        return rv;
/* CHANGE START */

    return OK;
}

/*===========================================================================*
 *              do_start_scheduling              *
 *===========================================================================*/
PUBLIC int do_start_scheduling(message *m_ptr)
{
    register struct schedproc *rmp;
    int rv, proc_nr_n, parent_nr_n, nice;
    
    printf("Starting process\n");
    /* we can handle two kinds of messages here */
    assert(m_ptr->m_type == SCHEDULING_START || 
        m_ptr->m_type == SCHEDULING_INHERIT);

    /* check who can send you requests */
    if (!accept_message(m_ptr))
        return EPERM;

    /* Resolve endpoint to proc slot. */
    if ((rv = sched_isemtyendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n))
            != OK) {
        return rv;
    }
    rmp = &schedproc[proc_nr_n];

    /* Populate process slot */
    rmp->endpoint     = m_ptr->SCHEDULING_ENDPOINT;
    rmp->parent       = m_ptr->SCHEDULING_PARENT;
    rmp->max_priority = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
/* CHANGE START */
    rmp->tickets      = STARTING_TICKETS;
/* CHANGE END */

    if (rmp->max_priority >= NR_SCHED_QUEUES)
        return EINVAL;
    
    switch (m_ptr->m_type) {
        case SCHEDULING_START:
            /* We have a special case here for system processes, for which
             * quanum and priority are set explicitly rather than inherited 
             * from the parent */
            rmp->priority   = rmp->max_priority;
            rmp->time_slice = (unsigned) m_ptr->SCHEDULING_QUANTUM;
            printf("Starting kernel process %d\n", proc_nr_n);
            break;
        
        case SCHEDULING_INHERIT:
            /* Inherit current priority and time slice from parent. Since there
             * is currently only one scheduler scheduling the whole system, this
             * value is local and we assert that the parent endpoint is valid */
            if ((rv = sched_isokendpt(m_ptr->SCHEDULING_PARENT,
                    &parent_nr_n)) != OK)
                return rv;

/* CHANGE START */
            printf("Starting user process %d\n", proc_nr_n);
            rmp->priority = HOLDING_Q;
            rmp->time_slice = USER_QUANTUM;
/* CHANGE END */
            break;
        
        default: 
            /* not reachable */
            assert(0);
    }

    /* Take over scheduling the process. The kernel reply message populates
     * the processes current priority and its time slice */
    if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0)) != OK) {
        printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
            rmp->endpoint, rv);
        return rv;
    }
    rmp->flags = IN_USE;

    /* Schedule the process, giving it some quantum */
    if ((rv = schedule_process(rmp)) != OK) {
        printf("Sched: Error while scheduling process, kernel replied %d\n",
            rv);
        return rv;
    }
    
    /* Mark ourselves as the new scheduler.
     * By default, processes are scheduled by the parents scheduler. In case
     * this scheduler would want to delegate scheduling to another
     * scheduler, it could do so and then write the endpoint of that
     * scheduler into SCHEDULING_SCHEDULER
     */

    m_ptr->SCHEDULING_SCHEDULER = SCHED_PROC_NR;

    return OK;
}

/*===========================================================================*
 *              do_nice                      *
 *===========================================================================*/
PUBLIC int do_nice(message *m_ptr)
{
    struct schedproc *rmp;
    int rv;
    int proc_nr_n;
    unsigned new_q, old_q, old_max_q;
/* CHANGE START */
    int new_tickets;
/* CHANGE END */

    /* check who can send you requests */
    if (!accept_message(m_ptr))
        return EPERM;

    if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
        printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
        "%ld\n", m_ptr->SCHEDULING_ENDPOINT);
        return EBADEPT;
    }

    rmp = &schedproc[proc_nr_n];
    new_q = (unsigned) m_ptr->SCHEDULING_MAXPRIO;

/* CHANGE START */
    new_tickets = rmp->tickets + new_q;
    if (new_tickets > MAX_TICKETS)
        rmp->tickets = MAX_TICKETS;
    if (new_tickets < MIN_TICKETS)
        rmp->tickets = MIN_TICKETS;

    return rv;
/* CHANGE END */

    if (new_q >= NR_SCHED_QUEUES)
        return EINVAL;
 
    /* Store old values, in case we need to roll back the changes */
    old_q     = rmp->priority;
    old_max_q = rmp->max_priority;

    /* Update the proc entry and reschedule the process */
    rmp->max_priority = rmp->priority = new_q;

    if ((rv = schedule_process(rmp)) != OK) {
        /* Something went wrong when rescheduling the process, roll
         * back the changes to proc struct */
        rmp->priority     = old_q;
        rmp->max_priority = old_max_q;
    }

    return rv;
}

/*===========================================================================*
 *              schedule_process                 *
 *===========================================================================*/
PRIVATE int schedule_process(struct schedproc * rmp)
{
    int rv;

    if ((rv = sys_schedule(rmp->endpoint, rmp->priority,
            rmp->time_slice)) != OK) {
        printf("SCHED: An error occurred when trying to schedule %d: %d\n",
        rmp->endpoint, rv);
    }

    return rv;
}

/*===========================================================================*
*				start_scheduling			     *
*===========================================================================*/

PUBLIC void init_scheduling(void) {
    balance_timeout = BALANCE_TIMEOUT * sys_hz();
    init_timer(&sched_timer);
    set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}

/*===========================================================================*
*				balance_queues				     *
*===========================================================================*/

/* This function in called every 100 ticks to rebalance the queues. The current
* scheduler bumps processes down one priority when ever they run out of
* quantum. This function will find all proccesses that have been bumped down,
* and pulls them back up. This default policy will soon be changed.
*/
PRIVATE void balance_queues(struct timer *tp) {
    struct schedproc *rmp;
    int proc_nr;
    int rv;

    for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
        if (rmp->flags & IN_USE) {
/* CHANGE START */
            /* we only want to change kernel processes */
            if (rmp->priority > rmp->max_priority && rmp->priority < WINNING_Q) {
/* CHANGE END */
                rmp->priority -= 1; /* increase priority */
                schedule_process(rmp);
            }
        }
    }

    set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}
