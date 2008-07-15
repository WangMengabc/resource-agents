/**
  @file Main loop / functions for disk-based quorum daemon.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <disk.h>
#include <platform.h>
#include <unistd.h>
#include <time.h>
#include <sys/reboot.h>
#include <sys/time.h>
#include <sys/un.h>
#include <linux/reboot.h>
#include <sched.h>
#include <signal.h>
#include <ccs.h>
#include <openais/service/logsys.h>
#include "score.h"
#if (!defined(LIBCMAN_VERSION) || \
     (defined(LIBCMAN_VERSION) && LIBCMAN_VERSION < 2))
#include <cluster/cnxman-socket.h>
#endif

/* from daemon_init.c */
int daemon_init(char *);
int check_process_running(char *, pid_t *);

/* from proc.c */
char *state_str(disk_node_state_t s);

/*
  TODO:
  1) Take into account timings to gracefully extend node timeouts during 
     node spikes (that's why they are there!).
  2) Poll ccsd for configuration changes.
  3) Logging.
 */

/* From bitmap.c */
int clear_bit(uint8_t *mask, uint32_t bitidx, uint32_t masklen);
int set_bit(uint8_t *mask, uint32_t bitidx, uint32_t masklen);
int is_bit_set(uint8_t *mask, uint32_t bitidx, uint32_t masklen);
inline int get_time(struct timeval *tv, int use_uptime);
inline void _diff_tv(struct timeval *dest, struct timeval *start,
		     struct timeval *end);

static int _running = 1;
static void update_local_status(qd_ctx *ctx, node_info_t *ni, int max, int score,
		    	 int score_req, int score_max);


LOGSYS_DECLARE_SYSTEM (NULL,
        LOG_MODE_OUTPUT_STDERR | LOG_MODE_OUTPUT_SYSLOG_THREADED | LOG_MODE_OUTPUT_FILE | LOG_MODE_BUFFER_BEFORE_CONFIG,
        LOGDIR "/qdisk.log",
        SYSLOGFACILITY);

LOGSYS_DECLARE_SUBSYS ("QDISK", SYSLOGLEVEL);

static void
int_handler(int sig)
{
	_running = 0;
}


/**
  Simple thing to see if a node is running.
 */
static inline int
state_run(disk_node_state_t state)
{
	return (state >= S_INIT ? state : 0);
}


/**
  Clear out / initialize node info block.
 */
static void
node_info_init(node_info_t *ni, int max)
{
	int x;
	time_t t = time(NULL);

	memset(ni, 0, sizeof(*ni) * max);
	for (x = 0; x < max; x++) {
		ni[x].ni_status.ps_nodeid = (x + 1); /* node ids are 1-based */
		ni[x].ni_status.ps_timestamp = t;
		ni[x].ni_misses = 0;
		ni[x].ni_last_seen = t;
	}
}


/**
  Check to see if someone tried to evict us but we were out to lunch.
  Rare case; usually other nodes would put up the 'Undead' message and
  re-evict us.
 */
static void
check_self(qd_ctx *ctx, status_block_t *sb)
{
	if (!sb->ps_updatenode ||
	    (sb->ps_updatenode == ctx->qc_my_id)) {
		return;
	}

	/* I did not update this??! */
	switch(sb->ps_state) {
	case S_EVICT:
		/* Someone told us to die. */
		reboot(RB_AUTOBOOT);
	default:
		log_printf(LOG_EMERG, "Unhandled state: %d\n", sb->ps_state);
		raise(SIGSTOP);
	}
}


/**
  Read in the node blocks off of the quorum disk and see if anyone has
  or has not updated their timestamp recently.  See check_transitions as
  well.
 */
static void
read_node_blocks(qd_ctx *ctx, node_info_t *ni, int max)
{
	int x;
	status_block_t *sb;

	for (x = 0; x < max; x++) {

		sb = &ni[x].ni_status;

		if (qdisk_read(&ctx->qc_disk,
			       qdisk_nodeid_offset(x+1, ctx->qc_disk.d_blksz),
			       sb, sizeof(*sb)) < 0) {
			log_printf(LOG_WARNING,"Error reading node ID block %d\n",
			       x+1);
			continue;
		}
		swab_status_block_t(sb);

		if (sb->ps_nodeid == ctx->qc_my_id) {
			check_self(ctx, sb);
			continue;
		} 
		/* message. */
		memcpy(&(ni[x].ni_last_msg), &(ni[x].ni_msg),
		       sizeof(ni[x].ni_last_msg));
		ni[x].ni_msg.m_arg = sb->ps_arg;
		ni[x].ni_msg.m_msg = sb->ps_msg;
		ni[x].ni_msg.m_seq = sb->ps_seq;

		if (!state_run(sb->ps_state))
			continue;

		/* Unchanged timestamp: miss */
		if (sb->ps_timestamp == ni[x].ni_last_seen) {
			/* XXX check for average + allow grace */
			ni[x].ni_misses++;
			if (ni[x].ni_misses > 1) {
				log_printf(LOG_DEBUG,
					"Node %d missed an update (%d/%d)\n",
					x+1, ni[x].ni_misses, ctx->qc_tko);
			}
			continue;
		}

		/* Got through?  The node is good. */
		ni[x].ni_misses = 0;
		ni[x].ni_seen++;
		ni[x].ni_last_seen = sb->ps_timestamp;
	}
}


/**
  Check for node transitions.
 */
