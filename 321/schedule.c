/* MODIFIED 5-1-14 */

/* This file contains the scheduling policy for SCHED
 *
 * The entry points are:
 *   do_noquantum:        Called on behalf of process' that run out of quantum
 *   do_start_scheduling  Request to start scheduling a proc
 *   do_stop_scheduling   Request to stop scheduling a proc
 *   do_nice		  Request to change the nice level on a proc
 *   init_scheduling      Called from main.c to set up/prepare scheduling
 */
#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include "kernel/proc.h" /* for queue constants */

/* CHANGE START */
#define HOLDING_Q       (MIN_USER_Q) /* this should be the queue in which processes are in
                                        when they have not won the lottery */
#define WINNING_Q       (HOLDING_Q - 1) /* this should be the queue in which processes are in
                                           when they HAVE won the lottery */
#define STARTING_TICKETS 20 /* the number of tickets each process starts with */

#define MAX_TICKETS     100 /* the max number of tickets a process can have */

#define MIN_TICKETS     1   /* the min number of tickets a process can have */
/* CHANGE END */

static timer_t sched_timer;
static unsigned balance_timeout;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */

static int schedule_process(struct schedproc * rmp, unsigned flags);
static void balance_queues(struct timer *tp);

#define SCHEDULE_CHANGE_PRIO	0x1
#define SCHEDULE_CHANGE_QUANTUM	0x2
#define SCHEDULE_CHANGE_CPU	0x4

#define SCHEDULE_CHANGE_ALL	(	\
		SCHEDULE_CHANGE_PRIO	|	\
		SCHEDULE_CHANGE_QUANTUM	|	\
		SCHEDULE_CHANGE_CPU		\
		)

#define schedule_process_local(p)	\
	schedule_process(p, SCHEDULE_CHANGE_PRIO | SCHEDULE_CHANGE_QUANTUM)
#define schedule_process_migrate(p)	\
	schedule_process(p, SCHEDULE_CHANGE_CPU)

#define CPU_DEAD	-1

#define cpu_is_available(c)	(cpu_proc[c] >= 0)

#define DEFAULT_USER_TIME_SLICE 200

/* processes created by RS are sysytem processes */
#define is_system_proc(p)	((p)->parent == RS_PROC_NR)

static unsigned cpu_proc[CONFIG_MAX_CPUS];

static void pick_cpu(struct schedproc * proc)
{
#ifdef CONFIG_SMP
	unsigned cpu, c;
	unsigned cpu_load = (unsigned) -1;
	
	if (machine.processors_count == 1) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* schedule sysytem processes only on the boot cpu */
	if (is_system_proc(proc)) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* if no other cpu available, try BSP */
	cpu = machine.bsp_id;
	for (c = 0; c < machine.processors_count; c++) {
		/* skip dead cpus */
		if (!cpu_is_available(c))
			continue;
		if (c != machine.bsp_id && cpu_load > cpu_proc[c]) {
			cpu_load = cpu_proc[c];
			cpu = c;
		}
	}
	proc->cpu = cpu;
	cpu_proc[cpu]++;
#else
	proc->cpu = 0;
#endif
}

/* CHANGE START */

void debug() {
    int q13, q14, q15, qsys, proc_nr_n, q13io, q14io, q15io;
    struct schedproc *rmp;

    q13 = q14 = q15 = qsys = q13io = q14io = q15io = 0;
    for (proc_nr_n = 0, rmp = schedproc; proc_nr_n < NR_PROCS; ++proc_nr_n, ++rmp)
        if (rmp->flags == (IN_USE | USER_PROCESS)) {
            printf("process %d priority %d tickets %d blocked %d times\n", proc_nr_n, rmp->priority, rmp->tickets, rmp->blocking);
            if (rmp->priority == WINNING_Q) {
                q13++;
                if (rmp->blocking)
                    q13io++;
            }
            if (rmp->priority == WINNING_Q + 1) {
                q14++;
                if (rmp->blocking)
                    q14io++;
            }
            if (rmp->priority == HOLDING_Q) {
                q15++;
                if (rmp->blocking)
                    q15io++;
            }
            if (rmp->priority < WINNING_Q)
                qsys++;
        }

    printf("%d(%d) winning, %d(%d) IO, %d(%d) holding, %d system\n", q13, q13io, q14, q14io, q15, q15io, qsys);
}

