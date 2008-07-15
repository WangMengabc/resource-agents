#ifndef __DLM_DAEMON_DOT_H__
#define __DLM_DAEMON_DOT_H__

#include <sys/types.h>
#include <asm/types.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <netdb.h>
#include <limits.h>
#include <unistd.h>
#include <time.h>
#include <syslog.h>
#include <sched.h>
#include <signal.h>
#include <sys/time.h>
#include <dirent.h>
#include <openais/saAis.h>
#include <openais/saCkpt.h>
#include <openais/cpg.h>

#include <linux/dlmconstants.h>
#include "libdlmcontrol.h"
#include "dlm_controld.h"
#include "list.h"
#include "linux_endian.h"

/* DLM_LOCKSPACE_LEN: maximum lockspace name length, from linux/dlmconstants.h.
   Copied in libdlm.h so apps don't need to include the kernel header.
   The libcpg limit is larger at CPG_MAX_NAME_LENGTH 128.  Our cpg name includes
   a "dlm:" prefix before the lockspace name. */

/* Maximum members of a lockspace, should match CPG_MEMBERS_MAX in openais/cpg.h.
   There are no max defines in dlm-kernel for lockspace members. */

#define MAX_NODES	128

/* Maximum number of IP addresses per node, when using SCTP and multi-ring in
   openais.  In dlm-kernel this is DLM_MAX_ADDR_COUNT, currently 3. */

#define MAX_NODE_ADDRESSES 4

/* Max string length printed on a line, for debugging/dump output. */

#define MAXLINE		256

extern int daemon_debug_opt;
extern int daemon_quit;
extern int poll_fencing;
extern int poll_quorum;
extern int poll_fs;
extern int poll_ignore_plock;
extern int plock_fd;
extern int plock_ci;
extern struct list_head lockspaces;
extern int cman_quorate;
extern int our_nodeid;
extern char daemon_debug_buf[256];
extern char dump_buf[DLMC_DUMP_SIZE];
extern int dump_point;
extern int dump_wrap;
extern char plock_dump_buf[DLMC_DUMP_SIZE];
extern int plock_dump_len;

void daemon_dump_save(void);