static void
check_transitions(qd_ctx *ctx, node_info_t *ni, int max, memb_mask_t mask)
{
	int x;

	if (mask)
		memset(mask, 0, sizeof(memb_mask_t));

	for (x = 0; x < max; x++) {

		/*
		   Case 1: check to see if the node is still up
		   according to our internal state, but has been
		   evicted by the master or cleanly shut down
		   (or restarted).

		   Transition from Evicted/Shutdown -> Offline
		 */
		if ((ni[x].ni_state >= S_EVICT &&
		     ni[x].ni_status.ps_state <= S_EVICT) ||
		     (ni[x].ni_incarnation &&
		      (ni[x].ni_incarnation !=
		       ni[x].ni_status.ps_incarnation))) {

			if (ni[x].ni_status.ps_state == S_EVICT) {
				log_printf(LOG_NOTICE, "Node %d evicted\n",
				       ni[x].ni_status.ps_nodeid);
			} else {
				/* State == S_NONE or incarnation change */
				log_printf(LOG_INFO, "Node %d shutdown\n",
				       ni[x].ni_status.ps_nodeid);
				ni[x].ni_evil_incarnation = 0;
			}

			ni[x].ni_incarnation = 0;
			ni[x].ni_seen = 0;
			ni[x].ni_misses = 0;
			ni[x].ni_state = S_NONE;

			/* Clear our master mask for the node after eviction
			 * or shutdown */
			if (mask)
				clear_bit(mask, (ni[x].ni_status.ps_nodeid-1),
					  sizeof(memb_mask_t));
			continue;
		}

		/*
		   Case 2: Check for a heartbeat timeout.  Write an eviction
		   notice if we're the master.  If this is our first notice
		   of the heartbeat timeout, update our internal state
		   accordingly.  When the master evicts this node, we will
		   hit case 1 above.

		   Transition from Online -> Evicted
		 */
		if (ni[x].ni_misses > ctx->qc_tko &&
		     state_run(ni[x].ni_status.ps_state)) {

			/*
			   Write eviction notice if we're the master.
			 */
			if (ctx->qc_status == S_MASTER) {
				log_printf(LOG_NOTICE,
				       "Writing eviction notice for node %d\n",
				       ni[x].ni_status.ps_nodeid);
				qd_write_status(ctx, ni[x].ni_status.ps_nodeid,
						S_EVICT, NULL, NULL, NULL);
				if (ctx->qc_flags & RF_ALLOW_KILL) {
					log_printf(LOG_DEBUG, "Telling CMAN to "
						"kill the node\n");
					cman_kill_node(ctx->qc_ch,
						ni[x].ni_status.ps_nodeid);
				}
			}

			/*
			   Mark our internal views as dead if nodes miss too
			   many heartbeats...  This will cause a master
			   transition if no live master exists.
			 */
			if (ni[x].ni_status.ps_state >= S_RUN &&
			    ni[x].ni_seen) {
				log_printf(LOG_DEBUG, "Node %d DOWN\n",
				       ni[x].ni_status.ps_nodeid);
				ni[x].ni_seen = 0;	
			}

			ni[x].ni_state = S_EVICT;
			ni[x].ni_status.ps_state = S_EVICT;
			ni[x].ni_evil_incarnation = 
				ni[x].ni_status.ps_incarnation;
			
			/* Clear our master mask for the node after eviction */
			if (mask)
				clear_bit(mask, (ni[x].ni_status.ps_nodeid-1),
					  sizeof(memb_mask_t));
			continue;
		}

		/*
		   Case 3:  Check for node who is supposed to be dead, but
		   has started writing to the disk again with the same
		   incarnation.  

		   Transition from Offline -> Undead (BAD!!!)
		 */
		if (ni[x].ni_evil_incarnation &&
                    (ni[x].ni_evil_incarnation == 
		     ni[x].ni_status.ps_incarnation)) {
			log_printf(LOG_CRIT, "Node %d is undead.\n",
			       ni[x].ni_status.ps_nodeid);

			log_printf(LOG_ALERT,
			       "Writing eviction notice for node %d\n",
			       ni[x].ni_status.ps_nodeid);
			qd_write_status(ctx, ni[x].ni_status.ps_nodeid,
					S_EVICT, NULL, NULL, NULL);
			ni[x].ni_status.ps_state = S_EVICT;

			/* XXX Need to fence it again */
			if (ctx->qc_flags & RF_ALLOW_KILL) {
				log_printf(LOG_DEBUG, "Telling CMAN to "
					"kill the node\n");
				cman_kill_node(ctx->qc_ch,
					ni[x].ni_status.ps_nodeid);
			}
			continue;
		}


		/*
		   Case 4:  Check for a node who has met our minimum # of
		   'seen' requests.

		   Transition from Offline -> Online
		 */
		if (ni[x].ni_seen > ctx->qc_tko_up &&
		    !state_run(ni[x].ni_state)) {
			/*
			   Node-join - everyone just kind of "agrees"
			   there's no consensus to just have a node join
			   right now.
			 */
			ni[x].ni_state = S_RUN;
			log_printf(LOG_DEBUG, "Node %d is UP\n",
			       ni[x].ni_status.ps_nodeid);
			ni[x].ni_incarnation =
			    ni[x].ni_status.ps_incarnation;
			if (mask)
				set_bit(mask, (ni[x].ni_status.ps_nodeid-1),
					sizeof(memb_mask_t));

			continue;
		}

		/*
		   Case 5: Check for a node becoming master.  Not really a
		   transition.
		 */
		if (ni[x].ni_state == S_RUN &&
		    ni[x].ni_status.ps_state == S_MASTER) {
			log_printf(LOG_INFO, "Node %d is the master\n",
			       ni[x].ni_status.ps_nodeid);
			ni[x].ni_state = S_MASTER;
			if (mask)
				set_bit(mask, (ni[x].ni_status.ps_nodeid-1),
					sizeof(memb_mask_t));
			continue;
		}

		/*
		   All other cases: Believe the node's reported state ;)
		 */
		if (state_run(ni[x].ni_state)) {
			ni[x].ni_state = ni[x].ni_status.ps_state;
			if (mask)
				set_bit(mask, (ni[x].ni_status.ps_nodeid-1),
					sizeof(memb_mask_t));
		}
	}
}


/**
  Checks for presence of an online master.  If there is no
  Returns
 */
static int
master_exists(qd_ctx *ctx, node_info_t *ni, int max, int *low_id, int *count)
{
	int x;
	int masters = 0;
	int ret = 0;

	if (count)
		*count = 0;
	*low_id = ctx->qc_my_id;

	for (x = 0; x < max; x++) {

		/* See if this one's a master */
		if (ni[x].ni_state >= S_RUN &&
		    ni[x].ni_status.ps_state == S_MASTER &&
		    ni[x].ni_status.ps_nodeid != ctx->qc_my_id) {
			if (!ret)
				ret = ni[x].ni_status.ps_nodeid;
			++masters;
			continue;
		}

		/* See if it's us... */
		if (ni[x].ni_status.ps_nodeid == ctx->qc_my_id &&
		    ni[x].ni_status.ps_state == S_MASTER) {
			if (!ret)
				ret = ctx->qc_my_id;
			++masters;
			continue;
		}

		/* Look for dead master */
		if (ni[x].ni_state < S_RUN &&
		    ni[x].ni_status.ps_state == S_MASTER) {
			log_printf(LOG_DEBUG,
			       "Node %d is marked master, but is dead.\n",
			       ni[x].ni_status.ps_nodeid);
			continue;
		}

		if (ni[x].ni_state < S_RUN)
			continue;
		
		if (ni[x].ni_status.ps_nodeid < *low_id)
			*low_id = ni[x].ni_status.ps_nodeid;
	}

	if (count)
		*count = masters;
	/*
 	else if (masters == 1) {
		printf("Node %d is the master\n", ret);
	} else {
		printf("No master found; node %d should be the master\n",
		       *low_id);
	}
	*/

	return ret;
}


/**
  initialize node information blocks and wait to see if there is already
  a cluster running using this QD.  Note that this will delay master
  election if multiple nodes start with a second or two of each other.
 */