int count_winners() {
    int winners, proc_nr_n;
    struct schedproc *rmp;

    winners = 0;
    for (proc_nr_n = 0, rmp = schedproc; proc_nr_n < NR_PROCS; ++proc_nr_n, ++rmp)
        if (rmp->flags == (IN_USE | USER_PROCESS))
            if (rmp->priority == WINNING_Q)
                winners++;

    return winners;
}

/*===========================================================================*
*              do_lottery                     *
* pick a winning process randomly from the holding queue                     *
* change the process to the winning queue and give it some quantum           *
*===========================================================================*/

int do_lottery() {
    struct schedproc *rmp;
    int rv, proc_nr;
    int total_tickets = 0;
    u64_t tsc;
    int winner;

    /* count the total number of tickets in all processes */
    /* we really should have a global to keep track of this total */
    /* rather than computing it every time */
    for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; ++proc_nr, ++rmp)
        if (rmp->priority == HOLDING_Q && rmp->flags == (IN_USE | USER_PROCESS)) /* winnable? */
            total_tickets += rmp->tickets;

    if (!total_tickets) /* there were no winnable processes */
        return OK;

    /* generate a "random" winning ticket */
    /* lower bits of time stamp counter are random enough */
    /*   and much faster then random() */
    read_tsc_64(&tsc);
    winner = tsc % total_tickets + 1;

    /* now find the process with the winning ticket */
    for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; ++proc_nr, ++rmp) {
        if (rmp->priority == HOLDING_Q && rmp->flags == (IN_USE | USER_PROCESS)) /* winnable? */
            winner -= rmp->tickets;
        if (winner <= 0)
            break;
    }

    printf("Process %d won with %d(%d) of %d tickets\n", proc_nr, rmp->tickets, rmp->blocking, total_tickets);
    /* schedule new winning process */
    rmp->priority = WINNING_Q;
    rmp->time_slice = USER_QUANTUM;
    /*if (rmp->blocking)
    rmp->time_slice = USER_QUANTUM / (rmp->blocking + 1); */
    rmp->blocking = 0;

    if ((rv = schedule_process_local(rmp)) != OK)
        return rv;
    return OK;
}

/*===========================================================================*
*              change_tickets                     *
*===========================================================================*/

void change_tickets(struct schedproc *rmp, int qty) {
    rmp->tickets += qty;
    if (rmp->tickets > MAX_TICKETS)
        rmp->tickets = MAX_TICKETS;
    if (rmp->tickets < MIN_TICKETS)
        rmp->tickets = MIN_TICKETS;
}

/* CHANGE END */

/*===========================================================================*
 *				do_noquantum				     *
 *===========================================================================*/

int do_noquantum(message *m_ptr)
{
/* CHANGE START */
    struct schedproc *rmp, *rmp_temp;
/* CHANGE END */
    int rv, proc_nr_n;

	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
		m_ptr->m_source);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];

    /* CHANGE START */
    printf("do_noquantum, priority %d\n", rmp->priority);
    /* system process - change priority and return */
    if (!(rmp->flags & USER_PROCESS) && rmp->priority < WINNING_Q) {
        if (rmp->priority < WINNING_Q - 1) {
            rmp->priority++;
            schedule_process_local(rmp);
        }
        return OK;
    }
    /* user process */
    if (rmp->priority == WINNING_Q) { /* winner ran out of quantum */
        if (rmp->blocking) {
            change_tickets(rmp, 1);
            printf("IO process out of quantum, blocked %d times\n", rmp->blocking);
        } else {
            change_tickets(rmp, -1);
            printf("CPU process out of quantum\n");
        }
        rmp->priority = HOLDING_Q;
    } else { /* a non winning task finished, meaning all winning tasks are io bound */
        for (proc_nr_n = 0, rmp_temp = schedproc; proc_nr_n < NR_PROCS; ++proc_nr_n, ++rmp_temp)
            if (rmp_temp->priority == WINNING_Q)
                rmp_temp->blocking++;
        printf("IO bound process detected - increasing blocking to %d\n", rmp_temp->blocking);
    }

    if ((rv = schedule_process_local(rmp)) != OK) /* move out of quantum process */
        return rv;

    debug();

    if ((rv = do_lottery()) != OK) /* schedule a new winner */
        return rv;

    /* CHANGE END */

    return OK;
}