#define log_debug(fmt, args...) \
do { \
	snprintf(daemon_debug_buf, 255, "%ld " fmt "\n", time(NULL), ##args); \
	if (daemon_debug_opt) fprintf(stderr, "%s", daemon_debug_buf); \
	daemon_dump_save(); \
} while (0)

#define log_group(ls, fmt, args...) \
do { \
	snprintf(daemon_debug_buf, 255, "%ld %s " fmt "\n", time(NULL), \
		 (ls)->name, ##args); \
	if (daemon_debug_opt) fprintf(stderr, "%s", daemon_debug_buf); \
	daemon_dump_save(); \
} while (0)

#define log_error(fmt, args...) \
do { \
	log_debug(fmt, ##args); \
	syslog(LOG_ERR, fmt, ##args); \
} while (0)

#define log_plock(ls, fmt, args...) \
do { \
	snprintf(daemon_debug_buf, 255, "%ld %s " fmt "\n", time(NULL), \
		 (ls)->name, ##args); \
	if (daemon_debug_opt && cfgd_plock_debug) fprintf(stderr, "%s", daemon_debug_buf); \
} while (0)

/* dlm_header types */
enum {
	DLM_MSG_START = 1,
	DLM_MSG_PLOCK,
	DLM_MSG_PLOCK_OWN,
	DLM_MSG_PLOCK_DROP,
	DLM_MSG_PLOCK_SYNC_LOCK,
	DLM_MSG_PLOCK_SYNC_WAITER,
	DLM_MSG_PLOCKS_STORED,
	DLM_MSG_DEADLK_CYCLE_START,
	DLM_MSG_DEADLK_CYCLE_END,
	DLM_MSG_DEADLK_CHECKPOINT_READY,
	DLM_MSG_DEADLK_CANCEL_LOCK
};

/* dlm_header flags */
#define DLM_MFLG_JOINING   1  /* accompanies start, we are joining */
#define DLM_MFLG_HAVEPLOCK 2  /* accompanies start, we have plock state */

struct dlm_header {
	uint16_t version[3];
	uint16_t type;	  	/* DLM_MSG_ */
	uint32_t nodeid;	/* sender */
	uint32_t to_nodeid;     /* recipient, 0 for all */
	uint32_t global_id;     /* global unique id for this lockspace */
	uint32_t flags;		/* DLM_MFLG_ */
	uint32_t msgdata;       /* in-header payload depends on MSG type; lkid
				   for deadlock, seq for lockspace membership */
	uint32_t pad1;
	uint64_t pad2;
};

struct lockspace {
	struct list_head	list;
	char			name[DLM_LOCKSPACE_LEN+1];
	uint32_t		global_id;

	/* lockspace membership stuff */

	cpg_handle_t		cpg_handle;
	int			cpg_client;
	int			cpg_fd;
	int			joining;
	int			leaving;
	int			kernel_stopped;
	int			fs_registered;
	uint32_t		change_seq;
	uint32_t		started_count;
	struct change		*started_change;
	struct list_head	changes;
	struct list_head	node_history;

	/* plock stuff */

	int			plock_ckpt_node;
	int			need_plocks;
	int			save_plocks;
	uint32_t		associated_mg_id;
	struct list_head	saved_messages;
	struct list_head	plock_resources;
	time_t			last_checkpoint_time;
	time_t			last_plock_time;
	struct timeval		drop_resources_last;
	uint64_t		plock_ckpt_handle;

	/* deadlock stuff */

	int			deadlk_low_nodeid;
	struct list_head	deadlk_nodes;
	uint64_t		deadlk_ckpt_handle;
	int			deadlk_confchg_init;
	struct list_head	transactions;
	struct list_head	resources;
	struct timeval		cycle_start_time;
	struct timeval		cycle_end_time;
	struct timeval		last_send_cycle_start;
	int			cycle_running;
	int			all_checkpoints_ready;
};

/* action.c */
void set_associated_id(uint32_t mg_id);
int set_sysfs_control(char *name, int val);
int set_sysfs_event_done(char *name, int val);
int set_sysfs_id(char *name, uint32_t id);
int set_configfs_members(char *name, int new_count, int *new_members,
			int renew_count, int *renew_members);
int add_configfs_node(int nodeid, char *addr, int addrlen, int local);
void del_configfs_node(int nodeid);
void clear_configfs(void);
int setup_configfs(void);

/* config.c */
int get_weight(int nodeid, char *lockspace);
int setup_ccs(void);
void close_ccs(void);

/* cpg.c */
int setup_cpg(void);
void process_lockspace_changes(void);
void dlm_send_message(struct lockspace *ls, char *buf, int len);
int dlm_join_lockspace(struct lockspace *ls);
int dlm_leave_lockspace(struct lockspace *ls);
char *msg_name(int type);
void update_flow_control_status(void);
int set_node_info(struct lockspace *ls, int nodeid, struct dlmc_node *node);
int set_lockspace_info(struct lockspace *ls, struct dlmc_lockspace *lockspace);
int set_lockspaces(int *count, struct dlmc_lockspace **lss_out);
int set_lockspace_nodes(struct lockspace *ls, int option, int *node_count,
			struct dlmc_node **nodes_out);
int set_fs_notified(struct lockspace *ls, int nodeid);

/* deadlock.c */
void setup_deadlock(void);
void send_cycle_start(struct lockspace *ls);
void receive_checkpoint_ready(struct lockspace *ls, struct dlm_header *hd,
			int len);
void receive_cycle_start(struct lockspace *ls, struct dlm_header *hd, int len);
void receive_cycle_end(struct lockspace *ls, struct dlm_header *hd, int len);
void receive_cancel_lock(struct lockspace *ls, struct dlm_header *hd, int len);

/* main.c */
int do_read(int fd, void *buf, size_t count);
int do_write(int fd, void *buf, size_t count);
void client_dead(int ci);
int client_add(int fd, void (*workfn)(int ci), void (*deadfn)(int ci));
int client_fd(int ci);
void client_ignore(int ci, int fd);
void client_back(int ci, int fd);
struct lockspace *find_ls(char *name);
struct lockspace *find_ls_id(uint32_t id);
char *dlm_mode_str(int mode);
void cluster_dead(int ci);

/* member_cman.c */
int setup_cman(void);
void close_cman(void);
void process_cman(int ci);
void cman_statechange(void);
int is_cman_member(int nodeid);
char *nodeid2name(int nodeid);

/* netlink.c */
int setup_netlink(void);
void process_netlink(int ci);

/* plock.c */
int setup_plocks(void);
void process_plocks(int ci);
int limit_plocks(void);
void receive_plock(struct lockspace *ls, struct dlm_header *hd, int len);
void receive_own(struct lockspace *ls, struct dlm_header *hd, int len);
void receive_sync(struct lockspace *ls, struct dlm_header *hd, int len);
void receive_drop(struct lockspace *ls, struct dlm_header *hd, int len);
void process_saved_plocks(struct lockspace *ls);
void close_plock_checkpoint(struct lockspace *ls);
void store_plocks(struct lockspace *ls);
void retrieve_plocks(struct lockspace *ls);
void purge_plocks(struct lockspace *ls, int nodeid, int unmount);
int fill_plock_dump_buf(struct lockspace *ls);

/* group.c */
int setup_groupd(void);
void close_groupd(void);
void process_groupd(int ci);
int dlm_join_lockspace_group(struct lockspace *ls);
int dlm_leave_lockspace_group(struct lockspace *ls);
int set_node_info_group(struct lockspace *ls, int nodeid,
			struct dlmc_node *node);
int set_lockspace_info_group(struct lockspace *ls,
			struct dlmc_lockspace *lockspace);
int set_lockspaces_group(int *count, struct dlmc_lockspace **lss_out);
int set_lockspace_nodes_group(struct lockspace *ls, int option, int *node_count,
			struct dlmc_node **nodes);

#endif