static int
quorum_init(qd_ctx *ctx, node_info_t *ni, int max, struct h_data *h, int maxh)
{
	int x = 0, score, maxscore, score_req;

	log_printf(LOG_INFO, "Quorum Daemon Initializing\n");
	
	if (mlockall(MCL_CURRENT|MCL_FUTURE) != 0) {
		log_printf(LOG_ERR, "Unable to mlockall()\n");
	}

	if (qdisk_validate(ctx->qc_device) < 0)
		return -1;

	if (qdisk_open(ctx->qc_device, &ctx->qc_disk) < 0) {
		log_printf(LOG_CRIT, "Failed to open %s: %s\n", ctx->qc_device,
		       strerror(errno));
		return -1;
	}

	log_printf(LOG_DEBUG, "I/O Size: %lu  Page Size: %lu\n",
	       (unsigned long)ctx->qc_disk.d_blksz, (unsigned long)ctx->qc_disk.d_pagesz);
	
	if (h && maxh) {
		start_score_thread(ctx, h, maxh);
	} else {
		log_printf(LOG_DEBUG, "Permanently setting score to 1/1\n");
		fudge_scoring();
	}

	node_info_init(ni, max);
	ctx->qc_status = S_INIT;
	if (qd_write_status(ctx, ctx->qc_my_id,
			    S_INIT, NULL, NULL, NULL) != 0) {
		log_printf(LOG_CRIT, "Could not initialize status block!\n");
		return -1;
	}

	while (++x <= ctx->qc_tko && _running) {
		read_node_blocks(ctx, ni, max);
		check_transitions(ctx, ni, max, NULL);

		if (qd_write_status(ctx, ctx->qc_my_id,
				    S_INIT, NULL, NULL, NULL) != 0) {
			log_printf(LOG_CRIT, "Initialization failed\n");
			return -1;
		}

		get_my_score(&score, &maxscore);
		score_req = ctx->qc_scoremin;
		if (score_req <= 0)
			score_req = (maxscore/2 + 1);
		update_local_status(ctx, ni, max, score, score_req, maxscore);

		sleep(ctx->qc_interval);
	}

	get_my_score(&score, &maxscore);
	log_printf(LOG_INFO, "Initial score %d/%d\n", score, maxscore);
	log_printf(LOG_INFO, "Initialization complete\n");

	return 0;
}


/**
  Vote for a master if it puts a bid in.
 */
static void
do_vote(qd_ctx *ctx, node_info_t *ni, int max, disk_msg_t *msg)
{
	int x;

	for (x = 0; x < max; x++) {
		if (ni[x].ni_state != S_RUN)
			continue;

		if (ni[x].ni_status.ps_msg == M_BID &&
		    ni[x].ni_status.ps_nodeid < ctx->qc_my_id) {

			/* Vote for lowest bidding ID that is lower
			   than us */
			msg->m_msg = M_ACK;
			msg->m_arg = ni[x].ni_status.ps_nodeid;
			msg->m_seq = ni[x].ni_status.ps_seq;

			return;
		}
	}
}


/*
  Check to match nodes in mask with nodes online according to CMAN.
  Only the master needs to do this.
 */
static void
check_cman(qd_ctx *ctx, memb_mask_t mask, memb_mask_t master_mask)
{
	cman_node_t nodes[MAX_NODES_DISK];
	int retnodes, x;

	if (cman_get_nodes(ctx->qc_ch, MAX_NODES_DISK,
			   &retnodes, nodes) <0 )
		return;

	memset(master_mask, 0, sizeof(master_mask));
	for (x = 0; x < retnodes; x++) {
		if (is_bit_set(mask, nodes[x].cn_nodeid-1, sizeof(mask)) &&
		    nodes[x].cn_member) {
			set_bit(master_mask, nodes[x].cn_nodeid-1,
				sizeof(master_mask));
		} else {
			/* Not in CMAN output = not allowed */
			clear_bit(master_mask, (nodes[x].cn_nodeid-1),
				  sizeof(memb_mask_t));
		}
	}
}


/* 
   returns:
	3: all acks received - you are the master.
	2: nacked (not highest score?) might not happen
	1: other node with lower ID is bidding and we should rescind our
	   bid.
	0: still waiting; don't clear bid; just wait another round.
   Modifies:
	*msg - it will store the vote for the lowest bid if we should
	clear our bid.
 */ 
static int
check_votes(qd_ctx *ctx, node_info_t *ni, int max, disk_msg_t *msg)
{
	int x, running = 0, acks = 0, nacks = 0, low_id = ctx->qc_my_id;

	for (x = 0; x < max; x++) {
		if (state_run(ni[x].ni_state))
			++running;
		else
			continue;

		if (ni[x].ni_status.ps_msg == M_ACK &&
		    ni[x].ni_status.ps_arg == ctx->qc_my_id) {
			++acks;
		}

		if (ni[x].ni_status.ps_msg == M_NACK &&
		    ni[x].ni_status.ps_arg == ctx->qc_my_id) {
			++nacks;
		}
		
		/* If there's someone with a lower ID who is also
		   bidding for master, change our message to vote
		   for the lowest bidding node ID */
		if (ni[x].ni_status.ps_msg == M_BID && 
		    ni[x].ni_status.ps_nodeid < low_id) {
			low_id = ni[x].ni_status.ps_nodeid;
			msg->m_msg = M_ACK;
			msg->m_arg = ni[x].ni_status.ps_nodeid;
			msg->m_seq = ni[x].ni_status.ps_seq;
		}
	}

	if (acks == running)
		return 3;
	if (nacks)
		return 2;
	if (low_id != ctx->qc_my_id)
		return 1;
	return 0;
}

static void
print_node_info(FILE *fp, node_info_t *ni)
{
	fprintf(fp, "node_info_t [node %d] {\n", ni->ni_status.ps_nodeid);
	fprintf(fp, "    ni_incarnation = 0x%08x%08x\n",
		((int)(ni->ni_incarnation>>32))&0xffffffff,
		((int)(ni->ni_incarnation)&0xffffffff));
	fprintf(fp, "    ni_evil_incarnation = 0x%08x%08x\n",
		((int)(ni->ni_evil_incarnation>>32))&0xffffffff,
		((int)(ni->ni_evil_incarnation)&0xffffffff));
	fprintf(fp, "    ni_last_seen = %s", ctime(&ni->ni_last_seen));
	fprintf(fp, "    ni_misses = %d\n", ni->ni_misses);
	fprintf(fp, "    ni_seen = %d\n", ni->ni_seen);
	fprintf(fp, "    ni_msg = {\n");
	fprintf(fp, "        m_msg = 0x%08x\n", ni->ni_msg.m_msg);
	fprintf(fp, "        m_arg = %d\n", ni->ni_msg.m_arg);
	fprintf(fp, "        m_seq = %d\n", ni->ni_msg.m_seq);
	fprintf(fp, "    }\n");
	fprintf(fp, "    ni_last_msg = {\n");
	fprintf(fp, "        m_msg = 0x%08x\n", ni->ni_last_msg.m_msg);
	fprintf(fp, "        m_arg = %d\n", ni->ni_last_msg.m_arg);
	fprintf(fp, "        m_seq = %d\n", ni->ni_last_msg.m_seq);
	fprintf(fp, "    }\n");
	fprintf(fp, "    ni_state = 0x%08x (%s)\n", ni->ni_state,
		state_str(ni->ni_state));
	fprintf(fp, "}\n\n");
}