/*===========================================================================*
 *				do_stop_scheduling			     *
 *===========================================================================*/
int do_stop_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int proc_nr_n;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
#ifdef CONFIG_SMP
	cpu_proc[rmp->cpu]--;
#endif
	rmp->flags = 0; /*&= ~IN_USE;*/

	return OK;
}

/*===========================================================================*
 *				do_start_scheduling			     *
 *===========================================================================*/
int do_start_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n;
	
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
	if (rmp->max_priority >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Inherit current priority and time slice from parent. Since there
	 * is currently only one scheduler scheduling the whole system, this
	 * value is local and we assert that the parent endpoint is valid */
	if (rmp->endpoint == rmp->parent) {
		/* We have a special case here for init, which is the first
		   process scheduled, and the parent of itself. */
		rmp->priority   = USER_Q;
		rmp->time_slice = DEFAULT_USER_TIME_SLICE;

		/*
		 * Since kernel never changes the cpu of a process, all are
		 * started on the BSP and the userspace scheduling hasn't
		 * changed that yet either, we can be sure that BSP is the
		 * processor where the processes run now.
		 */
#ifdef CONFIG_SMP
		rmp->cpu = machine.bsp_id;
		/* FIXME set the cpu mask */
#endif
	}
	
	switch (m_ptr->m_type) {

	case SCHEDULING_START:
		/* We have a special case here for system processes, for which
		 * quanum and priority are set explicitly rather than inherited 
		 * from the parent */
		rmp->priority   = rmp->max_priority;
		rmp->time_slice = (unsigned) m_ptr->SCHEDULING_QUANTUM;
		break;
		
	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->SCHEDULING_PARENT,
				&parent_nr_n)) != OK)
			return rv;

		rmp->priority = schedproc[parent_nr_n].priority;
		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		break;
		
	default: 
		/* not reachable */
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0, 0)) != OK) {
		printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
			rmp->endpoint, rv);
		return rv;
	}
	rmp->flags = IN_USE;

	/* Schedule the process, giving it some quantum */
	pick_cpu(rmp);
	while ((rv = schedule_process(rmp, SCHEDULE_CHANGE_ALL)) == EBADCPU) {
		/* don't try this CPU ever again */
		cpu_proc[rmp->cpu] = CPU_DEAD;
		pick_cpu(rmp);
	}

	if (rv != OK) {
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
 *				do_nice					     *
 *===========================================================================*/
int do_nice(message *m_ptr)
{
	struct schedproc *rmp;
	int rv;
	int proc_nr_n;
/* CHANGE START */
    unsigned tickets_to_add;
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
/* CHANGE START */
    tickets_to_add = (unsigned)m_ptr->SCHEDULING_MAXPRIO;

    change_tickets(rmp, tickets_to_add);
/* CHANGE END */

	return rv;
}

/*===========================================================================*
 *				schedule_process			     *
 *===========================================================================*/
static int schedule_process(struct schedproc * rmp, unsigned flags)
{
	int err;
	int new_prio, new_quantum, new_cpu;

	pick_cpu(rmp);

	if (flags & SCHEDULE_CHANGE_PRIO)
		new_prio = rmp->priority;
	else
		new_prio = -1;

	if (flags & SCHEDULE_CHANGE_QUANTUM)
		new_quantum = rmp->time_slice;
	else
		new_quantum = -1;

	if (flags & SCHEDULE_CHANGE_CPU)
		new_cpu = rmp->cpu;
	else
		new_cpu = -1;

	if ((err = sys_schedule(rmp->endpoint, new_prio,
		new_quantum, new_cpu)) != OK) {
		printf("PM: An error occurred when trying to schedule %d: %d\n",
		rmp->endpoint, err);
	}

	return err;
}


/*===========================================================================*
 *				start_scheduling			     *
 *===========================================================================*/

void init_scheduling(void)
{
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
static void balance_queues(struct timer *tp)
{
	struct schedproc *rmp;
	int proc_nr;

/* CHANGE START */
    for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++)
        /* we only want to change system processes */
        if (rmp->flags == IN_USE && rmp->priority > rmp->max_priority) {
            rmp->priority--; /* increase priority */
            schedule_process_local(rmp);
        }
/* CHANGE END */

	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}