static void
update_local_status(qd_ctx *ctx, node_info_t *ni, int max, int score,
		    int score_req, int score_max)
{
	FILE *fp;
	int x, need_close = 0;
	time_t now;

	if (!ctx->qc_status_file)
		return;

	if (strcmp(ctx->qc_status_file, "-") == 0) {
		fp = stdout;
	} else {
		fp = fopen(ctx->qc_status_file, "w+");
		if (fp == NULL)
			return;
		need_close = 1;
	}

	now = time(NULL);
	fprintf(fp, "Time Stamp: %s", ctime(&now));
	fprintf(fp, "Node ID: %d\n", ctx->qc_my_id);
	
	fprintf(fp, "Score: %d/%d (Minimum required = %d)\n",
		score, score_max, score_req);
	fprintf(fp, "Current state: %s\n", state_str(ctx->qc_status));

	/*
	fprintf(fp, "Current disk state: %s\n",
		state_str(ctx->qc_disk_status));
	 */
	fprintf(fp, "Initializing Set: {");
	for (x=0; x<max; x++) {
		if (ni[x].ni_status.ps_state == S_INIT && ni[x].ni_seen)
			fprintf(fp," %d", ni[x].ni_status.ps_nodeid);
	}
	fprintf(fp, " }\n");

	fprintf(fp, "Visible Set: {");
	for (x=0; x<max; x++) {
		if (ni[x].ni_state >= S_RUN || ni[x].ni_status.ps_nodeid == 
		    ctx->qc_my_id)
			fprintf(fp," %d", ni[x].ni_status.ps_nodeid);
	}
	fprintf(fp, " }\n");
	
	if (ctx->qc_status == S_INIT)
		goto out;
	
	if (ctx->qc_master)
		fprintf(fp, "Master Node ID: %d\n", ctx->qc_master);
	else 
		fprintf(fp, "Master Node ID: (none)\n");

	if (!ctx->qc_master)
		goto out;

	fprintf(fp, "Quorate Set: {");
	for (x=0; x<max; x++) {
		if (is_bit_set(ni[ctx->qc_master-1].ni_status.ps_master_mask,
			       ni[x].ni_status.ps_nodeid-1,
			       sizeof(memb_mask_t))) {
			fprintf(fp," %d", ni[x].ni_status.ps_nodeid);
		}
	}

	fprintf(fp, " }\n");

out:
	if (ctx->qc_flags & RF_DEBUG) {
		for (x = 0; x < max; x++)
			print_node_info(fp, &ni[x]);
	}

	fprintf(fp, "\n");
	if (need_close)
		fclose(fp);
}

static inline int
_cmp_tv(struct timeval *left, struct timeval *right)
{
	if (left->tv_sec > right->tv_sec)
		return -1;

	if (left->tv_sec < right->tv_sec)
		return 1;

	if (left->tv_usec > right->tv_usec)
		return -1;
	
	if (left->tv_usec < right->tv_usec)
		return 1;

	return 0;
}


void
set_priority(int queue, int prio)
{
	struct sched_param s;
	int ret;
	char *func = "nice";
	
	if (queue == SCHED_OTHER) {
		s.sched_priority = 0;
		ret = sched_setscheduler(0, queue, &s);
		errno = 0;
		ret = nice(prio);
	} else {
		memset(&s,0,sizeof(s));
		s.sched_priority = prio;
		ret = sched_setscheduler(0, queue, &s);
		func = "sched_setscheduler";
	}
	
	if (ret < 0 && errno) {
		log_printf(LOG_WARNING, "set_priority [%s] failed: %s\n", func,
		       strerror(errno));
	}
}


static int
cman_alive(cman_handle_t ch)
{
	fd_set rfds;
	int fd = cman_get_fd(ch);
	struct timeval tv = {0, 0};
	
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	if (select(fd + 1, &rfds, NULL, NULL, &tv) == 1) {
		if (cman_dispatch(ch, CMAN_DISPATCH_ALL) < 0) {
			if (errno == EAGAIN)
				return 0;
			return -1;
		}
	}
	return 0;
}


static int
quorum_loop(qd_ctx *ctx, node_info_t *ni, int max)
{
	disk_msg_t msg = {0, 0, 0};
	int low_id, bid_pending = 0, score, score_max, score_req,
	    upgrade = 0, count;
	memb_mask_t mask, master_mask;
	struct timeval maxtime, oldtime, newtime, diff, sleeptime, interval;

	ctx->qc_status = S_NONE;
	
	maxtime.tv_usec = 0;
	maxtime.tv_sec = ctx->qc_interval * ctx->qc_tko;
	
	interval.tv_usec = 0;
	interval.tv_sec = ctx->qc_interval;
	
	get_my_score(&score, &score_max);
	if (score_max < ctx->qc_scoremin) {
		log_printf(LOG_WARNING, "Minimum score (%d) is impossible to "
		       "achieve (heuristic total = %d)\n",
		       ctx->qc_scoremin, score_max);
	}
	
	_running = 1;
	while (_running) {
		/* XXX this was getuptime() in clumanager */
		get_time(&oldtime, (ctx->qc_flags&RF_UPTIME));
		
		/* Read everyone else's status */
		read_node_blocks(ctx, ni, max);

		/* Check for node transitions */
		check_transitions(ctx, ni, max, mask);

		/* Check heuristics and remove ourself if necessary */
		get_my_score(&score, &score_max);

		/* If we recently upgraded, decrement our wait time */
		if (upgrade > 0)
			--upgrade;

		score_req = ctx->qc_scoremin;
		if (score_req <= 0)
			score_req = (score_max/2 + 1);

		if (score < score_req) {
			clear_bit(mask, (ctx->qc_my_id-1), sizeof(mask));
			if (ctx->qc_status > S_NONE) {
				log_printf(LOG_NOTICE,
				       "Score insufficient for master "
				       "operation (%d/%d; required=%d); "
				       "downgrading\n",
				       score, score_max, score_req);
				ctx->qc_status = S_NONE;
				msg.m_msg = M_NONE;
				++msg.m_seq;
				bid_pending = 0;
				if (cman_alive(ctx->qc_ch) < 0) {
					log_printf(LOG_ERR, "cman: %s\n",
					       strerror(errno));
				} else {
					cman_poll_quorum_device(ctx->qc_ch, 0);
				}
				if (ctx->qc_flags & RF_REBOOT)
					reboot(RB_AUTOBOOT);
			}
		}  else {
			set_bit(mask, (ctx->qc_my_id-1), sizeof(mask));
			if (ctx->qc_status == S_NONE) {
				log_printf(LOG_NOTICE,
				       "Score sufficient for master "
				       "operation (%d/%d; required=%d); "
				       "upgrading\n",
				       score, score_max, score_req);
				ctx->qc_status = S_RUN;
				upgrade = ctx->qc_upgrade_wait;
				bid_pending = 0;
				msg.m_msg = M_NONE;
				++msg.m_seq;
			}
		}

		/* Find master */
		ctx->qc_master = master_exists(ctx, ni, max, &low_id, &count);

		/* Resolve master conflict, if one exists */
		if (count >= 1 && ctx->qc_status == S_MASTER &&
		    ctx->qc_master != ctx->qc_my_id) {
			log_printf(LOG_WARNING, "Master conflict: abdicating\n");

			/* Handle just like a recent upgrade */
			ctx->qc_status = S_RUN;
			upgrade = ctx->qc_upgrade_wait;
			bid_pending = 0;
			msg.m_msg = M_NONE;
			++msg.m_seq;
		}

		/* Figure out what to do based on what we know */
		if (!ctx->qc_master &&
		    low_id == ctx->qc_my_id &&
		    ctx->qc_status == S_RUN &&
		    !bid_pending &&
		    !upgrade) {
			/*
			   If there's no master, and we are the lowest node
			   ID, make a bid to become master if we're not 
			   already bidding.  We can't do this if we've just
			   upgraded.
			 */

			log_printf(LOG_DEBUG,"Making bid for master\n");
			msg.m_msg = M_BID;
			++msg.m_seq;
			bid_pending = 1;

		} else if (!ctx->qc_master && !bid_pending) {

			/* We're not the master, and we do not have a bid
			   pending.  Check for voting on other nodes. */
			do_vote(ctx, ni, max, &msg);
		} else if (!ctx->qc_master && bid_pending) {

			/* We're currently bidding for master.
			   See if anyone's voted, or if we should
			   rescind our bid */
			++bid_pending;

			/* Yes, those are all deliberate fallthroughs */
			switch (check_votes(ctx, ni, max, &msg)) {
			case 3:
				/* 
				 * Give ample time to become aware of other
				 * nodes
				 */
				if (bid_pending < (ctx->qc_master_wait))
					break;
				
				log_printf(LOG_INFO,
				       "Assuming master role\n");
				ctx->qc_status = S_MASTER;
			case 2:
				msg.m_msg = M_NONE;
			case 1:
				bid_pending = 0;
			default:
				break;
			}
		} else if (ctx->qc_status == S_MASTER &&
			   ctx->qc_master != ctx->qc_my_id) {
			
			/* We think we're master, but someone else claims
			   that they are master. */

			log_printf(LOG_CRIT,
			       "A master exists, but it's not me?!\n");
			/* XXX Handle this how? Should not happen*/
			/* reboot(RB_AUTOBOOT); */

		} else if (ctx->qc_status == S_MASTER &&
			   ctx->qc_master == ctx->qc_my_id) {

			/* We are the master.  Poll the quorum device.
			   We can't be the master unless we score high
			   enough on our heuristics. */
			if (cman_alive(ctx->qc_ch) < 0) {
				log_printf(LOG_ERR, "cman_dispatch: %s\n",
				       strerror(errno));
				log_printf(LOG_ERR,
				       "Halting qdisk operations\n");
				return -1;
			}
			check_cman(ctx, mask, master_mask);
			cman_poll_quorum_device(ctx->qc_ch, 1);

		} else if (ctx->qc_status == S_RUN && ctx->qc_master &&
			   ctx->qc_master != ctx->qc_my_id) {

			/* We're not the master, but a master exists
			   Check to see if the master thinks we are 
			   online.  If we are, tell CMAN so. */
			if (is_bit_set(
			      ni[ctx->qc_master-1].ni_status.ps_master_mask,
				       ctx->qc_my_id-1,
				       sizeof(memb_mask_t))) {
				if (cman_alive(ctx->qc_ch) < 0) {
					log_printf(LOG_ERR, "cman_dispatch: %s\n",
						strerror(errno));
					log_printf(LOG_ERR,
						"Halting qdisk operations\n");
					return -1;
				}
				cman_poll_quorum_device(ctx->qc_ch, 1);
			}
		}
		
		/* Write out our status */
		if (qd_write_status(ctx, ctx->qc_my_id, ctx->qc_status,
				    &msg, mask, master_mask) != 0) {
			log_printf(LOG_ERR, "Error writing to quorum disk\n");
		}

		/* write out our local status */
		update_local_status(ctx, ni, max, score, score_req, score_max);

		/* Cycle. We could time the loop and sleep
		   usleep(interval-looptime), but this is fine for now.*/
		get_time(&newtime, ctx->qc_flags&RF_UPTIME);
		_diff_tv(&diff, &oldtime, &newtime);
		
		/*
		 * Reboot if we didn't send a heartbeat in interval*TKO_COUNT
		 */
		if (_cmp_tv(&maxtime, &diff) == 1 &&
		    ctx->qc_flags & RF_PARANOID) {
			log_printf(LOG_EMERG, "Failed to complete a cycle within "
			       "%d second%s (%d.%06d) - REBOOTING\n",
			       (int)maxtime.tv_sec,
			       maxtime.tv_sec==1?"":"s",
			       (int)diff.tv_sec,
			       (int)diff.tv_usec);
			if (!(ctx->qc_flags & RF_DEBUG)) 
				reboot(RB_AUTOBOOT);
		}

		/*
		 * If the amount we took to complete a loop is greater or less
		 * than our interval, we adjust by the difference each round.
		 *
		 * It's not really "realtime", but it helps!
		 */
		if (_cmp_tv(&diff, &interval) == 1) {
			_diff_tv(&sleeptime, &diff, &interval);
		} else {
			log_printf(LOG_WARNING, "qdisk cycle took more "
			       "than %d second%s to complete (%d.%06d)\n",
			       ctx->qc_interval, ctx->qc_interval==1?"":"s",
			       (int)diff.tv_sec, (int)diff.tv_usec);
			memcpy(&sleeptime, &interval, sizeof(sleeptime));
		}
		
		/* Could hit a watchdog timer here if we wanted to */
		if (_running)
			select(0, NULL, NULL, NULL, &sleeptime);
	}

	return 0;
}


/**
  Tell the other nodes we're done (safely!).
 */
static int
quorum_logout(qd_ctx *ctx)
{
	/* Write out our status */
	if (qd_write_status(ctx, ctx->qc_my_id, S_NONE,
			    NULL, NULL, NULL) != 0) {
		log_printf(LOG_WARNING,
		       "Error writing to quorum disk during logout\n");
	}
	return 0;
}


/**
  Grab logsys configuration data from libccs
 */
static int
get_logsys_config_data(int *debug)
{
	int ccsfd = -1, loglevel = SYSLOGLEVEL, facility = SYSLOGFACILITY;
	char *val = NULL, *error = NULL;
	unsigned int logmode;
	int global_debug = 0;

	log_printf(LOG_DEBUG, "Loading logsys configuration information\n");

	ccsfd = ccs_connect();
	if (ccsfd < 0) {
		log_printf(LOG_CRIT, "Connection to CCSD failed; cannot start\n");
		return -1;
	}

	logmode = logsys_config_mode_get();

	if (!debug) {
		if (ccs_get(ccsfd, "/cluster/logging/@debug", &val) == 0) {
			if(!strcmp(val, "on")) {
				global_debug = 1;
			} else 
			if(!strcmp(val, "off")) {
				global_debug = 0;
			} else
				log_printf(LOG_ERR, "global debug: unknown value\n");
			free(val);
			val = NULL;
		}

		if (ccs_get(ccsfd, "/cluster/logging/logger_subsys[@subsys=\"QDISK\"]/@debug", &val) == 0) {
			if(!strcmp(val, "on")) {
				*debug = 1;
			} else 
			if(!strcmp(val, "off")) { /* debug from cmdline/envvars override config */
				*debug = 0;
			} else
				log_printf(LOG_ERR, "subsys debug: unknown value: %s\n", val);
			free(val);
			val = NULL;
		} else
			*debug = global_debug; /* global debug overrides subsystem only if latter is not specified */

		if (ccs_get(ccsfd, "/cluster/logging/logger_subsys[@subsys=\"QDISK\"]/@syslog_level", &val) == 0) {
			loglevel = logsys_priority_id_get (val);
			if (loglevel < 0)
				loglevel = SYSLOGLEVEL;

			if (!*debug) {
				if (loglevel == LOG_LEVEL_DEBUG)
					*debug = 1;

				logsys_config_priority_set (loglevel);
			}

			free(val);
			val = NULL;
		} else
		if (ccs_get(ccsfd, "/cluster/quorumd/@log_level", &val) == 0) { /* check backward compat options */
			loglevel = logsys_priority_id_get (val);
			if (loglevel < 0)
				loglevel = SYSLOGLEVEL;

			log_printf(LOG_ERR, "<quorumd log_level=\"%s\".. option is depracated\n", val);

			if (!*debug) {
				if (loglevel == LOG_LEVEL_DEBUG)
					*debug = 1;

				logsys_config_priority_set (loglevel);
			}

			free(val);
			val = NULL;
		}
	} else
		logsys_config_priority_set (LOG_LEVEL_DEBUG);

	if (ccs_get(ccsfd, "/cluster/logging/@to_stderr", &val) == 0) {
		if(!strcmp(val, "yes")) {
			logmode |= LOG_MODE_OUTPUT_STDERR;
		} else 
		if(!strcmp(val, "no")) {
			logmode &= ~LOG_MODE_OUTPUT_STDERR;
		} else
			log_printf(LOG_ERR, "to_stderr: unknown value\n");
		free(val);
		val = NULL;
	}

	if (ccs_get(ccsfd, "/cluster/logging/@to_syslog", &val) == 0) {
		if(!strcmp(val, "yes")) {
			logmode |= LOG_MODE_OUTPUT_SYSLOG_THREADED;
		} else 
		if(!strcmp(val, "no")) {
			logmode &= ~LOG_MODE_OUTPUT_SYSLOG_THREADED;
		} else
			log_printf(LOG_ERR, "to_syslog: unknown value\n");
		free(val);
		val = NULL;
	}

	if (ccs_get(ccsfd, "/cluster/logging/@to_file", &val) == 0) {
		if(!strcmp(val, "yes")) {
			logmode |= LOG_MODE_OUTPUT_FILE;
		} else 
		if(!strcmp(val, "no")) {
			logmode &= ~LOG_MODE_OUTPUT_FILE;
		} else
			log_printf(LOG_ERR, "to_file: unknown value\n");
		free(val);
		val = NULL;
	}

	if (ccs_get(ccsfd, "/cluster/logging/@logfile", &val) == 0) {
		if(logsys_config_file_set(&error, val))
			log_printf(LOG_ERR, "logfile: unable to open %s for logging\n", val);
		free(val);
		val = NULL;
	} else
		log_printf(LOG_DEBUG, "logfile: use default built-in log file: %s\n", LOGDIR "/qdisk.log");

	if (ccs_get(ccsfd, "/cluster/logging/@syslog_facility", &val) == 0) {
		facility = logsys_facility_id_get (val);
		if (facility < 0) {
			log_printf(LOG_ERR, "syslog_facility: unknown value\n");
			facility = SYSLOGFACILITY;
		}

		logsys_config_facility_set ("QDISK", facility);
		free(val);
	} else
	if (ccs_get(ccsfd, "/cluster/quorumd/@log_facility", &val) == 0) {
		facility = logsys_facility_id_get (val);
		if (facility < 0) {
			log_printf(LOG_ERR, "syslog_facility: unknown value\n");
			facility = SYSLOGFACILITY;
		}

		log_printf(LOG_ERR, "<quorumd log_facility=\"%s\".. option is depracated\n", val);

		logsys_config_facility_set ("QDISK", facility);
		free(val);
		val = NULL;
	}

	if(logmode & LOG_MODE_BUFFER_BEFORE_CONFIG) {
		log_printf(LOG_DEBUG, "logsys config enabled from get_logsys_config_data\n");
		logmode &= ~LOG_MODE_BUFFER_BEFORE_CONFIG;
		logmode |= LOG_MODE_FLUSH_AFTER_CONFIG;
		logsys_config_mode_set (logmode);
	}

	ccs_disconnect(ccsfd);

	return 0;
}

/**
  Grab all our configuration data from libccs
 */
static int
get_config_data(qd_ctx *ctx, struct h_data *h, int maxh,
		int *cfh, int debug, int trylater)
{
	int ccsfd = -1;
	char query[256];
	char *val;

	log_printf(LOG_DEBUG, "Loading configuration information\n");

	if (trylater)
		if(get_logsys_config_data(&debug))
			return -1;

	ccsfd = ccs_connect();
	if (ccsfd < 0) {
		log_printf(LOG_CRIT, "Connection to CCSD failed; cannot start\n");
		return -1;
	}

	ctx->qc_interval = 1;
	ctx->qc_tko = 10;
	ctx->qc_scoremin = 0;
	ctx->qc_flags = RF_REBOOT | RF_ALLOW_KILL | RF_UPTIME;
			/* | RF_STOP_CMAN;*/
	if (debug)
		ctx->qc_flags |= RF_DEBUG;

	ctx->qc_sched = SCHED_RR;
	ctx->qc_sched_prio = 1;

	/* Get interval */
	snprintf(query, sizeof(query), "/cluster/quorumd/@interval");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_interval = atoi(val);
		free(val);
		if (ctx->qc_interval < 1)
			ctx->qc_interval = 1;
	}
		
	/* Get tko */
	snprintf(query, sizeof(query), "/cluster/quorumd/@tko");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_tko = atoi(val);
		free(val);
		if (ctx->qc_tko < 3)
			ctx->qc_tko = 3;
	}

	/* Get up-tko (transition off->online) */
	ctx->qc_tko_up = (ctx->qc_tko / 3);
	snprintf(query, sizeof(query), "/cluster/quorumd/@tko_up");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_tko_up = atoi(val);
		free(val);
	}
	if (ctx->qc_tko_up < 2)
		ctx->qc_tko_up = 2;

	/* After coming online, wait this many intervals before
	   being allowed to bid for master. */
	ctx->qc_upgrade_wait = 2; /* (ctx->qc_tko / 3); */
	snprintf(query, sizeof(query), "/cluster/quorumd/@upgrade_wait");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_upgrade_wait = atoi(val);
		free(val);
	}
	if (ctx->qc_upgrade_wait < 1)
		ctx->qc_upgrade_wait = 1;

	/* wait this many intervals after bidding for master before
	   becoming Caesar  */
	ctx->qc_master_wait = (ctx->qc_tko / 2);
	snprintf(query, sizeof(query), "/cluster/quorumd/@master_wait");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_master_wait = atoi(val);
		free(val);
	}
	if (ctx->qc_master_wait <= ctx->qc_tko_up)
		ctx->qc_master_wait = ctx->qc_tko_up + 1;
		
	/* Get votes */
	snprintf(query, sizeof(query), "/cluster/quorumd/@votes");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_votes = atoi(val);
		free(val);
		if (ctx->qc_votes < 0)
			ctx->qc_votes = 0;
	}

	/* Get device */
	snprintf(query, sizeof(query), "/cluster/quorumd/@device");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_device = val;
	}

	/* Get label (overrides device) */
	snprintf(query, sizeof(query), "/cluster/quorumd/@label");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_label = val;
	}

	/* Get status file */
	snprintf(query, sizeof(query), "/cluster/quorumd/@status_file");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_status_file = val;
	}

	/* Get status socket */
	snprintf(query, sizeof(query), "/cluster/quorumd/@status_sock");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_status_sockname = val;
	}

	/* Get min score */
	snprintf(query, sizeof(query), "/cluster/quorumd/@min_score");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_scoremin = atoi(val);
		free(val);
		if (ctx->qc_scoremin < 0)
			ctx->qc_scoremin = 0;
	}
	
	/* Get scheduling queue */
	snprintf(query, sizeof(query), "/cluster/quorumd/@scheduler");
	if (ccs_get(ccsfd, query, &val) == 0) {
		switch(val[0]) {
		case 'r':
		case 'R':
			ctx->qc_sched = SCHED_RR;
			break;
		case 'f':
		case 'F':
			ctx->qc_sched = SCHED_FIFO;
			break;
		case 'o':
		case 'O':
			ctx->qc_sched = SCHED_OTHER;
			break;
		default:
			log_printf(LOG_WARNING, "Invalid scheduling queue '%s'\n",
			       val);
			break;
		}
		free(val);
	}
	
	/* Get priority */
	snprintf(query, sizeof(query), "/cluster/quorumd/@priority");
	if (ccs_get(ccsfd, query, &val) == 0) {
		ctx->qc_sched_prio = atoi(val);
		free(val);
	}	

	/* Get reboot flag for when we transition -> offline */
	/* default = on, so, 0 to turn off */
	snprintf(query, sizeof(query), "/cluster/quorumd/@reboot");
	if (ccs_get(ccsfd, query, &val) == 0) {
		if (!atoi(val))
			ctx->qc_flags &= ~RF_REBOOT;
		free(val);
	}

	/* Get cman_label */
	snprintf(query, sizeof(query), "/cluster/quorumd/@cman_label");
	if (ccs_get(ccsfd, query, &val) == 0) {
		if (strlen(val) > 0) {
			ctx->qc_flags |= RF_CMAN_LABEL;
			ctx->qc_cman_label = val;
		}
	}
	
	/*
	 * Get flag to see if we're supposed to kill cman if qdisk is not 
	 * available.
	 */
	/* default = off, so, 1 to turn on */
	snprintf(query, sizeof(query), "/cluster/quorumd/@stop_cman");
	if (ccs_get(ccsfd, query, &val) == 0) {
		if (!atoi(val))
			ctx->qc_flags &= ~RF_STOP_CMAN;
		else
			ctx->qc_flags |= RF_STOP_CMAN;
		free(val);
	}
	
	
	/*
	 * Get flag to see if we're supposed to reboot if we can't complete
	 * a pass in failure time
	 */
	/* default = off, so, 1 to turn on */
	snprintf(query, sizeof(query), "/cluster/quorumd/@paranoid");
	if (ccs_get(ccsfd, query, &val) == 0) {
		if (!atoi(val))
			ctx->qc_flags &= ~RF_PARANOID;
		else
			ctx->qc_flags |= RF_PARANOID;
		free(val);
	}
	
	
	/*
	 * Get flag to see if we're supposed to reboot if we can't complete
	 * a pass in failure time
	 */
	/* default = off, so, 1 to turn on */
	snprintf(query, sizeof(query), "/cluster/quorumd/@allow_kill");
	if (ccs_get(ccsfd, query, &val) == 0) {
		if (!atoi(val))
			ctx->qc_flags &= ~RF_ALLOW_KILL;
		else
			ctx->qc_flags |= RF_ALLOW_KILL;
		free(val);
	}

	/*
	 * Get flag to see if we're supposed to use /proc/uptime instead of
	 * gettimeofday(2)
	 */
	/* default = off, so, 1 to turn on */
	snprintf(query, sizeof(query), "/cluster/quorumd/@use_uptime");
	if (ccs_get(ccsfd, query, &val) == 0) {
		if (!atoi(val))
			ctx->qc_flags &= ~RF_UPTIME;
		else
			ctx->qc_flags |= RF_UPTIME;
		free(val);
	}

	*cfh = configure_heuristics(ccsfd, h, maxh);

	log_printf(LOG_DEBUG,
	       "Quorum Daemon: %d heuristics, %d interval, %d tko, %d votes\n",
	       *cfh, ctx->qc_interval, ctx->qc_tko, ctx->qc_votes);
	log_printf(LOG_DEBUG, "Run Flags: %08x\n", ctx->qc_flags);

	ccs_disconnect(ccsfd);

	return 0;
}


static void
check_stop_cman(qd_ctx *ctx)
{
	if (!(ctx->qc_flags & RF_STOP_CMAN))
		return;
	
	log_printf(LOG_WARNING, "Telling CMAN to leave the cluster; qdisk is not"
		" available\n");
#if (defined(LIBCMAN_VERSION) && LIBCMAN_VERSION >= 2)
	if (cman_shutdown(ctx->qc_ch, 0) < 0) {
#else
	int x = 0;
	if (ioctl(cman_get_fd(ctx->qc_ch), SIOCCLUSTER_LEAVE_CLUSTER, &x) < 0) {
#endif
		log_printf(LOG_CRIT, "Could not leave the cluster - rebooting\n");
		sleep(5);
		if (ctx->qc_flags & RF_DEBUG)
			return;
		reboot(RB_AUTOBOOT);
	}
}


int
main(int argc, char **argv)
{
	cman_node_t me;
	int cfh, rv, forked = 0, nfd = -1, ret = -1;
	qd_ctx ctx;
	cman_handle_t ch = NULL;
	node_info_t ni[MAX_NODES_DISK];
	struct h_data h[10];
	int debug = 0, foreground = 0, trylater = 0;
	char device[128];
	pid_t pid;
	quorum_header_t qh;

	if (check_process_running(argv[0], &pid) && pid !=getpid()) {
		printf("QDisk services already running\n");
		return 0;
	}

	while ((rv = getopt(argc, argv, "fdQs")) != EOF) {
		switch (rv) {
		case 'd':
			debug = 1;
			break;
		case 'f':
			foreground = 1;
			break;
		case 'Q':
			/* Make qdisk very quiet */
			nfd = open("/dev/null", O_RDWR);
			close(0);
			close(1);
			close(2);
			dup2(nfd, 0);
			dup2(nfd, 1);
			dup2(nfd, 2);
			close(nfd);
			break;
		default:
			break;
		}
	}

	if(getenv("QDISK_DEBUGLOG"))
		debug = 1;

	if(debug)
		logsys_config_priority_set (LOG_LEVEL_DEBUG);

	trylater = get_logsys_config_data(&debug);

	if (trylater)
		logsys_config_mode_set (LOG_MODE_OUTPUT_STDERR | LOG_MODE_OUTPUT_SYSLOG_THREADED | LOG_MODE_OUTPUT_FILE | LOG_MODE_FLUSH_AFTER_CONFIG);	

#if (defined(LIBCMAN_VERSION) && LIBCMAN_VERSION >= 2)
	ch = cman_admin_init(NULL);
#else
	ch = cman_init(NULL);
#endif
	if (!ch) {
		if (!foreground && !forked) {
			if (daemon_init(argv[0]) < 0)
				goto out;
			else
				forked = 1;
		}
		
		log_printf(LOG_INFO, "Waiting for CMAN to start\n");
		
		do {
			sleep(5);
#if (defined(LIBCMAN_VERSION) && LIBCMAN_VERSION >= 2)
			ch = cman_admin_init(NULL);
#else
			ch = cman_init(NULL);
#endif
		} while (!ch);
	}

	memset(&me, 0, sizeof(me));
	while (cman_get_node(ch, CMAN_NODEID_US, &me) < 0) {
		if (!foreground && !forked) {
			if (daemon_init(argv[0]) < 0)
				goto out;
			else
				forked = 1;
		}
		sleep(5);
	}

	qd_init(&ctx, ch, me.cn_nodeid);

	signal(SIGINT, int_handler);
	signal(SIGTERM, int_handler);

	if (get_config_data(&ctx, h, 10, &cfh, debug, trylater) < 0) {
		log_printf(LOG_CRIT, "Configuration failed\n");
		check_stop_cman(&ctx);
		goto out;
	}

	if (ctx.qc_label) {
		ret = find_partitions(ctx.qc_label, device, sizeof(device), 0);
		if (ret < 0) {
			log_printf(LOG_CRIT, "Unable to match label"
					 " '%s' to any device\n",
					 ctx.qc_label);
			check_stop_cman(&ctx);
			goto out;
		}
		/* XXX Multiple matches: do we care? */

		if (ctx.qc_device)
			free(ctx.qc_device);

		ctx.qc_device = strdup(device);

		log_printf(LOG_INFO, "Quorum Partition: %s Label: %s\n",
		       ctx.qc_device, ctx.qc_label);
	} else if (ctx.qc_device) {
		if (check_device(ctx.qc_device, NULL, &qh, 0) != 0) {
			log_printf(LOG_CRIT,
			       "Specified partition %s does not have a "
			       "qdisk label\n", ctx.qc_device);
			check_stop_cman(&ctx);
			goto out;
		}

		if (qh.qh_version == VERSION_MAGIC_V2 &&
                    qh.qh_blksz != rv) {
			log_printf(LOG_CRIT,
			       "Specified device %s does match kernel's "
			       "reported sector size (%lu != %d)\n",
			       ctx.qc_device,
			       (unsigned long)ctx.qc_disk.d_blksz, rv);
			check_stop_cman(&ctx);
			goto out;
		}
	}

	if (!foreground && !forked) {
                if (daemon_init(argv[0]) < 0)
			goto out;
	}
	
	set_priority(ctx.qc_sched, ctx.qc_sched_prio);

	if (quorum_init(&ctx, ni, MAX_NODES_DISK, h, cfh) < 0) {
		log_printf(LOG_CRIT, "Initialization failed\n");
		check_stop_cman(&ctx);
		goto out;
	}

	ret = 0;

	if (!_running)
		goto out;
	
	cman_register_quorum_device(ctx.qc_ch,
				    (ctx.qc_flags&RF_CMAN_LABEL)? 
				        ctx.qc_cman_label:
                                        ctx.qc_device,
				    ctx.qc_votes);
	/*
		XXX this always returns -1 / EBUSY even when it works?!!!
		
	if ((rv = cman_register_quorum_device(ctx.qc_ch, ctx.qc_device,
					      ctx.qc_votes)) < 0) {
		log_printf(LOG_CRIT,
				 "Could not register %s with CMAN; "
				 "return = %d; error = %s\n",
				 ctx.qc_device, rv, strerror(errno));
		goto out;
	}
	*/
	if (quorum_loop(&ctx, ni, MAX_NODES_DISK) == 0)
		cman_unregister_quorum_device(ctx.qc_ch);

	quorum_logout(&ctx);
	/* free cman handle to avoid leak in cman */
out:
	cman_finish(ctx.qc_ch);
	qd_destroy(&ctx);

	return ret;
}
