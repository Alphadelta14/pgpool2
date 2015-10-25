/* -*-pgsql-c-*- */
/*
 * $Header$
 *
 * pgpool: a language independent connection pool server for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2015	PgPool Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * watchdog.c: child process main
 *
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <ifaddrs.h>

#include "pool.h"
#include "auth/md5.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/elog.h"
#include "utils/json_writer.h"
#include "utils/json.h"
#include "pool_config.h"

#include "watchdog/wd_utils.h"
#include "watchdog/watchdog.h"
#include "watchdog/wd_json_data.h"
#include "watchdog/wd_ipc_defines.h"
#include "watchdog/wd_ipc_commands.h"
#include "parser/stringinfo.h"

typedef enum IPC_CMD_PREOCESS_RES
{
	IPC_CMD_COMPLETE,
	IPC_CMD_PROCESSING,
	IPC_CMD_ERROR
}IPC_CMD_PREOCESS_RES;

#define MIN_SECS_CONNECTION_RETRY	10 /* Time in seconds to retry connection with node once it was failed */


#define WD_NO_MESSAGE						0
#define WD_ADD_NODE_MESSAGE					'A'
#define WD_REQ_INFO_MESSAGE					'B'
#define WD_DECLARE_COORDINATOR_MESSAGE		'C'
#define WD_DATA_MESSAGE						'D'
#define WD_ERROR_MESSAGE					'E'
#define WD_ACCEPT_MESSAGE					'G'
#define WD_INFO_MESSAGE						'I'
#define WD_JOIN_COORDINATOR_MESSAGE			'J'
#define WD_INTERLOCKING_REQUEST				'L'
#define WD_IAM_COORDINATOR_MESSAGE			'M'
#define WD_IAM_IN_NW_TROUBLE_MESSAGE		'N'
#define WD_QUORUM_IS_LOST					'Q'
#define WD_REJECT_MESSAGE					'R'
#define WD_STAND_FOR_COORDINATOR_MESSAGE	'S'
#define WD_INTERUNLOCKING_REQUEST			'U'
#define WD_REPLICATE_VARIABLE_REQUEST		'V'
#define WD_INFORM_I_AM_GOING_DOWN			'X'
#define WD_ASK_FOR_POOL_CONFIG				'Y'
#define WD_POOL_CONFIG_DATA					'Z'

typedef struct packet_types
{
	char type;
	char name[100];
}packet_types;
packet_types all_packet_types[] = {
	{WD_ADD_NODE_MESSAGE, "ADD NODE"},
	{WD_REQ_INFO_MESSAGE, "REQUEST INFO"},
	{WD_DECLARE_COORDINATOR_MESSAGE, "DECLARE COORDINATOR"},
	{WD_DATA_MESSAGE, "DATA"},
	{WD_ERROR_MESSAGE, "ERROR"},
	{WD_ACCEPT_MESSAGE, "ACCEPT"},
	{WD_INFO_MESSAGE, "NODE INFO"},
	{WD_JOIN_COORDINATOR_MESSAGE, "JOIN COORDINATOR"},
	{WD_INTERLOCKING_REQUEST, "INTERLOCKING REQUEST"},
	{WD_IAM_COORDINATOR_MESSAGE, "IAM COORDINATOR"},
	{WD_IAM_IN_NW_TROUBLE_MESSAGE, "I AM IN NETWORK TROUBLE"},
	{WD_QUORUM_IS_LOST, "QUORUM IS LOST"},
	{WD_REJECT_MESSAGE, "REJECT"},
	{WD_STAND_FOR_COORDINATOR_MESSAGE, "STAND FOR COORDINATOR"},
	
	{WD_INTERUNLOCKING_REQUEST, "INTERUNLOCKING REQUEST"},
	{WD_REPLICATE_VARIABLE_REQUEST, "REPLICATE VARIABLE REQUEST"},
	{WD_INFORM_I_AM_GOING_DOWN, "INFORM I AM GOING DOWN"},
	{WD_ASK_FOR_POOL_CONFIG, "ASK FOR POOL CONFIG"},
	{WD_POOL_CONFIG_DATA, "CONFIG DATA"},
	{WD_NO_MESSAGE,""}
};

char *wd_event_name[] =
{	"WD_EVENT_WD_STATE_CHANGED",
	"WD_EVENT_CON_OPEN",
	"WD_EVENT_CON_CLOSED",
	"WD_EVENT_CON_ERROR",
	"WD_EVENT_TIMEOUT",
	"WD_EVENT_PACKET_RCV",
	"WD_EVENT_COMMAND_FINISHED",
	"WD_EVENT_NEW_OUTBOUND_CONNECTION",
	"WD_EVENT_NW_IP_IS_REMOVED",
	"WD_EVENT_NW_IP_IS_ASSIGNED",
	"WD_EVENT_LOCAL_NODE_LOST",
	"WD_EVENT_REMOTE_NODE_LOST",
	"WD_EVENT_REMOTE_NODE_FOUND",
	"WD_EVENT_LOCAL_NODE_FOUND"
};

char *debug_states[] = {
	"WD_DEAD",
	"WD_LOADING",
	"WD_JOINING",
	"WD_INITIALIZING",
	"WD_WAITING_CONNECT",
	"WD_COORDINATOR",
	"WD_PARTICIPATE_IN_ELECTION",
	"WD_STAND_FOR_COORDINATOR",
	"WD_STANDBY",
	"WD_WAITING_FOR_QUORUM",
	"WD_LOST",
	"WD_IN_NW_TROUBLE",
	"WD_ADD_MESSAGE_SENT"};

typedef struct WDPacketData
{
	char	type;
	int		command_id;
	int		len;
	char*	data;
}WDPacketData;


typedef enum WDNodeCommandState
{
	COMMAMD_STATE_INIT,
	COMMAND_STATE_SENT,
	COMMAND_STATE_REPLIED,
	COMMAND_STATE_SEND_ERROR,
	COMMAND_STATE_DO_NOT_SEND
}WDNodeCommandState;

typedef struct WDCommandNodeResult
{
	WatchdogNode* wdNode;
	WDNodeCommandState cmdState;
	char	result_type;
	int		result_data_len;
	char*	result_data;
}WDCommandNodeResult;


typedef struct WDIPCCommandData
{
	WD_COMMAND_ACTIONS command_action;
	MemoryContext		memoryContext;
	int					issueing_sock;
	char				type;
	struct timeval		issue_time;
	unsigned int		internal_command_id;
	int					data_len;
	char				*data_buf;
	
	unsigned int	sendTo_count;
	unsigned int	reply_from_count;
	unsigned int	timeout_secs;

	WDCommandNodeResult*	nodeResults;
}WDIPCCommandData;

typedef struct WDFunctionCommandData
{
	char				commandType;
	unsigned int		commandID;
	char*				funcName;
	WatchdogNode*		wdNode;
}WDFunctionCommandData;

typedef struct WDCommandTimerData
{
	struct timeval  startTime;
	unsigned int	expire_sec;
	bool			need_tics;
	WDFunctionCommandData*	wd_func_command;
}WDCommandTimerData;

typedef struct InterlockingNode
{
	WatchdogNode*	lockHolderNode;
	bool			locked;
}InterlockingNode;


typedef enum WDCommandStatus
{
	COMMAND_EMPTY,
	COMMAND_IN_PROGRESS,
	COMMAND_FINISHED_TIMEOUT,
	COMMAND_FINISHED_ALL_REPLIED,
	COMMAND_FINISHED_NODE_REJECTED
}WDCommandStatus;

typedef struct WDCommandData
{
	WDPacketData			packet;
	WDCommandNodeResult		*nodeResults;
	WatchdogNode			*sendToNode;	/* NULL means send to all */
	WDCommandStatus			commandStatus;
	unsigned int			commandTimeoutSecs;
	struct timeval			commandTime;
	unsigned int			commandSendToCount;
	unsigned int			commandReplyFromCount;
	int						commandFinished;
	int						partial_sent;
}WDCommandData;


typedef struct wd_cluster
{
	WatchdogNode*		localNode;
	WatchdogNode*		remoteNodes;
	WatchdogNode*		masterNode;
	WatchdogNode*		lockHolderNode;
	InterlockingNode	interlockingNodes[MAX_FAILOVER_CMDS];
	int					remoteNodeCount;
	int					aliveNodeCount;
	bool				quorum_exists;
	WDCommandData		currentCommand;
	unsigned int		nextCommandID;
	pid_t				escalation_pid;
	int				command_server_sock;
	int				network_monitor_sock;
	bool			holding_vip;
	bool			network_error;
	struct timeval  network_error_time;
	
	List			*unidentified_socks;
	List			*notify_clients;
	List			*ipc_command_socks;
	List			*ipc_commands;
	List			*wd_timer_commands;
}wd_cluster;

int	*escalation_status = NULL; /* Lives in shared memory */


volatile sig_atomic_t reload_config_signal = 0;

static void check_config_reload(void);
static RETSIGTYPE reload_config_handler(int sig);
static void FileUnlink(int code, Datum path);
static void wd_child_exit(int exit_signo);

static void wd_cluster_initialize(void);
static int wd_create_client_socket(char * hostname, int port, bool *connected);
static int read_from_socket(int sock, void* buf, size_t len, int timeout);
static int connect_with_all_configured_nodes(void);
static void try_connecting_with_all_unreachable_nodes(void);
static bool connect_to_node(WatchdogNode* wdNode);
static bool is_socket_connection_connected(SocketConnection* conn);

static int update_successful_outgoing_cons(fd_set* wmask, int pending_fds_count);
static int set_local_node_state(WD_STATES newState);
static int prepare_fds(fd_set* rmask, fd_set* wmask, fd_set* emask);

static WDPacketData* get_addnode_message(void);
static WDPacketData* get_mynode_info_message(WDPacketData* replyFor);
static WDPacketData* get_minimum_message(char type, WDPacketData* replyFor);

static void set_next_commandID_in_message(WDPacketData* pkt);
static void set_message_commandID(WDPacketData* pkt, unsigned int commandID);
static void set_message_data(WDPacketData* pkt, const char* data, int len);
static void set_message_type(WDPacketData* pkt, char type);
static void free_packet(WDPacketData *pkt);

static WDPacketData* get_empty_packet(void);
static WDPacketData* read_packet_of_type(SocketConnection* conn, char ensure_type);
static WDPacketData* read_packet(SocketConnection* conn);

static int issue_watchdog_internal_command(WatchdogNode* wdNode, WDPacketData *pkt, int timeout_sec);
static char get_current_command_resultant_message_type(void);
static void check_for_current_command_timeout(void);
static bool watchdog_internal_command_packet_processor(WatchdogNode* wdNode, WDPacketData* pkt);


static unsigned int get_next_commandID(void);
static WatchdogNode* parse_node_info_message(WDPacketData* pkt, char **authkey);
static int get_quorum_status(void);
static int get_mimimum_nodes_required_for_quorum(void);

static bool write_packet_to_socket(int sock, WDPacketData* pkt);
static int read_sockets(fd_set* rmask,int pending_fds_count);
static void set_timeout(unsigned int sec);
static int wd_create_command_server_socket(void);
static void close_socket_connection(SocketConnection* conn);
static bool send_message_to_connection(SocketConnection* conn, WDPacketData *pkt);

static int send_message(WatchdogNode* wdNode, WDPacketData *pkt);
static bool send_message_to_node(WatchdogNode* wdNode, WDPacketData *pkt);
static bool reply_with_minimal_message(WatchdogNode* wdNode, char type, WDPacketData* replyFor);
static bool reply_with_message(WatchdogNode* wdNode, char type, char* data, int data_len, WDPacketData* replyFor);
static int send_cluster_command(WatchdogNode* wdNode, char type, int timeout_sec);
static int accept_incomming_connections(fd_set* rmask, int pending_fds_count);

static int standard_packet_processor(WatchdogNode* wdNode, WDPacketData* pkt);
static int update_connected_node_count(void);
static int get_cluster_node_count(void);
static void clear_command_node_result(WDCommandNodeResult* nodeResult);
static void clear_current_command(void);

static inline WD_STATES get_local_node_state(void);
static int set_state(WD_STATES newState);

static int watchdog_state_machine_standby(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);
static int watchdog_state_machine_voting(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);
static int watchdog_state_machine_coordinator(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);
static int watchdog_state_machine_standForCord(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);
static int watchdog_state_machine_initializing(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);
static int watchdog_state_machine_joining(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);
static int watchdog_state_machine_loading(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);
static int watchdog_state_machine(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);
static int watchdog_state_machine_waiting_for_quorum(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);
static int watchdog_state_machine_nw_error(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);

static void cleanUpIPCCommand(WDIPCCommandData* ipcCommand);
static bool read_ipc_command_and_process(int socket, bool *remove_socket);

static JsonNode* get_node_list_json(int id);
static bool add_nodeinfo_to_json(JsonNode* jNode, WatchdogNode* node);
static bool fire_node_status_event(int nodeID, int nodeStatus);
static void resign_from_coordinator(void);
static void init_wd_packet(WDPacketData* pkt);
static WDIPCCommandData* get_wd_IPC_command_from_reply(WDPacketData* pkt);
static WDIPCCommandData* get_wd_IPC_command_from_socket(int sock);
static bool wd_commands_packet_processor(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt);

static IPC_CMD_PREOCESS_RES process_IPC_command(WDIPCCommandData* IPCCommand);
static IPC_CMD_PREOCESS_RES process_IPC_nodeStatusChange_command(WDIPCCommandData* IPCCommand);
static IPC_CMD_PREOCESS_RES process_IPC_lock_request(WDIPCCommandData *IPCCommand);
static IPC_CMD_PREOCESS_RES process_IPC_nodeList_command(WDIPCCommandData* IPCCommand);
static IPC_CMD_PREOCESS_RES process_IPC_unlock_request(WDIPCCommandData *IPCCommand);
static IPC_CMD_PREOCESS_RES process_IPC_replicate_variable(WDIPCCommandData* IPCCommand);
static IPC_CMD_PREOCESS_RES process_IPC_failover_cmd_synchronise(WDIPCCommandData *IPCCommand);
static IPC_CMD_PREOCESS_RES execute_replicate_command(WDIPCCommandData* ipcCommand);

static int node_has_requested_for_interlocking(WatchdogNode* wdNode, WDPacketData* pkt);
static bool node_has_resigned_from_interlocking(WatchdogNode* wdNode, WDPacketData* pkt);

static void process_wd_func_commands_for_timer_events(void);
static void add_wd_command_for_timer_events(unsigned int expire_secs, bool need_tics, WDFunctionCommandData* wd_func_command);
static bool reply_is_received_for_pgpool_replicate_command(WatchdogNode* wdNode, WDPacketData* pkt, WDIPCCommandData* ipcCommand);
static bool process_wd_command_function(WatchdogNode* wdNode, WDPacketData* pkt, char* func_name, int node_count, int* node_id_list);
static bool process_pgpool_replicate_command(WatchdogNode* wdNode, WDPacketData* pkt);

static void process_failover_command_sync_requests(WatchdogNode* wdNode, WDPacketData* pkt, WDIPCCommandData* ipcCommand);
static WDFailoverCMDResults node_is_asking_for_failover_cmd_end(WatchdogNode* wdNode, WDPacketData* pkt, int failoverCmdType, bool resign);
static WDFailoverCMDResults node_is_asking_for_failover_cmd_start(WatchdogNode* wdNode, WDPacketData* pkt, int failoverCmdType, bool check);
static void wd_system_will_go_down(int code, Datum arg);
static bool verify_pool_configurations(POOL_CONFIG* config);

static bool get_authhash_for_node(WatchdogNode* wdNode, char* authhash);
static bool verify_authhash_for_node(WatchdogNode* wdNode, char* authhash);

static void print_watchdog_node_info(WatchdogNode* wdNode);
static int wd_create_recv_socket(int port);
static void wd_check_config(void);
static pid_t watchdog_main(void);
static pid_t fork_watchdog_child(void);

static void print_packet_info(WDPacketData* pkt,WatchdogNode* wdNode);
/* global variables */
wd_cluster g_cluster;
struct timeval g_tm_set_time;
int g_timeout_sec = 0;


static unsigned int get_next_commandID(void)
{
	return ++g_cluster.nextCommandID;
}

static void set_timeout(unsigned int sec)
{
	g_timeout_sec = sec;
	gettimeofday(&g_tm_set_time,NULL);
}

pid_t initialize_watchdog(void)
{
	if (!pool_config->use_watchdog)
		return -1;
	/* check pool_config data related to watchdog */
	wd_check_config();
	return fork_watchdog_child();
}

static void
wd_check_config(void)
{
	if (pool_config->wd_remote_nodes.num_wd == 0)
		ereport(ERROR,
				(errmsg("invalid watchdog configuration. other pgpools setting is not defined")));
	
	if (strlen(pool_config->wd_authkey) > MAX_PASSWORD_SIZE)
		ereport(ERROR,
				(errmsg("invalid watchdog configuration. wd_authkey length can't be larger than %d",
						MAX_PASSWORD_SIZE)));
	if (!strcmp(pool_config->wd_lifecheck_method, MODE_HEARTBEAT))
	{
		if (pool_config->num_hb_if  <= 0)
			ereport(ERROR,
					(errmsg("invalid lifecheck configuration. no heartbeat interfaces defined")));
	}
}

static void wd_cluster_initialize(void)
{
	int i = 0;
	
	if (pool_config->wd_remote_nodes.num_wd <= 0)
	{
		/* should also have upper limit???*/
		ereport(ERROR,
				(errmsg("initializing watchdog failed. no watchdog nodes configured")));
	}
	/* initialize local node settings */
	g_cluster.localNode = palloc0(sizeof(WatchdogNode));
	g_cluster.localNode->wd_port = pool_config->wd_port;
	g_cluster.localNode->wd_priority = pool_config->wd_priority;
	g_cluster.localNode->pgpool_port = pool_config->port;
	g_cluster.localNode->private_id = 0;
	strcpy(g_cluster.localNode->hostname, pool_config->wd_hostname);
	strcpy(g_cluster.localNode->delegate_ip, pool_config->delegate_IP);
	/* Assign the node name */
	{
		struct utsname unameData;
		uname(&unameData);
		snprintf(g_cluster.localNode->nodeName, WD_MAX_HOST_NAMELEN, "%s_%s_%d",unameData.sysname,unameData.nodename,pool_config->port);
		/* should also have upper limit???*/
		ereport(LOG,
				(errmsg("setting the local watchdog node name to \"%s\"",g_cluster.localNode->nodeName)));
	}
	
	/* initialize remote nodes */
	g_cluster.remoteNodeCount = pool_config->wd_remote_nodes.num_wd;
	g_cluster.remoteNodes = palloc0((sizeof(WatchdogNode) * g_cluster.remoteNodeCount));
	
	ereport(LOG,
			(errmsg("watchdog cluster configured with %d remote nodes",g_cluster.remoteNodeCount)));
	
	for ( i = 0; i < pool_config->wd_remote_nodes.num_wd; i++)
	{
		g_cluster.remoteNodes[i].wd_port = pool_config->wd_remote_nodes.wd_remote_node_info[i].wd_port;
		g_cluster.remoteNodes[i].private_id = i+1;
		g_cluster.remoteNodes[i].pgpool_port = pool_config->wd_remote_nodes.wd_remote_node_info[i].pgpool_port;
		strcpy(g_cluster.remoteNodes[i].hostname, pool_config->wd_remote_nodes.wd_remote_node_info[i].hostname);
		g_cluster.remoteNodes[i].delegate_ip[0] = '\0';	/*this will be populated by remote node*/
		
		ereport(LOG,
				(errmsg("watchdog remote node:%d on %s:%d",i,g_cluster.remoteNodes[i].hostname, g_cluster.remoteNodes[i].wd_port)));
		
	}
	
	escalation_status = pool_shared_memory_create(sizeof(int));
	*escalation_status = 0;
	
	g_cluster.masterNode = NULL;
	g_cluster.aliveNodeCount = 0;
	g_cluster.quorum_exists = false;
	g_cluster.nextCommandID = 1;
	g_cluster.unidentified_socks = NULL;
	g_cluster.command_server_sock = 0;
	g_cluster.notify_clients = NULL;
	g_cluster.ipc_command_socks = NULL;
	g_cluster.wd_timer_commands = NULL;
	g_cluster.localNode->state = WD_DEAD;
	
	/* initialize the memory for command object */
	g_cluster.currentCommand.nodeResults = palloc0((sizeof(WDCommandNodeResult) * g_cluster.remoteNodeCount));
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		g_cluster.currentCommand.nodeResults[i].wdNode = &g_cluster.remoteNodes[i];
		clear_command_node_result(&g_cluster.currentCommand.nodeResults[i]);
	}
}

static void clear_command_node_result(WDCommandNodeResult* nodeResult)
{
	nodeResult->result_type = WD_NO_MESSAGE;
	nodeResult->result_data = NULL;
	nodeResult->result_data_len = 0;
	nodeResult->cmdState = COMMAMD_STATE_INIT;
}

static int
wd_create_recv_socket(int port)
{
	size_t	len = 0;
	struct sockaddr_in addr;
	int one = 1;
	int sock = -1;
	
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		/* socket create failed */
		ereport(ERROR,
				(errmsg("failed to create watchdog receive socket"),
				 errdetail("create socket failed with reason: \"%s\"", strerror(errno))));
	}
	if ( fcntl(sock, F_SETFL, O_NONBLOCK) == -1)
	{
		/* failed to set nonblock */
		close(sock);
		ereport(ERROR,
				(errmsg("failed to create watchdog receive socket"),
				 errdetail("setting non blocking mode on socket failed with reason: \"%s\"", strerror(errno))));
	}
	if ( setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one)) == -1 )
	{
		/* setsockopt(SO_REUSEADDR) failed */
		close(sock);
		ereport(ERROR,
				(errmsg("failed to create watchdog receive socket"),
				 errdetail("setsockopt(SO_REUSEADDR) failed with reason: \"%s\"", strerror(errno))));
	}
	if ( setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one)) == -1 )
	{
		/* setsockopt(TCP_NODELAY) failed */
		close(sock);
		ereport(ERROR,
				(errmsg("failed to create watchdog receive socket"),
				 errdetail("setsockopt(TCP_NODELAY) failed with reason: \"%s\"", strerror(errno))));
	}
	if ( setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &one, sizeof(one)) == -1 )
	{
		/* setsockopt(SO_KEEPALIVE) failed */
		close(sock);
		ereport(ERROR,
				(errmsg("failed to create watchdog receive socket"),
				 errdetail("setsockopt(SO_KEEPALIVE) failed with reason: \"%s\"", strerror(errno))));
	}
	
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	len = sizeof(struct sockaddr_in);
	
	if ( bind(sock, (struct sockaddr *) & addr, len) < 0 )
	{
		/* bind failed */
		char *host = "", *serv = "";
		char hostname[NI_MAXHOST], servname[NI_MAXSERV];
		if (getnameinfo((struct sockaddr *) &addr, len, hostname, sizeof(hostname), servname, sizeof(servname), 0) == 0) {
			host = hostname;
			serv = servname;
		}
		close(sock);
		ereport(ERROR,
				(errmsg("failed to create watchdog receive socket"),
				 errdetail("bind on \"%s:%s\" failed with reason: \"%s\"", host, serv, strerror(errno))));
	}
	
	if ( listen(sock, MAX_WATCHDOG_NUM * 2) < 0 )
	{
		/* listen failed */
		close(sock);
		ereport(ERROR,
				(errmsg("failed to create watchdog receive socket"),
				 errdetail("listen failed with reason: \"%s\"", strerror(errno))));
	}
	
	return sock;
}



/*
 * creates a socket in non blocking mode and connects it to the hostname and port
 * the out parameter connected is set to true if the connection is successfull
 */
static int
wd_create_client_socket(char * hostname, int port, bool *connected)
{
	int sock;
	int one = 1;
	size_t len = 0;
	struct sockaddr_in addr;
	struct hostent * hp;
	
	*connected = false;
	/* create socket */
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		/* socket create failed */
		printf("create socket failed with reason: \"%s\"\n", strerror(errno));
		return -1;
	}
	
	/* set socket option */
	if ( setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one)) == -1 )
	{
		close(sock);
		ereport(LOG,
				(errmsg("failed to set socket options"),
				 errdetail("setsockopt(TCP_NODELAY) failed with error: \"%s\"", strerror(errno))));
		return -1;
	}
	if ( setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &one, sizeof(one)) == -1 )
	{
		ereport(LOG,
				(errmsg("failed to set socket options"),
				 errdetail("setsockopt(SO_KEEPALIVE) failed with error: \"%s\"", strerror(errno))));
		close(sock);
		return -1;
	}
	
	/* set sockaddr_in */
	memset(&addr,0,sizeof(addr));
	addr.sin_family = AF_INET;
	hp = gethostbyname(hostname);
	if ((hp == NULL) || (hp->h_addrtype != AF_INET))
	{
		hp = gethostbyaddr(hostname,strlen(hostname),AF_INET);
		if ((hp == NULL) || (hp->h_addrtype != AF_INET))
		{
			ereport(LOG,
					(errmsg("failed to host"),
					 errdetail("gethostbyaddr failed with error: \"%s\"", hstrerror(h_errno))));
			close(sock);
			return -1;
		}
	}
	memmove((char *)&(addr.sin_addr), (char *)hp->h_addr, hp->h_length);
	addr.sin_port = htons(port);
	len = sizeof(struct sockaddr_in);
	
	/* set socket to non blocking */
	int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);
	
	if (connect(sock,(struct sockaddr*)&addr, len) < 0)
	{
		if (errno == EINPROGRESS)
		{
			return sock;
		}
		if (errno == EISCONN)
		{
			*connected = true;
			/* set socket to blocking again */
			flags = fcntl(sock, F_GETFL, 0);
			fcntl(sock, F_SETFL, flags | ~O_NONBLOCK);
			
			return sock;
		}
		ereport(LOG,
				(errmsg("connect on socket failed"),
				 errdetail("connect failed with error: \"%s\"", strerror(errno))));
		close(sock);
		return -1;
	}
	*connected = true;
	return sock;
}

/* returns the number of successfull connections */
static int
connect_with_all_configured_nodes(void)
{
	int connect_count = 0;
	int i;
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
		if (connect_to_node(wdNode))
			connect_count++;
	}
	return connect_count;
}

/*
 * Function tries to connect with nodes which have both sockets
 * disconnected
 */
static void
try_connecting_with_all_unreachable_nodes(void)
{
	int i;
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
		if ((wdNode->client_socket.sock_state != WD_SOCK_WAITING_FOR_CONNECT || wdNode->client_socket.sock_state != WD_SOCK_CONNECTED) &&
			(wdNode->server_socket.sock_state != WD_SOCK_WAITING_FOR_CONNECT || wdNode->server_socket.sock_state != WD_SOCK_CONNECTED))
		{
			wdNode->state = WD_DEAD;
			connect_to_node(wdNode);
		}
	}
}

/*
 * returns true if the connection is in progress or connected successfully
 * false is returned in case of failure
 */
static bool connect_to_node(WatchdogNode* wdNode)
{
	bool connected = false;
	wdNode->client_socket.sock = wd_create_client_socket(wdNode->hostname, wdNode->wd_port, &connected);
	gettimeofday(&wdNode->client_socket.tv, NULL);
	if (wdNode->client_socket.sock <= 0)
	{
		wdNode->client_socket.sock_state = WD_SOCK_ERROR;
		ereport(DEBUG1,
				(errmsg("outbound connection to \"%s:%d\" failed", wdNode->hostname, wdNode->wd_port)));
	}
	else
	{
		if (connected)
			wdNode->client_socket.sock_state = WD_SOCK_CONNECTED;
		else
			wdNode->client_socket.sock_state = WD_SOCK_WAITING_FOR_CONNECT;
	}
	return (wdNode->client_socket.sock_state != WD_SOCK_ERROR);
}

/* SIGHUP handler */
static RETSIGTYPE reload_config_handler(int sig)
{
	reload_config_signal = 1;
}

static void check_config_reload(void)
{
	/* reload config file */
	if (reload_config_signal)
	{
		MemoryContext oldContext = MemoryContextSwitchTo(TopMemoryContext);
		pool_get_config(get_config_file_name(), RELOAD_CONFIG);
		MemoryContextSwitchTo(oldContext);
		reload_config_signal = 0;
	}
}


/*
 * fork a child for watchdog
 */
static pid_t fork_watchdog_child(void)
{
	pid_t pid;
	
	pid = fork();
	
	if (pid == 0)
	{
		on_exit_reset();
		
		/* Set the process type variable */
		processType = PT_WATCHDOG;
		
		/* call PCP child main */
		POOL_SETMASK(&UnBlockSig);
		watchdog_main();
	}
	else if (pid == -1)
	{
		ereport(FATAL,
				(errmsg("fork() failed. reason: %s", strerror(errno))));
	}
	
	return pid;
}

/* Never returns */
static int
watchdog_main(void)
{
	fd_set rmask;
	fd_set wmask;
	fd_set emask;
	const int select_timeout = 1;
	struct timeval tv, ref_time;
	tv.tv_sec = select_timeout;
	tv.tv_usec = 0;
	
	
	volatile int fd;
	sigjmp_buf	local_sigjmp_buf;
	
	pool_signal(SIGTERM, wd_child_exit);
	pool_signal(SIGINT, wd_child_exit);
	pool_signal(SIGQUIT, wd_child_exit);
	pool_signal(SIGHUP, reload_config_handler);
	pool_signal(SIGCHLD, SIG_DFL);
	pool_signal(SIGUSR1, SIG_IGN);
	pool_signal(SIGUSR2, SIG_IGN);
	pool_signal(SIGPIPE, SIG_IGN);
	pool_signal(SIGALRM, SIG_IGN);
	
	init_ps_display("", "", "", "");
	
	/* Create per loop iteration memory context */
	ProcessLoopContext = AllocSetContextCreate(TopMemoryContext,
											   "wd_child_main_loop",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
	
	MemoryContextSwitchTo(TopMemoryContext);
	
	set_ps_display("watchdog", false);
	
	/* initialize all the local structures for watchdog */
	wd_cluster_initialize();
	/* create a server socket for incoming watchdog connections */
	g_cluster.localNode->server_socket.sock = wd_create_recv_socket(g_cluster.localNode->wd_port);
	/* open the command server */
	g_cluster.command_server_sock = wd_create_command_server_socket();
	
	/* try connecting to all watchdog nodes */
	g_cluster.network_monitor_sock = create_monitoring_socket();
	connect_with_all_configured_nodes();

	/* set the initial state of local node */
	set_local_node_state(WD_LOADING);

	/*
	 * Okay inform the parent by SIGUSR1 about initialization complete
	 */
	kill(getppid(), SIGUSR2);
	
	/*
	 * install the call back for preparation of system exit
	 */
	on_system_exit(wd_system_will_go_down, (Datum)NULL);
	
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		if(fd > 0)
			close(fd);
		
		error_context_stack = NULL;
		
		EmitErrorReport();
		MemoryContextSwitchTo(TopMemoryContext);
		FlushErrorState();
	}
	
	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;
	
	/* watchdog child loop */
	for(;;)
	{
		int fd_max, select_ret;
		bool timeout_event = false;
		
		MemoryContextSwitchTo(ProcessLoopContext);
		//MemoryContextResetAndDeleteChildren(ProcessLoopContext); TODO
		
		check_config_reload();
		
		fd_max = prepare_fds(&rmask,&wmask,&emask);
		
		select_ret = select(fd_max + 1, &rmask, &wmask, &emask, &tv);
		
		gettimeofday(&ref_time,NULL);
		
		if (g_timeout_sec > 0 )
		{
			if (WD_TIME_DIFF_SEC(ref_time,g_tm_set_time) >=  g_timeout_sec)
			{
				timeout_event = true;
				g_timeout_sec = 0;
			}
		}
		if (select_ret > 0)
		{
			int processed_fds = 0;
			processed_fds += accept_incomming_connections(&rmask, (select_ret - processed_fds));
			processed_fds += update_successful_outgoing_cons(&wmask,(select_ret - processed_fds));
			processed_fds += read_sockets(&rmask,(select_ret - processed_fds));
		}
		
		if (timeout_event)
			watchdog_state_machine(WD_EVENT_TIMEOUT, NULL, NULL);
		if (WD_TIME_DIFF_SEC(ref_time,g_tm_set_time) >=  1)
			process_wd_func_commands_for_timer_events();

		check_for_current_command_timeout();
		
	}
	return 0;
}

static int
wd_create_command_server_socket(void)
{
	size_t	len = 0;
	struct sockaddr_un addr;
	int sock = -1;
	
	/* We use unix domain stream sockets for the purpose */
	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
	{
		/* socket create failed */
		ereport(ERROR,
				(errmsg("failed to create watchdog command server socket"),
				 errdetail("create socket failed with reason: \"%s\"", strerror(errno))));
	}
	memset((char *) &addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path),"%s",get_watchdog_ipc_address());
	len = sizeof(struct sockaddr_un);
	ereport(LOG,
			(errmsg("IPC socket path: \"%s\"",get_watchdog_ipc_address())));
	if ( bind(sock, (struct sockaddr *) &addr, len) == -1)
	{
		close(sock);
		unlink(addr.sun_path);
		ereport(ERROR,
				(errmsg("failed to create watchdog command server socket"),
				 errdetail("bind on \"%s\" failed with reason: \"%s\"", addr.sun_path, strerror(errno))));
	}
	
	if ( listen(sock, 5) < 0 )
	{
		/* listen failed */
		close(sock);
		unlink(addr.sun_path);
		ereport(ERROR,
				(errmsg("failed to create watchdog command server socket"),
				 errdetail("listen failed with reason: \"%s\"", strerror(errno))));
	}
	on_proc_exit(FileUnlink, (Datum) pstrdup(addr.sun_path));
	return sock;
}

static void FileUnlink(int code, Datum path)
{
	char* filePath = (char*)path;
	unlink(filePath);
}


/*
 * sets all the valid watchdog cluster descriptors to the fd_set.
 returns the fd_max */
static int
prepare_fds(fd_set* rmask, fd_set* wmask, fd_set* emask)
{
	int i;
	ListCell *lc;
	int fd_max = g_cluster.localNode->server_socket.sock;
	
	FD_ZERO(rmask);
	FD_ZERO(wmask);
	FD_ZERO(emask);
	
	/* local node server socket will set the read and exception fds */
	FD_SET(g_cluster.localNode->server_socket.sock,rmask);
	FD_SET(g_cluster.localNode->server_socket.sock,emask);
	
	/* command server socket will set the read and exception fds */
	FD_SET(g_cluster.command_server_sock,rmask);
	FD_SET(g_cluster.command_server_sock,emask);
	if (fd_max < g_cluster.command_server_sock)
		fd_max = g_cluster.command_server_sock;
	
	FD_SET(g_cluster.network_monitor_sock,rmask);
	if (fd_max < g_cluster.network_monitor_sock)
		fd_max = g_cluster.network_monitor_sock;
	
	/*
	 * set write fdset for all waiting for connection sockets,
	 * while already connected will be only be waiting for read
	 */
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
		if (wdNode->client_socket.sock > 0)
		{
			if (fd_max < wdNode->client_socket.sock)
				fd_max = wdNode->client_socket.sock;
			
			FD_SET(wdNode->client_socket.sock,emask);
			
			if (wdNode->client_socket.sock_state == WD_SOCK_WAITING_FOR_CONNECT)
				FD_SET(wdNode->client_socket.sock,wmask);
			else
				FD_SET(wdNode->client_socket.sock,rmask);
		}
		if (wdNode->server_socket.sock > 0)
		{
			if (fd_max < wdNode->server_socket.sock)
				fd_max = wdNode->server_socket.sock;
			
			FD_SET(wdNode->server_socket.sock,emask);
			FD_SET(wdNode->server_socket.sock,rmask);
		}
	}
	/*
	 * I know this is getting complex but we need to add all incomming unassigned connection sockets
	 * these one will go for reading
	 */
	foreach(lc, g_cluster.unidentified_socks)
	{
		SocketConnection *conn = lfirst(lc);
		int ui_sock = conn->sock;
		if (ui_sock > 0)
		{
			FD_SET(ui_sock,rmask);
			FD_SET(ui_sock,emask);
			if (fd_max < ui_sock)
				fd_max = ui_sock;
		}
	}
	
	/* Add the notification connected clients */
	foreach(lc, g_cluster.notify_clients)
	{
		int ui_sock = lfirst_int(lc);
		if (ui_sock > 0)
		{
			FD_SET(ui_sock,rmask);
			FD_SET(ui_sock,emask);
			if (fd_max < ui_sock)
				fd_max = ui_sock;
		}
	}
	
	/* Finally Add the command IPC sockets */
	foreach(lc, g_cluster.ipc_command_socks)
	{
		int ui_sock = lfirst_int(lc);
		if (ui_sock > 0)
		{
			FD_SET(ui_sock,rmask);
			FD_SET(ui_sock,emask);
			if (fd_max < ui_sock)
				fd_max = ui_sock;
		}
	}
	
	return fd_max;
}

static int read_sockets(fd_set* rmask,int pending_fds_count)
{
	int i,count = 0;
	List* socks_to_del = NIL;
	ListCell *lc;
	
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);

		if (is_socket_connection_connected(&wdNode->client_socket))
		{
			if ( FD_ISSET(wdNode->client_socket.sock, rmask) )
			{
				ereport(LOG,
						(errmsg("client socket of %s is ready for reading", wdNode->nodeName)));
				
				WDPacketData* pkt = read_packet(&wdNode->client_socket);
				if (pkt)
				{
					watchdog_state_machine(WD_EVENT_PACKET_RCV, wdNode, pkt);
					free_packet(pkt);
				}
				count++;
				if (count >= pending_fds_count)
					return count;
			}
		}
		if (is_socket_connection_connected(&wdNode->server_socket))
		{
			if ( FD_ISSET(wdNode->server_socket.sock, rmask) )
			{
				ereport(LOG,
						(errmsg("server socket of %s is ready for reading", wdNode->nodeName)));
				WDPacketData* pkt = read_packet(&wdNode->server_socket);
				if (pkt)
				{
					watchdog_state_machine(WD_EVENT_PACKET_RCV, wdNode, pkt);
					free_packet(pkt);
				}
				
				count++;
				if (count >= pending_fds_count)
					return count;
			}
		}
	}
	
	foreach(lc, g_cluster.unidentified_socks)
	{
		SocketConnection *conn = lfirst(lc);
		if (conn->sock > 0 &&  FD_ISSET(conn->sock, rmask))
		{
			WDPacketData* pkt;
			ereport(LOG,
					(errmsg("un-identified socket %d is ready for reading",conn->sock)));
			/* we only entertain ADD NODE messages from unidentified sockets */
			pkt = read_packet_of_type(conn,WD_ADD_NODE_MESSAGE);
			if (pkt)
			{
				char *authkey = NULL;
				WatchdogNode* tempNode = parse_node_info_message(pkt, &authkey);
				if (tempNode)
				{
					WatchdogNode* wdNode;
					bool found = false;
					bool authenticated = false;

					print_watchdog_node_info(tempNode);
					authenticated = verify_authhash_for_node(tempNode, authkey);
					ereport(DEBUG1,
							(errmsg("NODE ADD MESSAGE from Hostname:\"%s\" PORT:%d pgpool_port:%d",tempNode->hostname,tempNode->wd_port,tempNode->pgpool_port)));
					/* verify this node */
					if (authenticated)
					{
						for (i=0; i< g_cluster.remoteNodeCount; i++)
						{
							wdNode = &(g_cluster.remoteNodes[i]);
							ereport(DEBUG1,
									(errmsg("Comparing with NODE having Hostname:\"%s\" PORT:%d pgpool_port:%d",wdNode->hostname,wdNode->wd_port,wdNode->pgpool_port)));
							
							if ( (wdNode->wd_port == tempNode->wd_port && wdNode->pgpool_port == tempNode->pgpool_port) &&
								( (strcmp(wdNode->hostname,conn->addr) == 0) || (strcmp(wdNode->hostname,tempNode->hostname) == 0)) )
							{
								/* We have found the match */
								found = true;
								close_socket_connection(&wdNode->server_socket);
								strlcpy(wdNode->delegate_ip, tempNode->delegate_ip, WD_MAX_HOST_NAMELEN);
								strlcpy(wdNode->nodeName, tempNode->nodeName, WD_MAX_HOST_NAMELEN);
								wdNode->state = tempNode->state;
								wdNode->tv.tv_sec = tempNode->tv.tv_sec;
								wdNode->wd_priority = tempNode->wd_priority;
								wdNode->server_socket = *conn;
								wdNode->server_socket.sock_state = WD_SOCK_CONNECTED;
	//							wdNode->server_socket.sock = ui_sock;
								break;
							}
						}
						if (found)
						{
							/* reply with node info message */
							ereport(NOTICE,
									(errmsg("New node joined the cluster Hostname:\"%s\" PORT:%d pgpool_port:%d",tempNode->hostname,tempNode->wd_port,tempNode->pgpool_port)));

							watchdog_state_machine(WD_EVENT_PACKET_RCV, wdNode, pkt);
						}
						else
							ereport(NOTICE,
								(errmsg("add node from Hostname:\"%s\" PORT:%d pgpool_port:%d rejected.",tempNode->hostname,tempNode->wd_port,tempNode->pgpool_port),
									 errdetail("verify the other watchdog node configurations")));

					}
					else
					{
						ereport(NOTICE,
								(errmsg("Authentication failed for add node from Hostname:\"%s\" PORT:%d pgpool_port:%d",tempNode->hostname,tempNode->wd_port,tempNode->pgpool_port),
								 errdetail("make sure wd_authkey configuration is same on all nodes")));
					}

					if (found == false || authenticated == false)
					{
						/* reply with reject message, We do not need to go to state processor */
						/* For now, create a empty temp node. TODO*/
						WatchdogNode tmpNode;
						tmpNode.client_socket = *conn;
						tmpNode.client_socket.sock_state = WD_SOCK_CONNECTED;
						tmpNode.server_socket.sock = -1;
						tmpNode.server_socket.sock_state = WD_SOCK_UNINITIALIZED;
						reply_with_minimal_message(&tmpNode, WD_REJECT_MESSAGE, pkt);
						close_socket_connection(conn);
					}
					pfree(tempNode);
				}
				if (authkey)
					pfree(authkey);
				//				watchdog_state_machine(WD_EVENT_PACKET_RCV, p, packet);
				free_packet(pkt);
				count++;
			}
			socks_to_del = lappend(socks_to_del,conn);
			count++;
			if (count >= pending_fds_count)
				break;
		}
	}
	
	/* delete all the sockets from unidentified list which are now identified */
	foreach(lc, socks_to_del)
	{
		g_cluster.unidentified_socks = list_delete_ptr(g_cluster.unidentified_socks,lfirst(lc));
	}

	list_free_deep(socks_to_del);
	socks_to_del = NULL;

	if (count >= pending_fds_count)
		return count;

	foreach(lc, g_cluster.ipc_command_socks)
	{
		int command_sock = lfirst_int(lc);
		if (command_sock > 0 &&  FD_ISSET(command_sock, rmask))
		{
			bool remove_sock = false;
			read_ipc_command_and_process(command_sock, &remove_sock);
			if (remove_sock)
			{
				/* Also locate the command if it has this socket */
				WDIPCCommandData* ipcCommand = get_wd_IPC_command_from_socket(command_sock);
				if (ipcCommand)
				{
					/* special case we want to remove the socket from
					 * ipc_command_sock list manually, so mark the issuing socket
					 * of ipcComman to invalid value
					 */
					ipcCommand->issueing_sock = -1;
				}
				close(command_sock);
				socks_to_del = lappend_int(socks_to_del,command_sock);
			}
			count++;
			if (count >= pending_fds_count)
				break;
		}
	}
	/* delete all the sockets from unidentified list which are now identified */
	foreach(lc, socks_to_del)
	{
		g_cluster.ipc_command_socks = list_delete_int(g_cluster.ipc_command_socks,lfirst_int(lc));
	}
	
	list_free(socks_to_del);
	socks_to_del = NULL;
	
	if (count >= pending_fds_count)
		return count;
	
	foreach(lc, g_cluster.notify_clients)
	{
		int notify_sock = lfirst_int(lc);
		if (notify_sock > 0 &&  FD_ISSET(notify_sock, rmask))
		{
			bool remove_sock = false;
			read_ipc_command_and_process(notify_sock, &remove_sock);
			if (remove_sock)
			{
				close(notify_sock);
				socks_to_del = lappend_int(socks_to_del,notify_sock);
			}
			count++;
			if (count >= pending_fds_count)
				break;
		}
	}
	/* delete all the sockets from unidentified list which are now identified */
	foreach(lc, socks_to_del)
	{
		g_cluster.notify_clients = list_delete_int(g_cluster.notify_clients,lfirst_int(lc));
	}
	
	list_free(socks_to_del);
	socks_to_del = NULL;
	
	return count;
}

static bool read_ipc_command_and_process(int sock, bool *remove_socket)
{
	char type;
	WD_COMMAND_ACTIONS command_action;
	IPC_CMD_PREOCESS_RES res;
	int data_len,ret;
	WDIPCCommandData* IPCCommand = NULL;
	
	*remove_socket = true;
	
	/* 1st byte is command type */
	ret = read(sock, &type, sizeof(char));
	if (ret == 0) /* remote end has closed the connection */
		return false;
	
	if (ret != sizeof(char))
	{
		ereport(WARNING,
				(errmsg("error reading from IPC socket"),
				 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
		return false;
	}
	/* Next is is command action */
	ret = read(sock, &command_action, sizeof(WD_COMMAND_ACTIONS));
	if (ret != sizeof(WD_COMMAND_ACTIONS))
	{
		ereport(WARNING,
				(errmsg("error reading from IPC socket"),
				 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
		return false;
	}
	/* We should have data length */
	ret = read(sock, &data_len, sizeof(int));
	if (ret != sizeof(int))
	{
		ereport(WARNING,
				(errmsg("error reading from IPC socket"),
				 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
		return false;
	}
	
	data_len = ntohl(data_len);
	/* see if we have enough information to process this command */
	MemoryContext mCxt, oldCxt;
	mCxt = AllocSetContextCreate(TopMemoryContext,
								 "WDIPCCommand",
								 ALLOCSET_SMALL_MINSIZE,
								 ALLOCSET_SMALL_INITSIZE,
								 ALLOCSET_SMALL_MAXSIZE);
	oldCxt = MemoryContextSwitchTo(mCxt);
	
	IPCCommand = palloc0(sizeof(WDIPCCommandData));
	
	IPCCommand->issueing_sock = sock;
	IPCCommand->command_action = command_action;
	IPCCommand->type = type;
	gettimeofday(&IPCCommand->issue_time, NULL);
	
	if (data_len > 0)
		IPCCommand->data_buf = palloc(data_len);
	else
		IPCCommand->data_buf = NULL;
	
	IPCCommand->nodeResults = NULL;
	
	IPCCommand->memoryContext = mCxt;
	
	MemoryContextSwitchTo(oldCxt);
	
	while (IPCCommand->data_len < data_len)
	{
		int ret = read(sock, IPCCommand->data_buf + IPCCommand->data_len, (data_len - IPCCommand->data_len));
		if (ret <= 0)
		{
			ereport(NOTICE,
					(errmsg("error reading IPC from socket"),
					 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
			MemoryContextDelete(mCxt);
			return false;
		}
		IPCCommand->data_len +=ret;
	}
	
	res = process_IPC_command(IPCCommand);
	if (res == IPC_CMD_PROCESSING)
	{
		/*
		 * The command still needs further processing
		 * store it in the list
		 */
		*remove_socket = false;
		g_cluster.ipc_commands = lappend(g_cluster.ipc_commands,IPCCommand);
		return true;
	}
	if (res == IPC_CMD_ERROR)
		ereport(NOTICE,
				(errmsg("error processing IPC from socket")));
	
	/* Delete the ipcCommand structure,
	 * it is as simple as to delete the memory context
	 */
	MemoryContextDelete(mCxt);
	return (res != IPC_CMD_ERROR);
}

static IPC_CMD_PREOCESS_RES process_IPC_command(WDIPCCommandData* IPCCommand)
{
	switch(IPCCommand->type)
	{
		case WD_NODE_STATUS_CHANGE_COMMAND:
			return process_IPC_nodeStatusChange_command(IPCCommand);
			break;
			
		case WD_TRY_COMMAND_LOCK:
			return process_IPC_lock_request(IPCCommand);
			break;
			
		case WD_COMMAND_UNLOCK:
			process_IPC_unlock_request(IPCCommand);
			break;
			
		case WD_REGISTER_FOR_NOTIFICATION:
			/* Add this socket to the notify socket list*/
			g_cluster.notify_clients = lappend_int(g_cluster.notify_clients, IPCCommand->issueing_sock);
			/* The command is completed successfully */
			return IPC_CMD_COMPLETE;
			break;
			
		case WD_GET_NODES_LIST_COMMAND:
			return process_IPC_nodeList_command(IPCCommand);
			break;
			
		case WD_FUNCTION_COMMAND:
			return process_IPC_replicate_variable(IPCCommand);
			break;
			
		case WD_FAILOVER_CMD_SYNC_REQUEST:
			return process_IPC_failover_cmd_synchronise(IPCCommand);
			
		default:
			ereport(LOG,
					(errmsg("invalid IPC command type %c",IPCCommand->type)));
			break;
	}
	return IPC_CMD_ERROR;
}


static IPC_CMD_PREOCESS_RES process_IPC_nodeList_command(WDIPCCommandData* IPCCommand)
{
	/* get the json for node list */
	JsonNode* jNode = NULL;
	int NodeID = -1;
	int len, nwlen;
	json_value *root = json_parse(IPCCommand->data_buf,IPCCommand->data_len);
	/* The root node must be object */
	if (root == NULL || root->type != json_object)
	{
		json_value_free(root);
		ereport(NOTICE,
				(errmsg("unable to parse json data from get node list command")));
		return IPC_CMD_ERROR;
	}
	/* If it is a node function ?*/
	if (json_get_int_value_for_key(root, "NodeID", &NodeID))
	{
		json_value_free(root);
		return IPC_CMD_ERROR;
	}
	json_value_free(root);
	jNode = get_node_list_json(NodeID);
	len = jw_get_json_length(jNode);
	nwlen = htonl(len);
	char type = WD_IPC_CMD_RESULT_OK;
	int ret = write(IPCCommand->issueing_sock, &type, 1);
	if (ret < 1)
		return IPC_CMD_ERROR;
	
	/* write data length */
	ret = write(IPCCommand->issueing_sock, &nwlen, 4);
	if (ret < 4)
		return IPC_CMD_ERROR;
	
	/* write json data */
	ret = write(IPCCommand->issueing_sock, jw_get_json_string(jNode), len);
	jw_destroy(jNode);
	if (ret < len)
		return IPC_CMD_ERROR;
	/* good */
	return IPC_CMD_COMPLETE;
}

static IPC_CMD_PREOCESS_RES process_IPC_nodeStatusChange_command(WDIPCCommandData* IPCCommand)
{
	int nodeStatus;
	int nodeID;
	char *message;
	bool ret;
 
	ret = parse_node_status_json(IPCCommand->data_buf, IPCCommand->data_len, &nodeID, &nodeStatus, &message);
	
	if (ret == false)
	{
		ereport(WARNING,
				(errmsg("unable to parse json data from node status change ipc message")));
		return IPC_CMD_ERROR;
	}
	
	if (message)
		ereport(LOG,
				(errmsg("received node status change ipc message"),
				 errdetail("%s",message)));
	pfree(message);
	
	if (fire_node_status_event(nodeID,nodeStatus) == false)
		return IPC_CMD_ERROR;
	
	return IPC_CMD_COMPLETE;
}

static bool fire_node_status_event(int nodeID, int nodeStatus)
{
	WatchdogNode* wdNode = NULL;
	if (nodeID == 0) /* this is reserved for local node */
	{
		wdNode = g_cluster.localNode;
	}
	else
	{
		int i;
		for (i = 0; i < g_cluster.remoteNodeCount; i++)
		{
			if (nodeID == g_cluster.remoteNodes[i].private_id)
			{
				wdNode = &g_cluster.remoteNodes[i];
				break;
			}
		}
	}
	if (wdNode == NULL)
	{
		ereport(LOG,
				(errmsg("invalid Node id for node event")));
		return false;
	}
	
	if (nodeStatus == WD_LIFECHECK_NODE_STATUS_DEAD)
	{
		ereport(DEBUG1,
				(errmsg("Firing NODE STATUS EVENT: NODE(ID=%d) IS DEAD",nodeID)));

		if (wdNode == g_cluster.localNode)
			watchdog_state_machine(WD_EVENT_LOCAL_NODE_LOST, wdNode, NULL);
		else
			watchdog_state_machine(WD_EVENT_REMOTE_NODE_LOST, wdNode, NULL);
	}
	else if (nodeStatus == WD_LIFECHECK_NODE_STATUS_ALIVE)
	{
		ereport(DEBUG1,
				(errmsg("Firing NODE STATUS EVENT: NODE(ID=%d) IS ALIVE",nodeID)));

		if (wdNode == g_cluster.localNode)
			watchdog_state_machine(WD_EVENT_REMOTE_NODE_FOUND, wdNode, NULL);
		else
			watchdog_state_machine(WD_EVENT_REMOTE_NODE_FOUND, wdNode, NULL);
	}
	else
		ereport(LOG,
				(errmsg("invalid Node action")));
	return true;
}



static IPC_CMD_PREOCESS_RES process_IPC_replicate_variable(WDIPCCommandData* IPCCommand)
{
	char res_type;
	int res_len = 0;
	int ret;
	
	if (get_local_node_state() == WD_STANDBY ||
		get_local_node_state() == WD_COORDINATOR)
	{
		res_type = execute_replicate_command(IPCCommand);
	}
	else /* we are not in any stable state at the moment */
	{
		res_type = WD_IPC_CMD_CLUSTER_IN_TRAN;
	}
	
	if (res_type == IPC_CMD_PROCESSING)
	{
		/* Do not reply back to requester, as we are
		 * still processing the results
		 */
		return res_type;
	}
	
	ret = write(IPCCommand->issueing_sock, &res_type, 1);
	if (ret < 1)
		return IPC_CMD_ERROR;
	/* write data length */
	ret = write(IPCCommand->issueing_sock, &res_len, 4);
	if (ret < 4)
		return IPC_CMD_ERROR;
	/*
	 * This is the complete lifecycle of command.
	 * we are done with it
	 */
	
	return IPC_CMD_COMPLETE;
	
}

static IPC_CMD_PREOCESS_RES process_IPC_unlock_request(WDIPCCommandData *IPCCommand)
{
	char res_type;
	int res_len = 0;
	int ret;
	/*
	 * if cluster or myself is not in stable state
	 * just return cluster in transaction
	 */
	IPCCommand->type = WD_INTERUNLOCKING_REQUEST;
	if (g_cluster.lockHolderNode == NULL)
	{
		/* There is no lock holder as per our records
		 * just ignore this request
		 */
		res_type = WD_IPC_CMD_RESULT_OK;
	}
	else if(g_cluster.lockHolderNode != g_cluster.localNode)
	{
		/* I am not the lockhoder node, so How can I unlonk
		 * just return the error
		 */
		res_type = WD_IPC_CMD_RESULT_BAD;
	}
	else if (get_local_node_state() == WD_STANDBY)
	{
		/* I am a standby node, and also the lock holder
		 * Just forward the request to coordinator */
		
		printf("\t\t\t %s:%d I AM STANDBY \n",__FUNCTION__,__LINE__);
		
		WDPacketData *wdPacket = get_minimum_message(WD_INTERUNLOCKING_REQUEST,NULL);
		/* save the command ID */
		IPCCommand->internal_command_id = wdPacket->command_id;
		
		if (send_message(g_cluster.masterNode, wdPacket) <= 0)
		{
			printf("\t\t\t %s:%d send unlock request message failed \n",__FUNCTION__,__LINE__);
			
			/* we have failed to send to any node, return lock failed  */
			res_type = WD_IPC_CMD_RESULT_BAD;
		}
		else
		{
			/*
			 * we need to wait for the results
			 */
			printf("\t\t\t %s:%d PROCESSING \n",__FUNCTION__,__LINE__);
			
			res_type = IPC_CMD_PROCESSING;
		}
		/* whatever the case we need to resign ourself from lock
		 * holder
		 */
		pfree(wdPacket);
		g_cluster.lockHolderNode = NULL;
	}
	else if (get_local_node_state() == WD_COORDINATOR)
	{
		printf("\t\t\t %s:%d COORDINATOR \n",__FUNCTION__,__LINE__);
		
		/*
		 * If I am coordinator, Just process the request locally
		 */
		if (node_has_resigned_from_interlocking(g_cluster.localNode, NULL))
		{
			printf("\t\t\t %s:%d \n",__FUNCTION__,__LINE__);
			
			res_type = WD_IPC_CMD_RESULT_OK;
		}
		else
			res_type = WD_IPC_CMD_RESULT_BAD;
	}
	else /* we are not in any stable state at the moment */
	{
		g_cluster.lockHolderNode = NULL;
		res_type = WD_IPC_CMD_CLUSTER_IN_TRAN;
	}
	
	if (res_type == IPC_CMD_PROCESSING)
	{
		/* Do not reply back to requester, as we are
		 * still processing the results
		 */
		return res_type;
	}
	
	ret = write(IPCCommand->issueing_sock, &res_type, 1);
	if (ret < 1)
		return IPC_CMD_ERROR;
	/* write data length */
	ret = write(IPCCommand->issueing_sock, &res_len, 4);
	if (ret < 4)
		return IPC_CMD_ERROR;
	/*
	 * This is the complete lifecycle of command.
	 * we are done with it
	 */
	printf("\t\t\t %s:%d \n",__FUNCTION__,__LINE__);
	
	return IPC_CMD_COMPLETE;
}


static IPC_CMD_PREOCESS_RES process_IPC_lock_request(WDIPCCommandData *IPCCommand)
{
	char res_type;
	int res_len = 0;
	int ret;
	/*
	 * if cluster or myself is not in stable state
	 * just return cluster in transaction
	 */
	IPCCommand->type = WD_INTERLOCKING_REQUEST;
	if (get_local_node_state() == WD_STANDBY)
	{
		printf("\t\t\t %s:%d I AM STANDBY \n",__FUNCTION__,__LINE__);
		
		/* I am a standby node, Just forward the request to coordinator */
		WDPacketData * wdPacket = get_minimum_message(WD_INTERLOCKING_REQUEST,NULL);
		/* save the command ID */
		IPCCommand->internal_command_id = wdPacket->command_id;
		
		if (send_message(g_cluster.masterNode, wdPacket) <= 0)
		{
			printf("\t\t\t %s:%d send message failed \n",__FUNCTION__,__LINE__);
			
			/* we have failed to send to any node, return lock failed  */
			res_type = WD_IPC_CMD_RESULT_BAD;
		}
		else
		{
			/*
			 * we need to wait for the result
			 */
			printf("\t\t\t %s:%d PROCESSING \n",__FUNCTION__,__LINE__);
			
			res_type = IPC_CMD_PROCESSING;
		}
	}
	else if (get_local_node_state() == WD_COORDINATOR)
	{
		printf("\t\t\t %s:%d COORDINATOR \n",__FUNCTION__,__LINE__);
		
		/*
		 * If I am coordinator, Just process the request locally
		 */
		if (node_has_requested_for_interlocking(g_cluster.localNode, NULL))
		{
			printf("\t\t\t %s:%d \n",__FUNCTION__,__LINE__);
			
			res_type = WD_IPC_CMD_RESULT_OK;
		}
		else
			res_type = WD_IPC_CMD_RESULT_BAD;
	}
	else /* we are not in any stable state at the moment */
		res_type = WD_IPC_CMD_CLUSTER_IN_TRAN;
	
	if (res_type == IPC_CMD_PROCESSING)
	{
		/* Do not reply back to requester, as we are
		 * still processing the results
		 */
		return res_type;
	}
	
	ret = write(IPCCommand->issueing_sock, &res_type, 1);
	if (ret < 1)
		return IPC_CMD_ERROR;
	/* write data length */
	ret = write(IPCCommand->issueing_sock, &res_len, 4);
	if (ret < 4)
		return IPC_CMD_ERROR;
	/*
	 * This is the complete lifecycle of command.
	 * we are done with it
	 */
	printf("\t\t\t %s:%d \n",__FUNCTION__,__LINE__);
	
	return IPC_CMD_COMPLETE;
}

static IPC_CMD_PREOCESS_RES process_IPC_failover_cmd_synchronise(WDIPCCommandData *IPCCommand)
{
	char res_type;
	int res_len = 0;
	int ret;
	/*
	 * if cluster or myself is not in stable state
	 * just return cluster in transaction
	 */
	IPCCommand->type = WD_FAILOVER_CMD_SYNC_REQUEST;
	if (get_local_node_state() == WD_STANDBY)
	{
		/* I am a standby node, Just forward the request to coordinator */
		
		WDPacketData wdPacket;
		init_wd_packet(&wdPacket);
		set_message_type(&wdPacket, WD_FAILOVER_CMD_SYNC_REQUEST);
		set_next_commandID_in_message(&wdPacket);
		set_message_data(&wdPacket, IPCCommand->data_buf , IPCCommand->data_len);
		/* save the command ID */
		IPCCommand->internal_command_id = wdPacket.command_id;
		
		printf("\t\t\t %s:%d I AM STANDBY \n",__FUNCTION__,__LINE__);
		
		if (send_message(g_cluster.masterNode, &wdPacket) <= 0)
		{
			printf("\t\t\t %s:%d send message failed \n",__FUNCTION__,__LINE__);
			/* we have failed to send to any node, return lock failed  */
			res_type = WD_IPC_CMD_RESULT_BAD;
		}
		else
		{
			/*
			 * we need to wait for the result
			 */
			printf("\t\t\t %s:%d PROCESSING \n",__FUNCTION__,__LINE__);
			res_type = IPC_CMD_PROCESSING;
		}
	}
	else if (get_local_node_state() == WD_COORDINATOR)
	{
		printf("\t\t\t %s:%d COORDINATOR \n",__FUNCTION__,__LINE__);
		
		/*
		 * If I am coordinator, Just process the request locally
		 */
		process_failover_command_sync_requests(g_cluster.localNode, NULL, IPCCommand);
		return IPC_CMD_COMPLETE;
	}
	else /* we are not in any stable state at the moment */
		res_type = WD_IPC_CMD_CLUSTER_IN_TRAN;
	
	if (res_type == IPC_CMD_PROCESSING)
	{
		/* Do not reply back to requester, as we are
		 * still processing the results
		 */
		return res_type;
	}
	
	ret = write(IPCCommand->issueing_sock, &res_type, 1);
	if (ret < 1)
		return IPC_CMD_ERROR;
	/* write data length */
	ret = write(IPCCommand->issueing_sock, &res_len, 4);
	if (ret < 4)
		return IPC_CMD_ERROR;
	/*
	 * This is the complete lifecycle of command.
	 * we are done with it
	 */
	printf("\t\t\t %s:%d \n",__FUNCTION__,__LINE__);
	
	return IPC_CMD_COMPLETE;
}

static int node_has_requested_for_interlocking(WatchdogNode* wdNode, WDPacketData* pkt)
{
	/* only coordinator(master) node can process this request */
	if (get_local_node_state() == WD_COORDINATOR)
	{
		printf("\n\t\t\t %s:%d I AM COORDINATOR \n",__FUNCTION__,__LINE__);
		
		/* check if we already have no lockholder node */
		if (g_cluster.lockHolderNode == NULL || g_cluster.lockHolderNode == wdNode)
		{
			printf("\n\t\t\t %s:%d LOCK REQUESTED IS NULL OR FROM SAME NODE \n",__FUNCTION__,__LINE__);
			
			if (wdNode == g_cluster.localNode)
			{
				g_cluster.lockHolderNode = wdNode;
				/* TODO inform all cluster about the new lock holder */
				return true;
			}
			/* reply the node with success message */
			else if (reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt))
			{
				printf("\n\t\t\t %s:%d WD_ACCEPT_MESSAGE \n",__FUNCTION__,__LINE__);
				g_cluster.lockHolderNode = wdNode;
				/* TODO inform all cluster about the new lock holder */
				return true;
			}
		}
		else
		{
			printf("\n\t\t\t %s:%d WD_REJECT_MESSAGE \n",__FUNCTION__,__LINE__);
			reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
		}
	}
	else
	{
		printf("\n\t\t\t %s:%d WD_ERROR_MESSAGE \n",__FUNCTION__,__LINE__);
		reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
	}
	return false;
}

static void process_failover_command_sync_requests(WatchdogNode* wdNode, WDPacketData* pkt, WDIPCCommandData* ipcCommand)
{
	
	WDFailoverCMDResults res = FAILOVER_RES_TRANSITION;
	JsonNode* jNode = NULL;
	int failoverCmdType = -1;
	
	/* only coordinator(master) node can process this request */
	if (get_local_node_state() == WD_COORDINATOR)
	{
		char* json_data = NULL;
		int data_len = 0;
		json_value *root;
		char* syncRequestType = NULL;
		
		/* We need to identify failover command type and sync function */
		if (pkt)
		{
			json_data = pkt->data;
			data_len = pkt->len;
		}
		else
		{
			json_data = ipcCommand->data_buf;
			data_len = ipcCommand->data_len;
		}
		
		root = json_parse(json_data,data_len);
		/* The root node must be object */
		if (root == NULL || root->type != json_object)
		{
			ereport(NOTICE,
					(errmsg("unable to parse json data from replicate command")));
			res = FAILOVER_RES_ERROR;
		}
		if (root)
			syncRequestType = json_get_string_value_for_key(root, "SyncRequestType");
		
		if (syncRequestType == NULL)
		{
			ereport(NOTICE,
					(errmsg("invalid json data"),
					 errdetail("unable to find Watchdog Function Name")));
			res = FAILOVER_RES_ERROR;
		}
		else
			syncRequestType = pstrdup(syncRequestType);
		
		if (root && json_get_int_value_for_key(root, "FailoverCMDType", &failoverCmdType))
		{
			res = FAILOVER_RES_ERROR;
		}
		
		if (root)
			json_value_free(root);
		
		/* verify the failoverCmdType */
		if (failoverCmdType < 0 || failoverCmdType >= MAX_FAILOVER_CMDS)
			res = FAILOVER_RES_ERROR;
		
		if (syncRequestType == NULL)
			res = FAILOVER_RES_ERROR;
		
		if (res != FAILOVER_RES_ERROR)
		{
			if (strcasecmp("START_COMMAND", syncRequestType) == 0)
				res = node_is_asking_for_failover_cmd_start(wdNode, pkt, failoverCmdType, false);
			else if (strcasecmp("END_COMMAND", syncRequestType) == 0)
				res = node_is_asking_for_failover_cmd_end(wdNode, pkt, failoverCmdType, true);
			else if (strcasecmp("UNLOCK_COMMAND", syncRequestType) == 0)
				res = node_is_asking_for_failover_cmd_end(wdNode, pkt, failoverCmdType, false);
			else if (strcasecmp("CHECK_LOCKED", syncRequestType) == 0)
				res = node_is_asking_for_failover_cmd_start(wdNode, pkt, failoverCmdType, true);
			else
				res = FAILOVER_RES_ERROR;
		}
	}
	else
	{
		res = FAILOVER_RES_ERROR;
	}
	
	if (res != FAILOVER_RES_ERROR)
	{
		/* create the json result */
		jNode = jw_create_with_object(true);
		/* add the node count */
		jw_put_int(jNode, "FailoverCMDType", failoverCmdType);
		jw_put_int(jNode, "InterlockingResult", res);
		/* create the packet */
		jw_end_element(jNode);
		jw_finish_document(jNode);
	}
	
	if (wdNode != g_cluster.localNode)
	{
		if (jNode == NULL)
		{
			reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
		}
		else
		{
			reply_with_message(wdNode, WD_DATA_MESSAGE, jw_get_json_string(jNode), jw_get_json_length(jNode), pkt);
		}
	}
	else
	{
		/* Reply to IPC Socket */
		int res_len = 0;
		char res_type = WD_IPC_CMD_RESULT_BAD;
		if (jNode != NULL)
		{
			res_len = htonl(jw_get_json_length(jNode));
			res_type = WD_IPC_CMD_RESULT_OK;
		}
		
		write(ipcCommand->issueing_sock, &res_type, 1);
		write(ipcCommand->issueing_sock, &res_len, 4);
		if (res_len > 0)
			write(ipcCommand->issueing_sock, jw_get_json_string(jNode), jw_get_json_length(jNode));
	}
	if (jNode)
		jw_destroy(jNode);
	
}

static WDFailoverCMDResults
node_is_asking_for_failover_cmd_start(WatchdogNode* wdNode, WDPacketData* pkt, int failoverCmdType, bool check)
{
	WDFailoverCMDResults res = FAILOVER_RES_TRANSITION;
	/* only coordinator(master) node can process this request */
	if (get_local_node_state() == WD_COORDINATOR)
	{
		InterlockingNode* lockingNode = NULL;
		if (failoverCmdType < 0 || failoverCmdType >= MAX_FAILOVER_CMDS)
			res = FAILOVER_RES_ERROR;
		else
			lockingNode = &g_cluster.interlockingNodes[failoverCmdType];
		
		if (res != FAILOVER_RES_ERROR)
		{
			/* check if we already have no lockholder node */
			if (lockingNode->lockHolderNode == NULL || lockingNode->lockHolderNode == wdNode)
			{
				printf("\n\t\t\t %s:%d LOCK REQUESTED IS NULL OR FROM SAME NODE \n",__FUNCTION__,__LINE__);
				if (check == false)
				{
					lockingNode->lockHolderNode = wdNode;
					lockingNode->locked = true;
				}
				res = FAILOVER_RES_PROCEED_LOCK_HOLDER;
			}
			else /* some other node is holding the lock */
			{
				printf("\n\t\t\t %s:%d Some other node is already holding the lock \n",__FUNCTION__,__LINE__);
				if (lockingNode->locked)
					res = FAILOVER_RES_BLOCKED;
				else
					res = FAILOVER_RES_PROCEED_UNLOCKED;
			}
		}
	}
	else
	{
		printf("\n\t\t\t %s:%d I am not in position \n",__FUNCTION__,__LINE__);
		res = FAILOVER_RES_ERROR;
	}
	return res;
}

static WDFailoverCMDResults
node_is_asking_for_failover_cmd_end(WatchdogNode* wdNode, WDPacketData* pkt, int failoverCmdType, bool resign)
{
	WDFailoverCMDResults res = FAILOVER_RES_TRANSITION;
	/* only coordinator(master) node can process this request */
	if (get_local_node_state() == WD_COORDINATOR)
	{
		InterlockingNode* lockingNode = NULL;
		
		if (failoverCmdType < 0 || failoverCmdType >= MAX_FAILOVER_CMDS)
			res = FAILOVER_RES_ERROR;
		else
			lockingNode = &g_cluster.interlockingNodes[failoverCmdType];
		
		if (res != FAILOVER_RES_ERROR)
		{
			/* check if we already have no lockholder node */
			if (lockingNode->lockHolderNode == NULL || lockingNode->lockHolderNode == wdNode)
			{
				if (resign)
					lockingNode->lockHolderNode = NULL;
				lockingNode->locked = false;
				res = FAILOVER_RES_PROCEED_UNLOCKED;
			}
			else /* some other node is holding the lock */
			{
				printf("\n\t\t\t %s:%d only lock holder can resign from it \n",__FUNCTION__,__LINE__);
				res = FAILOVER_RES_BLOCKED;
			}
		}
	}
	else
	{
		printf("\n\t\t\t %s:%d I am not in position \n",__FUNCTION__,__LINE__);
		res = FAILOVER_RES_ERROR;
	}
	return res;
}

static bool node_has_resigned_from_interlocking(WatchdogNode* wdNode, WDPacketData* pkt)
{
	/* only coordinator(master) node can process this request */
	if (get_local_node_state() == WD_COORDINATOR)
	{
		/* check if we already have no lockholder node */
		if (g_cluster.lockHolderNode == NULL || g_cluster.lockHolderNode == wdNode)
		{
			/* reply the node with success message */
			if (reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt))
			{
				g_cluster.lockHolderNode = NULL;
				/* TODO inform all cluster about the new lock holder */
				return true;
			}
		}
		else
		{
			/* only lock holder can resign itself */
			reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
		}
	}
	else
		reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
	return false;
}


static WatchdogNode* parse_node_info_message(WDPacketData* pkt, char **authkey)
{
	if (pkt == NULL || (pkt->type != WD_ADD_NODE_MESSAGE && pkt->type != WD_INFO_MESSAGE))
		return NULL;
	if (pkt->data == NULL || pkt->len <= 0)
		return NULL;
	return get_watchdog_node_from_json(pkt->data,pkt->len, authkey);
}

static int read_from_socket(int sock, void* buf, size_t len, int timeout)
{
	int ret, read_len;
	read_len = 0;

	while (read_len < len)
	{
		ret = read(sock, buf + read_len, (len - read_len));
		if(ret < 0)
			ereport(DEBUG1,
				(errmsg("error reading from socket"),
					 errdetail("read from socket failed with error \"%s\"",strerror(errno))));
		if(ret <= 0)
			return ret;
		read_len +=ret;
	}
	return read_len;
}

static WDPacketData* read_packet(SocketConnection* conn)
{
	return read_packet_of_type(conn, WD_NO_MESSAGE);
}

static WDPacketData* read_packet_of_type(SocketConnection* conn, char ensure_type)
{
	char type;
	int len;
	unsigned int cmd_id;
	char* buf;
	WDPacketData* pkt = NULL;
	int ret;

	if (is_socket_connection_connected(conn) == false)
	{
		ereport(LOG,
			(errmsg("error reading from socket connection,socket is not connected")));
		return NULL;
	}

	ret = read_from_socket(conn->sock,&type, sizeof(char), 1 );
	if (ret != sizeof(char))
	{
		close_socket_connection(conn);
		return NULL;
	}

	ereport(DEBUG1,
			(errmsg("PACKET TYPE %c while need packet type %c",type,ensure_type)));
	
	if (ensure_type != WD_NO_MESSAGE && ensure_type != type)
	{
		/* The packet type is not what we want.*/
		ereport(DEBUG1,
				(errmsg("invalid packet type. expecting %c while received %c",ensure_type,type)));
		close_socket_connection(conn);
		return NULL;
	}
	
	ret = read_from_socket(conn->sock, &cmd_id, sizeof(int) ,1);
	if (ret != sizeof(int))
	{
		close_socket_connection(conn);
		return NULL;
	}
	cmd_id = ntohl(cmd_id);
	
	ereport(DEBUG3,
			(errmsg("PACKET COMMAND ID %d",cmd_id)));
	
	ret = read_from_socket(conn->sock, &len, sizeof(int), 1);
	if (ret != sizeof(int))
	{
		close_socket_connection(conn);
		return NULL;
	}
	
	len = ntohl(len);

	ereport(DEBUG2,
			(errmsg("PACKET DATA LENGTH %d",len)));
	
	pkt = get_empty_packet();
	set_message_type(pkt, type);
	set_message_commandID(pkt, cmd_id);

	buf = palloc(len);

	ret = read_from_socket(conn->sock, buf, len,1);
	if (ret != len)
	{
		close_socket_connection(conn);
		free_packet(pkt);
		pfree(buf);
		return NULL;
	}
	set_message_data(pkt, buf, len);
	return pkt;
}


/*
 * sets the state of local watchdog node, and fires an state change event
 * if the new and old state differes
 */

static int set_local_node_state(WD_STATES newState)
{
	WD_STATES oldState = g_cluster.localNode->state;
	g_cluster.localNode->state = newState;
	if (oldState != newState)
		watchdog_state_machine(WD_EVENT_WD_STATE_CHANGED, NULL, NULL);
	return 0;
}



static void
wd_child_exit(int exit_signo)
{
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	exit(0);
}

/* Function invoked when watchdog process is about to exit */
static void wd_system_will_go_down(int code, Datum arg)
{
	int i;
	ereport(LOG,
			(errmsg("Watchdog child is shutting down")));
	
	send_cluster_command(NULL, WD_INFORM_I_AM_GOING_DOWN, 0);
	
	if (get_local_node_state() == WD_COORDINATOR)
		resign_from_coordinator();
	/* close all sockets */
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
		close_socket_connection(&wdNode->client_socket);
		close_socket_connection(&wdNode->server_socket);
	}
}

static void close_socket_connection(SocketConnection* conn)
{
	if ((conn->sock > 0 && conn->sock_state == WD_SOCK_CONNECTED)
		|| conn->sock_state == WD_SOCK_WAITING_FOR_CONNECT)
	{
		close(conn->sock);
		conn->sock = -1;
		conn->sock_state = WD_SOCK_CLOSED;
	}
}

static bool is_socket_connection_connected(SocketConnection* conn)
{
	return (conn->sock > 0 && conn->sock_state == WD_SOCK_CONNECTED);
}


static int accept_incomming_connections(fd_set* rmask, int pending_fds_count)
{
	int processed_fds = 0;
	int fd;
	
	if ( FD_ISSET(g_cluster.localNode->server_socket.sock, rmask) )
	{
		struct sockaddr_in addr;
		socklen_t addrlen = sizeof(struct sockaddr_in);
		processed_fds++;
		fd = accept(g_cluster.localNode->server_socket.sock, (struct sockaddr *)&addr, &addrlen);
		if (fd < 0)
		{
			if ( errno == EINTR || errno == 0 || errno == EAGAIN || errno == EWOULDBLOCK )
			{
				/* nothing to accept now */
				ereport(DEBUG2,
						(errmsg("Failed to accept incoming watchdog connection, Nothing to accept")));
			}
			/* accept failed */
			ereport(DEBUG1,
					(errmsg("Failed to accept incomming watchdog connection")));
		}
		else
		{
			MemoryContext oldCxt = MemoryContextSwitchTo(TopMemoryContext);
			SocketConnection *conn = palloc(sizeof(SocketConnection));
			
			conn->sock = fd;
			conn->sock_state = WD_SOCK_CONNECTED;
			gettimeofday(&conn->tv, NULL);
			strncpy(conn->addr,inet_ntoa(addr.sin_addr),sizeof(conn->addr));
			ereport(LOG,
					(errmsg("new watchdog node connection is received from \"%s:%d\"",inet_ntoa(addr.sin_addr),addr.sin_port)));
			g_cluster.unidentified_socks = lappend(g_cluster.unidentified_socks, conn);
			MemoryContextSwitchTo(oldCxt);
		}
	}
	
	if (processed_fds >= pending_fds_count)
		return processed_fds;
	
	if ( FD_ISSET(g_cluster.command_server_sock, rmask) )
	{
		struct sockaddr addr;
		socklen_t addrlen = sizeof(struct sockaddr);
		processed_fds++;
		
		int fd = accept(g_cluster.command_server_sock, &addr, &addrlen);
		if (fd < 0)
		{
			if ( errno == EINTR || errno == 0 || errno == EAGAIN || errno == EWOULDBLOCK )
			{
				/* nothing to accept now */
				ereport(WARNING,
						(errmsg("Failed to accept incoming watchdog IPC connection, Nothing to accept")));
			}
			/* accept failed */
			ereport(WARNING,
					(errmsg("Failed to accept incoming watchdog IPC connection")));
		}
		else
		{
			MemoryContext oldCxt = MemoryContextSwitchTo(TopMemoryContext);
			ereport(LOG,
					(errmsg("new IPC connection is received ")));
			g_cluster.ipc_command_socks = lappend_int(g_cluster.ipc_command_socks, fd);
			MemoryContextSwitchTo(oldCxt);
		}
	}
	
	return processed_fds;
}

static int update_successful_outgoing_cons(fd_set* wmask, int pending_fds_count)
{
	int i;
	int count = 0;
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
		
		if (wdNode->client_socket.sock > 0 && wdNode->client_socket.sock_state == WD_SOCK_WAITING_FOR_CONNECT)
		{
			if ( FD_ISSET(wdNode->client_socket.sock, wmask) )
			{
				socklen_t lon;
				int valopt;
				lon = sizeof(int);

				gettimeofday(&wdNode->client_socket.tv, NULL);

				getsockopt(wdNode->client_socket.sock, SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon);
				if (valopt)
				{
					ereport(LOG,
							(errmsg("error in outbond connection to %s:%d",wdNode->hostname,wdNode->wd_port),
							 errdetail("%s",strerror(valopt))));
					close_socket_connection(&wdNode->client_socket);
					wdNode->client_socket.sock_state = WD_SOCK_ERROR;
				}
				else
				{
					wdNode->client_socket.sock_state = WD_SOCK_CONNECTED;
					ereport(LOG,
							(errmsg("new outbond connection to %s:%d ",wdNode->hostname,wdNode->wd_port)));
					
					/* set socket to blocking again */
					int flags = fcntl(wdNode->client_socket.sock, F_GETFL, 0);
					fcntl(wdNode->client_socket.sock, F_SETFL, flags | ~O_NONBLOCK);
					watchdog_state_machine(WD_EVENT_NEW_OUTBOUND_CONNECTION, wdNode, NULL);
				}
				count++;
				if (count >= pending_fds_count)
					break;
			}
		}
	}
	return count;
}

static bool write_packet_to_socket(int sock, WDPacketData* pkt)
{
	int ret = 0;
	int command_id, len;
	ereport(LOG,
			(errmsg("sending watchdog packet Socket:%d, Type:%c, Command_ID:%d, data Length:%d",sock,pkt->type, pkt->command_id,pkt->len)));
	
	/* TYPE */
	if (write(sock, &pkt->type, 1) < 1)
		return false;
	/* COMMAND */
	command_id = htonl(pkt->command_id);
	if (write(sock, &command_id, 4) < 4)
		return false;
	/* LENGTH */
	len = htonl(pkt->len);
	if (write(sock, &len, 4) < 4)
		return false;
	/* DATA */
	if (pkt->len > 0 && pkt->data)
	{
		int bytes_send = 0;
		do
		{
			ret = write(sock, pkt->data + bytes_send, (pkt->len - bytes_send));
			if (ret <=0)
				return false;
			bytes_send += ret;
		}while (bytes_send < pkt->len);
	}
	return true;
}

static void init_wd_packet(WDPacketData* pkt)
{
	pkt->len = 0;
	pkt->data = NULL;
}

static WDPacketData* get_empty_packet(void)
{
	WDPacketData *pkt = palloc0(sizeof(WDPacketData));
	return pkt;
}

static void free_packet(WDPacketData *pkt)
{
	if (pkt)
	{
		if (pkt->data)
			pfree(pkt->data);
		pfree(pkt);
	}
}

static void set_message_type(WDPacketData* pkt, char type)
{
	pkt->type = type;
}

static void set_message_commandID(WDPacketData* pkt, unsigned int commandID)
{
	pkt->command_id = commandID;
}

static void set_next_commandID_in_message(WDPacketData* pkt)
{
	set_message_commandID(pkt,get_next_commandID());
}

static void set_message_data(WDPacketData* pkt, const char* data, int len)
{
	pkt->data = (char*)data;
	pkt->len = len;
}

static bool add_nodeinfo_to_json(JsonNode* jNode, WatchdogNode* node)
{
	jw_start_object(jNode, "WatchdogNode");
	
	jw_put_int(jNode, "ID",node?node->private_id:-1);
	jw_put_int(jNode, "State",node?node->state:-1);
	jw_put_string(jNode, "NodeName",node?node->nodeName:"Not Set");
	jw_put_string(jNode, "HostName", node?node->hostname:"Not Set");
	jw_put_string(jNode, "DelegateIP",node?node->delegate_ip:"Not Set");
	jw_put_int(jNode, "WdPort", node?node->wd_port:0);
	jw_put_int(jNode, "PgpoolPort", node?node->pgpool_port:0);
	
	jw_end_element(jNode);
	
	return true;
}

static JsonNode* get_node_list_json(int id)
{
	int i;
	JsonNode* jNode = jw_create_with_object(true);
	/* add the node count */
	if (id < 0)
	{
		jw_put_int(jNode, "NodeCount", g_cluster.remoteNodeCount + 1);
		/* add the array */
		jw_start_array(jNode, "WatchdogNodes");
		/* add the local node info */
		add_nodeinfo_to_json(jNode,g_cluster.localNode);
		/* add remote nodes */
		for (i=0; i< g_cluster.remoteNodeCount; i++)
		{
			WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
			add_nodeinfo_to_json(jNode, wdNode);
		}
	}
	else
	{
		jw_put_int(jNode, "NodeCount", 1);
		if (id == 0)
		{
			/* add the local node info */
			add_nodeinfo_to_json(jNode,g_cluster.localNode);
		}
		else
		{
			/* find from remote nodes */
			WatchdogNode* wdNodeToAdd = NULL;
			for (i=0; i< g_cluster.remoteNodeCount; i++)
			{
				WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
				if (wdNode->private_id == id)
				{
					wdNodeToAdd = wdNode;
					break;
				}
			}
			add_nodeinfo_to_json(jNode, wdNodeToAdd);
		}
	}
	jw_finish_document(jNode);
	return jNode;
}


static WDPacketData* get_addnode_message(void)
{
	char authhash[MD5_PASSWD_LEN + MD5_PASSWD_LEN + 10]; //TODO
	WDPacketData *message = get_empty_packet();
	bool include_hash = get_authhash_for_node(g_cluster.localNode, authhash);
	char *json_data = get_watchdog_node_info_json(g_cluster.localNode, include_hash?authhash:NULL);

	set_message_type(message, WD_ADD_NODE_MESSAGE);
	set_next_commandID_in_message(message);
	set_message_data(message,json_data,strlen(json_data));
	return message;
}

static WDPacketData* get_mynode_info_message(WDPacketData* replyFor)
{
	char authhash[MD5_PASSWD_LEN +1];
	WDPacketData *message = get_empty_packet();
	bool include_hash = get_authhash_for_node(g_cluster.localNode, authhash);
	char *json_data = get_watchdog_node_info_json(g_cluster.localNode, include_hash?authhash:NULL);

	set_message_type(message, WD_INFO_MESSAGE);
	if (replyFor == NULL)
		set_next_commandID_in_message(message);
	else
		set_message_commandID(message, replyFor->command_id);
	
	set_message_data(message, json_data,strlen(json_data));
	return message;
}

static WDPacketData* get_minimum_message(char type, WDPacketData* replyFor)
{
	/* TODO it is a waste of space */
	WDPacketData *message = get_empty_packet();
	set_message_type(message,type);
	if (replyFor == NULL)
		set_next_commandID_in_message(message);
	else
		set_message_commandID(message, replyFor->command_id);
	return message;
}


static WDIPCCommandData* get_wd_IPC_command_from_reply(WDPacketData* pkt)
{
	ListCell *lc;
	foreach(lc, g_cluster.ipc_commands)
	{
		WDIPCCommandData* ipcCommand = lfirst(lc);
		if (ipcCommand)
		{
			if (ipcCommand->internal_command_id == pkt->command_id)
				return ipcCommand;
		}
	}
	return NULL;
}

static WDIPCCommandData* get_wd_IPC_command_from_socket(int sock)
{
	ListCell *lc;
	foreach(lc, g_cluster.ipc_commands)
	{
		WDIPCCommandData* ipcCommand = lfirst(lc);
		if (ipcCommand)
		{
			if (ipcCommand->issueing_sock == sock)
				return ipcCommand;
		}
	}
	return NULL;
}


static void cleanUpIPCCommand(WDIPCCommandData* ipcCommand)
{
	/*
	 * close the socket associated with ipcCommand
	 * and remove it from ipcSocket list
	 */
	if (ipcCommand->issueing_sock > 0)
	{
		close(ipcCommand->issueing_sock);
		g_cluster.ipc_command_socks = list_delete_int(g_cluster.ipc_command_socks,ipcCommand->issueing_sock);
		ipcCommand->issueing_sock = -1;
	}
	/* Now remove the ipcCommand instance from the command list */
	g_cluster.ipc_commands = list_delete_ptr(g_cluster.ipc_commands,ipcCommand);
	/*
	 * Finally the memory part
	 * As everything of IPCCommand live inside its own memory context.
	 * Delete the MemoryContext and we are good
	 */
	MemoryContextDelete(ipcCommand->memoryContext);
}

static int standard_packet_processor(WatchdogNode* wdNode, WDPacketData* pkt)
{
	WDPacketData* replyPkt = NULL;
	switch (pkt->type)
	{
		case WD_ASK_FOR_POOL_CONFIG:
		{
			char* config_data = get_pool_config_json();
			
			if (config_data == NULL)
				reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
			else
			{
				replyPkt = get_empty_packet();
				set_message_type(replyPkt, WD_POOL_CONFIG_DATA);
				set_message_commandID(replyPkt, pkt->command_id);
				set_message_data(replyPkt, config_data , strlen(config_data));
			}
		}
			break;
			
		case WD_POOL_CONFIG_DATA:
		{
			/* we only accept config data from coordinator node */
			if (wdNode == g_cluster.masterNode && pkt->data)
			{
				POOL_CONFIG* master_config = get_pool_config_from_json(pkt->data, pkt->len);
				if (master_config)
				{
					printf("\n%s\n",pkt->data);
					verify_pool_configurations(master_config);
				}
				
			}
			
		}
			break;
			
		case WD_ADD_NODE_MESSAGE:
		case WD_REQ_INFO_MESSAGE:
			replyPkt = get_mynode_info_message(pkt);
			break;
			
		case WD_INFO_MESSAGE:
		{
			char *authkey = NULL;
			WatchdogNode* tempNode = parse_node_info_message(pkt, &authkey);
			wdNode->state = tempNode->state;
			wdNode->tv.tv_sec = tempNode->tv.tv_sec;
			strlcpy(wdNode->nodeName, tempNode->nodeName, WD_MAX_HOST_NAMELEN);
			wdNode->state = tempNode->state;
			wdNode->tv.tv_sec = tempNode->tv.tv_sec;
			wdNode->wd_priority = tempNode->wd_priority;
			
			printf("NODE INFO MESSAGE RECEVD\n");
			print_watchdog_node_info(wdNode);

			if (authkey)
				pfree(authkey);

			if (wdNode->state == WD_COORDINATOR)
			{
				/* TODO check if we already have the coordinator */
				if (g_cluster.masterNode != NULL && g_cluster.masterNode != wdNode)
				{
					ereport(WARNING,(errmsg("WE already have the coordinator...")));
					/* What should be the best way to handle it */
				}
				g_cluster.masterNode = wdNode;
			}
			pfree(tempNode);
		}
			break;
			
		case WD_INTERLOCKING_REQUEST:
			printf("\n\t\t\t %s:%d INTERLOCKING REQUEST RECEIVED \n",__FUNCTION__,__LINE__);
			node_has_requested_for_interlocking(wdNode, pkt);
			break;
			
		case WD_INTERUNLOCKING_REQUEST:
			printf("\n\t\t\t %s:%d UNLOCKING_REQUEST REQUEST RECEIVED \n",__FUNCTION__,__LINE__);
			node_has_resigned_from_interlocking(wdNode, pkt);
			break;
			
		case WD_JOIN_COORDINATOR_MESSAGE:
		{
			/*
			 * if I am coordinator reply with accept,
			 * otherwise reject
			 */
			if (g_cluster.localNode == g_cluster.masterNode)
				reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
			else
				reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
		}
			
		case WD_IAM_COORDINATOR_MESSAGE:
		{
			/*
			 * if the message is received from coordinator reply with infor,
			 * otherwise reject
			 */
			if (g_cluster.masterNode != NULL && wdNode != g_cluster.masterNode)
			{
				ereport(NOTICE,
						(errmsg("cluster is in split brain")));
				reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
			}
			else
				replyPkt = get_mynode_info_message(pkt);
		}
			break;

		default:
			break;
	}
	if (replyPkt)
	{
		send_message_to_node(wdNode,replyPkt);
		pfree(replyPkt);
	}
	return 1;
}


static bool send_message_to_connection(SocketConnection* conn, WDPacketData *pkt)
{
	if (conn->sock > 0 && conn->sock_state == WD_SOCK_CONNECTED)
	{
		if (write_packet_to_socket(conn->sock, pkt) == true)
			return true;
		close_socket_connection(conn);
	}
	return false;
}

static bool send_message_to_node(WatchdogNode* wdNode, WDPacketData *pkt)
{
	if (send_message_to_connection(&wdNode->client_socket,pkt) == true)
		return true;
	if (send_message_to_connection(&wdNode->server_socket,pkt) == true)
		return true;
	return false;
}

/*
 * If wdNode is NULL message is sent to all nodes
 * Returns the number of nodes the message is sent to
 */
static int send_message(WatchdogNode* wdNode, WDPacketData *pkt)
{
	int i,count = 0;
	if (wdNode)
	{
		if (wdNode == g_cluster.localNode) /*Always return 1 if I myself is intended receiver */
			return 1;
		if (send_message_to_node(wdNode,pkt))
			return 1;
		return 0;
	}
	/* NULL means send to all nodes */
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		wdNode = &(g_cluster.remoteNodes[i]);
		if (send_message_to_node(wdNode,pkt))
			count++;
	}
	return count;
}


static bool watchdog_internal_command_packet_processor(WatchdogNode* wdNode, WDPacketData* pkt)
{
	int i;
	WDCommandNodeResult* nodeResult = NULL;
	/* verify the packet is reply for our command */
	if (pkt->command_id != g_cluster.currentCommand.packet.command_id)
		return false;
	if (g_cluster.currentCommand.commandStatus != COMMAND_IN_PROGRESS)
		return false;

	if (pkt->type != WD_ERROR_MESSAGE  &&
		pkt->type != WD_ACCEPT_MESSAGE &&
		pkt->type != WD_REJECT_MESSAGE &&
		pkt->type != WD_INFO_MESSAGE)
		return false;

	if (pkt->type == WD_INFO_MESSAGE)
		standard_packet_processor(wdNode, pkt);

	/* get the result node for */
	for (i = 0; i< g_cluster.remoteNodeCount; i++)
	{
		WDCommandNodeResult* nodeRes = &g_cluster.currentCommand.nodeResults[i];
		clear_command_node_result(nodeRes);
		if (nodeRes->wdNode == wdNode)
		{
			nodeResult = nodeRes;
			break;
		}
	}
	if (nodeResult == NULL)
	{
		ereport(NOTICE,(errmsg("unable to find node result")));
		return true;
	}

	ereport(LOG,
			(errmsg("Watchdog node \"%s\" has replied for command id %d",nodeResult->wdNode->nodeName,pkt->command_id)));

	nodeResult->result_type = pkt->type;
	nodeResult->cmdState = COMMAND_STATE_REPLIED;
	g_cluster.currentCommand.commandReplyFromCount++;

	printf("----*****----- [%d] reply_from_count = %d AND sendTo_count = %d\n",__LINE__,g_cluster.currentCommand.commandReplyFromCount,g_cluster.currentCommand.commandSendToCount);

	if (g_cluster.currentCommand.commandReplyFromCount >= g_cluster.currentCommand.commandSendToCount)
	{
		g_cluster.currentCommand.commandFinished = true;
		if (pkt->type == WD_REJECT_MESSAGE || pkt->type == WD_ERROR_MESSAGE)
			g_cluster.currentCommand.commandStatus = COMMAND_FINISHED_NODE_REJECTED;
		else
			g_cluster.currentCommand.commandStatus = COMMAND_FINISHED_ALL_REPLIED;
		watchdog_state_machine(WD_EVENT_COMMAND_FINISHED, wdNode, pkt);
	}
	else if (pkt->type == WD_REJECT_MESSAGE || pkt->type == WD_ERROR_MESSAGE)
	{
		/* Error or reject message by any node imidiately finishes the command */
		g_cluster.currentCommand.commandFinished = true;
		g_cluster.currentCommand.commandStatus = COMMAND_FINISHED_NODE_REJECTED;
		watchdog_state_machine(WD_EVENT_COMMAND_FINISHED, wdNode, pkt);
	}

	return true; /* do not process this packet further */
}


static void check_for_current_command_timeout(void)
{
	struct timeval currTime;
	if (g_cluster.currentCommand.commandStatus != COMMAND_IN_PROGRESS ||
		g_cluster.currentCommand.commandFinished != 0)
		return;

	gettimeofday(&currTime,NULL);
	if (WD_TIME_DIFF_SEC(currTime,g_cluster.currentCommand.commandTime) >=  g_cluster.currentCommand.commandTimeoutSecs)
	{
		g_cluster.currentCommand.commandFinished = true;
		g_cluster.currentCommand.commandStatus = COMMAND_FINISHED_TIMEOUT;
		watchdog_state_machine(WD_EVENT_COMMAND_FINISHED, NULL, NULL);
	}
}

static char get_current_command_resultant_message_type(void)
{
	char res = WD_ACCEPT_MESSAGE;
	int i;
	if (g_cluster.currentCommand.commandFinished == 0)
		return WD_NO_MESSAGE;
	if (g_cluster.currentCommand.sendToNode == NULL)
	{
		/* The command was for all nodes */
		for (i = 0; i< g_cluster.remoteNodeCount; i++)
		{
			WDCommandNodeResult* nodeRes = &g_cluster.currentCommand.nodeResults[i];
			if (nodeRes->cmdState != COMMAND_STATE_REPLIED)
				continue;
			if (nodeRes->result_type != WD_ACCEPT_MESSAGE && nodeRes->result_type != WD_INFO_MESSAGE)
			{
				/* failed */
				if (res != WD_ERROR_MESSAGE)
					res = nodeRes->result_type;
			}
		}
	}
	else
	{
		if (g_cluster.currentCommand.commandSendToCount == 0) /* We failed to send to any node */
			return WD_ERROR_MESSAGE;
		if (g_cluster.currentCommand.commandReplyFromCount == 0) /* We got no reply */
			return WD_ERROR_MESSAGE;
		for (i = 0; i< g_cluster.remoteNodeCount; i++)
		{
			WDCommandNodeResult* nodeRes = &g_cluster.currentCommand.nodeResults[i];
			if (nodeRes->wdNode == g_cluster.currentCommand.sendToNode)
			{
				if (nodeRes->cmdState != COMMAND_STATE_REPLIED)
					return WD_ERROR_MESSAGE;
				return nodeRes->result_type;
			}
		}
		return WD_ERROR_MESSAGE;
	}
	return res;
}

static void clear_current_command(void)
{
	g_cluster.currentCommand.commandStatus = COMMAND_EMPTY;
	g_cluster.currentCommand.packet.type = WD_NO_MESSAGE;
	if (g_cluster.currentCommand.packet.data)
		pfree(g_cluster.currentCommand.packet.data);
}

/*
 * If wdNode is NULL message is sent to all nodes
 * Returns the number of nodes the message is sent to
 */
static int issue_watchdog_internal_command(WatchdogNode* wdNode, WDPacketData *pkt, int timeout_sec)
{
	int i;
	bool save_message = false;
	/* clear the curretn command */
	gettimeofday(&g_cluster.currentCommand.commandTime, NULL);

	g_cluster.currentCommand.commandTimeoutSecs = timeout_sec;
	g_cluster.currentCommand.packet.type = pkt->type;
	g_cluster.currentCommand.packet.command_id = pkt->command_id;
	g_cluster.currentCommand.packet.len = 0;
	g_cluster.currentCommand.packet.data = NULL;

	g_cluster.currentCommand.sendToNode = wdNode;
	g_cluster.currentCommand.commandSendToCount = 0;
	g_cluster.currentCommand.commandReplyFromCount = 0;
	g_cluster.currentCommand.commandStatus = COMMAND_IN_PROGRESS;

	if (wdNode == NULL) /* This is send to all */
	{
		for (i = 0; i< g_cluster.remoteNodeCount; i++)
		{
			WDCommandNodeResult* nodeResult = &g_cluster.currentCommand.nodeResults[i];
			clear_command_node_result(nodeResult);
			if (nodeResult->wdNode->state == WD_DEAD )
			{
				/* Do not send to dead nodes */
				nodeResult->cmdState = COMMAND_STATE_DO_NOT_SEND;
			}
			else
			{
				if (send_message_to_node(nodeResult->wdNode, pkt) == false)
				{
					/* failed to send. May be try again later */
					save_message = true;
					nodeResult->cmdState = COMMAND_STATE_SEND_ERROR;
				}
				else
				{
					nodeResult->cmdState = COMMAND_STATE_SENT;
					g_cluster.currentCommand.commandSendToCount++;
				}
			}
		}
	}
	if (wdNode)
	{
		WDCommandNodeResult* nodeResult = NULL;
		for (i = 0; i< g_cluster.remoteNodeCount; i++)
		{
			WDCommandNodeResult* nodeRes = &g_cluster.currentCommand.nodeResults[i];
			clear_command_node_result(nodeRes);
			if (nodeRes->wdNode == wdNode)
				nodeResult = nodeRes;
		}
		if (nodeResult == NULL)
		{
			/* should never hapen */
			return -1;
		}
		if (send_message_to_node(nodeResult->wdNode, pkt) == false)
		{
			/* failed to send. May be try again later */
			save_message = true;
			nodeResult->cmdState = COMMAND_STATE_SEND_ERROR;
		}
		else
		{
			nodeResult->cmdState = COMMAND_STATE_SENT;
			g_cluster.currentCommand.commandSendToCount++;
		}
	}
	if (save_message && pkt->len > 0)
	{
		g_cluster.currentCommand.packet.data = MemoryContextAlloc(TopMemoryContext,pkt->len);
		memcpy(g_cluster.currentCommand.packet.data,pkt->data,pkt->len);
		g_cluster.currentCommand.packet.len = pkt->len;
	}
	g_cluster.currentCommand.commandFinished = false;
	return g_cluster.currentCommand.commandSendToCount;
}

static int update_connected_node_count(void)
{
	int i;
	g_cluster.aliveNodeCount = 0;
	for (i = 0; i< g_cluster.remoteNodeCount; i++)
	{
		WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
		if (wdNode->state == WD_DEAD)
			continue;
		if (is_socket_connection_connected(&wdNode->client_socket))
			g_cluster.aliveNodeCount++;
		else if (is_socket_connection_connected(&wdNode->server_socket))
			g_cluster.aliveNodeCount++;
	}
	return g_cluster.aliveNodeCount;
}

static void update_nodes_connection_status(void)
{
	int i;
	for (i = 0; i< g_cluster.remoteNodeCount; i++)
	{
		bool conectable = false;
		WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
		if (is_socket_connection_connected(&wdNode->client_socket))
			conectable = true;
		else if (is_socket_connection_connected(&wdNode->server_socket))
			conectable = true;
		if (wdNode->is_connectable && conectable)	/* new and old status is same */
			continue;
		wdNode->is_connectable = conectable;
		watchdog_state_machine(wdNode->is_connectable?WD_EVENT_NODE_CON_FOUND:WD_EVENT_NODE_CON_LOST, wdNode, NULL);
	}
}

static void service_lost_connections(void)
{
	int i;
	struct timeval currTime;
	gettimeofday(&currTime,NULL);
	for (i = 0; i< g_cluster.remoteNodeCount; i++)
	{
		WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
		if (is_socket_connection_connected(&wdNode->client_socket) == false)
		{
			if (WD_TIME_DIFF_SEC(currTime,wdNode->client_socket.tv) <=  MIN_SECS_CONNECTION_RETRY)
				continue;

			if (wdNode->client_socket.sock_state != WD_SOCK_WAITING_FOR_CONNECT)
				connect_to_node(wdNode);
		}
	}
}


/*
 * The function only considers the node state.
 * All node states conut towards the cluster participating nodes
 * except the dead and lost nodes.
 */
static int get_cluster_node_count(void)
{
	int i;
	int count = 0;
	for (i = 0; i< g_cluster.remoteNodeCount; i++)
	{
		WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
		if (wdNode->state == WD_DEAD || wdNode->state == WD_LOST)
			continue;
		count++;
	}
	return count;
}


static int send_cluster_command(WatchdogNode* wdNode, char type, int timeout_sec)
{
	WDPacketData *pkt = NULL;
	int ret = 0;
	switch (type)
	{
		case WD_INFO_MESSAGE:
			pkt = get_mynode_info_message(NULL);
			break;
		case WD_ADD_NODE_MESSAGE:
			pkt = get_addnode_message();
			break;
			
		case WD_REQ_INFO_MESSAGE:
		case WD_IAM_COORDINATOR_MESSAGE:
		case WD_STAND_FOR_COORDINATOR_MESSAGE:
		case WD_DECLARE_COORDINATOR_MESSAGE:
		case WD_JOIN_COORDINATOR_MESSAGE:
		case WD_QUORUM_IS_LOST:
		case WD_INFORM_I_AM_GOING_DOWN:
		case WD_ASK_FOR_POOL_CONFIG:
			pkt = get_minimum_message(type, NULL);
			break;
		default:
			ereport(LOG,(errmsg("invalid command message type %c",type)));
			break;
	}
	if (pkt)
	{
		ret = issue_watchdog_internal_command(wdNode, pkt, timeout_sec);
		free_packet(pkt);
	}
	return ret;
}

static bool reply_with_minimal_message(WatchdogNode* wdNode, char type, WDPacketData* replyFor)
{
	WDPacketData *pkt = get_minimum_message(type,replyFor);
	int ret = send_message(wdNode, pkt);
	free_packet(pkt);
	return ret;
}

static bool reply_with_message(WatchdogNode* wdNode, char type, char* data, int data_len, WDPacketData* replyFor)
{
	WDPacketData wdPacket;
	int ret;
	init_wd_packet(&wdPacket);
	set_message_type(&wdPacket, type);
	
	if (replyFor == NULL)
		set_next_commandID_in_message(&wdPacket);
	else
		set_message_commandID(&wdPacket, replyFor->command_id);
	
	set_message_data(&wdPacket, data, data_len);
	ret = send_message(wdNode, &wdPacket);
	return ret;
}

static inline WD_STATES get_local_node_state(void)
{
	return g_cluster.localNode->state;
}


/*
 * returns true if no message is swollowed by the
 * processor and no further action is required
 */
static bool wd_commands_packet_processor(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	WDIPCCommandData* ipcCommand;
	
	if (event != WD_EVENT_PACKET_RCV)
		return false;
	if (pkt == NULL)
		return false;
	
	if (pkt->type == WD_FAILOVER_CMD_SYNC_REQUEST)
	{
		process_failover_command_sync_requests(wdNode, pkt, NULL);
		return true;
	}
	
	if (pkt->type == WD_REPLICATE_VARIABLE_REQUEST)
	{
		process_pgpool_replicate_command(wdNode, pkt);
		return true;
	}
	
	if (pkt->type == WD_INTERLOCKING_REQUEST)
	{
		printf("\n\t\t\t %s:%d INTERLOCKING REQUEST RECEIVED \n",__FUNCTION__,__LINE__);
		node_has_requested_for_interlocking(wdNode, pkt);
		return true;
	}
	
	if (pkt->type == WD_INTERUNLOCKING_REQUEST)
	{
		printf("\n\t\t\t %s:%d UNLOCKING_REQUEST REQUEST RECEIVED \n",__FUNCTION__,__LINE__);
		node_has_resigned_from_interlocking(wdNode, pkt);
		return true;
	}
	
	if (pkt->type == WD_DATA_MESSAGE)
	{
		ipcCommand = get_wd_IPC_command_from_reply(pkt);
		if (ipcCommand)
		{
			int res_len = htonl(pkt->len);
			char res_type = WD_IPC_CMD_RESULT_OK;
			
			write(ipcCommand->issueing_sock, &res_type, 1);
			write(ipcCommand->issueing_sock, &res_len, 4);
			if (pkt->len > 0)
				write(ipcCommand->issueing_sock, pkt->data, pkt->len);
			/* ok we are done
			 * delete this command
			 */
			cleanUpIPCCommand(ipcCommand);
			return true; /* do not process this packet further */
		}
		return false;
		
	}
	
	
	if (pkt->type == WD_ACCEPT_MESSAGE ||
		pkt->type == WD_REJECT_MESSAGE ||
		pkt->type == WD_ERROR_MESSAGE)
	{
		ipcCommand = get_wd_IPC_command_from_reply(pkt);
		if (ipcCommand == NULL)
		{
			return false;
		}
		
		if (ipcCommand->type == WD_INTERLOCKING_REQUEST ||
			ipcCommand->type == WD_INTERUNLOCKING_REQUEST ||
			ipcCommand->type == WD_FAILOVER_CMD_SYNC_REQUEST)
		{
			/*
			 * we are expecting only one reply for this
			 * and we got that.
			 */
			printf("\t\t\t %s:%d \n",__FUNCTION__,__LINE__);
			
			int res_len = 0;
			char res_type = WD_IPC_CMD_RESULT_BAD;
			if (pkt->type == WD_ACCEPT_MESSAGE)
			{
				/* okay we are the lock holder */
				printf("\t\t\t %s:%d \n",__FUNCTION__,__LINE__);
				
				g_cluster.lockHolderNode = g_cluster.localNode;
				res_type = WD_IPC_CMD_RESULT_OK;
			}
			printf("\t\t\t %s:%d \n",__FUNCTION__,__LINE__);
			
			write(ipcCommand->issueing_sock, &res_type, 1);
			write(ipcCommand->issueing_sock, &res_len, 4);
			/*
			 * ok we are done, delete this command
			 */
			cleanUpIPCCommand(ipcCommand);
			
			return true; /* do not process this packet further */
		}
		
		if (ipcCommand->type == WD_REPLICATE_VARIABLE_REQUEST)
			return reply_is_received_for_pgpool_replicate_command(wdNode, pkt, ipcCommand);
	}
	
	return false;
}

static int watchdog_state_machine(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	ereport(DEBUG1,
			(errmsg("STATE MACHINE INVOKED WITH EVENT = %s Current State = %s",wd_event_name[event], debug_states[get_local_node_state()])));
	
	if (event == WD_EVENT_REMOTE_NODE_LOST)
	{
		wdNode->state = WD_LOST;
		if (wdNode == g_cluster.masterNode)
			g_cluster.masterNode = NULL;
	}

//	static int issue_watchdog_internal_command(WatchdogNode* wdNode, WDPacketData *pkt, int timeout_sec);
//	static char get_current_command_resultant_message_type(void);
//	static void check_for_current_command_timeout(void);
//	static bool watchdog_internal_command_packet_processor(WatchdogNode* wdNode, WDPacketData* pkt);

	else if (event == WD_EVENT_PACKET_RCV)
	{
		print_packet_info(pkt,wdNode);
		if (pkt->type == WD_INFO_MESSAGE)
			standard_packet_processor(wdNode, pkt);
		if (pkt->type == WD_INFORM_I_AM_GOING_DOWN)		/* TODO do it better way */
			return watchdog_state_machine(WD_EVENT_REMOTE_NODE_LOST, wdNode, NULL);
		if (watchdog_internal_command_packet_processor(wdNode,pkt) == true)
			return 0;
	}
	else if (event == WD_EVENT_NEW_OUTBOUND_CONNECTION)
	{
		WDPacketData* addPkt = get_addnode_message();
		send_message(wdNode, addPkt);
		free_packet(addPkt);
	}

	if (wd_commands_packet_processor(event, wdNode, pkt) == true)
		return 0;

	switch (get_local_node_state())
	{
		case WD_LOADING:
			watchdog_state_machine_loading(event,wdNode,pkt);
			break;
		case WD_JOINING:
			watchdog_state_machine_joining(event,wdNode,pkt);
			break;
		case WD_INITIALIZING:
			watchdog_state_machine_initializing(event,wdNode,pkt);
			break;
		case WD_COORDINATOR:
			watchdog_state_machine_coordinator(event,wdNode,pkt);
			break;
		case WD_PARTICIPATE_IN_ELECTION:
			watchdog_state_machine_voting(event,wdNode,pkt);
			break;
		case WD_STAND_FOR_COORDINATOR:
			watchdog_state_machine_standForCord(event,wdNode,pkt);
			break;
		case WD_STANDBY:
			watchdog_state_machine_standby(event,wdNode,pkt);
			break;
		case WD_WAITING_FOR_QUORUM:
			watchdog_state_machine_waiting_for_quorum(event,wdNode,pkt);
			break;
		case WD_DEAD:
		case WD_IN_NW_TROUBLE:
			watchdog_state_machine_nw_error(event,wdNode,pkt);
			break;
		default:
			//?????
			break;
	}
	
	return 0;
}

/*
 * This is the state where the watchdog enters when starting up.
 * upon entering this state we sends ADD node message to all reachable
 * nodes.
 * Wait for 4 seconds if some node rejects us.
 */
static int watchdog_state_machine_loading(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	switch (event)
	{
		case WD_EVENT_WD_STATE_CHANGED:
		{
			int i;
			clear_current_command();
			WDPacketData* addPkt = get_addnode_message();
			/* set the status to ADD_MESSAGE_SEND by hand */
			for (i = 0; i< g_cluster.remoteNodeCount; i++)
			{
				WatchdogNode* wdTmpNode;
				wdTmpNode = &(g_cluster.remoteNodes[i]);
				if (wdTmpNode->client_socket.sock_state == WD_SOCK_CONNECTED && wdTmpNode->state == WD_DEAD)
				{
					if (send_message(wdTmpNode, addPkt))
						wdTmpNode->state = WD_ADD_MESSAGE_SENT;
				}
			}
			free_packet(addPkt);
			set_timeout(4);
		}
			break;

		case WD_EVENT_CON_OPEN:
			break;


		case WD_EVENT_TIMEOUT:
			set_state(WD_JOINING);
			break;

		case WD_EVENT_PACKET_RCV:
		{
			switch (pkt->type)
			{
				case WD_INFO_MESSAGE:
				{
					int i;
					bool all_replied = true;
					for (i = 0; i< g_cluster.remoteNodeCount; i++)
					{
						wdNode = &(g_cluster.remoteNodes[i]);
						if (wdNode->state == WD_ADD_MESSAGE_SENT)
						{
							all_replied = false;
							break;
						}
					}
					if (all_replied)
					{
						/*
						 * we are already connected to all configured nodes
						 * Just move to initializing state
						 */
						set_state(WD_INITIALIZING);
					}
				}
					break;

				case WD_REJECT_MESSAGE:
					if (wdNode->state == WD_ADD_MESSAGE_SENT || wdNode->state == WD_DEAD)
						ereport(FATAL,
							(return_code(POOL_EXIT_FATAL),
							 errmsg("Add to watchdog cluster request is rejected by node \"%s:%d\"",wdNode->hostname,wdNode->wd_port),
								 errhint("check the watchdog configurations.")));
					break;
				default:
					standard_packet_processor(wdNode, pkt);
					break;
			}
		}
			break;
  default:
			break;
	}
	return 0;
}

/*
 * This is the intermediate state before going to cluster initialization
 * here we update the information of all connected nodes and move to the
 * initialization state. moving to this state from loading does not make
 * much sence as at loading time we already have updated node informations
 */
static int watchdog_state_machine_joining(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	switch (event)
	{
		case WD_EVENT_WD_STATE_CHANGED:
			g_cluster.masterNode = NULL;
			send_cluster_command(NULL, WD_REQ_INFO_MESSAGE, 5);
			set_timeout(5);
			break;

		case WD_EVENT_TIMEOUT:
			set_state(WD_INITIALIZING);
			break;

		case WD_EVENT_COMMAND_FINISHED:
		{
			if (g_cluster.currentCommand.packet.type == WD_REQ_INFO_MESSAGE)
				set_state(WD_INITIALIZING);
		}
			break;

		case WD_EVENT_PACKET_RCV:
		{
			switch (pkt->type)
			{
				case WD_REJECT_MESSAGE:
					if (wdNode->state == WD_ADD_MESSAGE_SENT)
						ereport(FATAL,
							(errmsg("Add to watchdog cluster request is rejected by node \"%s:%d\"",wdNode->hostname,wdNode->wd_port),
								 errhint("check the watchdog configurations.")));
					break;
				default:
					standard_packet_processor(wdNode, pkt);
					break;
			}
		}
			break;
			
  default:
			break;
	}
	return 0;
}

/*
 * This state only works on the local data and does not
 * sends any cluster command.
 */

static int watchdog_state_machine_initializing(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	switch (event)
	{
		case WD_EVENT_WD_STATE_CHANGED:
			clear_current_command();
			/* set 1 sec timeout, save ourself from recurrsion */
			set_timeout(1);
			break;

		case WD_EVENT_CON_OPEN:
			break;

		case WD_EVENT_TIMEOUT:
		{
			/*
			 * If master node exists in cluser, Join it
			 * otherwise try becoming a master
			 */
			if (g_cluster.masterNode)
			{
				/*
				 * we found the coordinator node in network.
				 * Just join the network
				 */
				set_state(WD_STANDBY);
			}
			else
			{
				/* check if the quorum exists */
				int quorum_status = get_quorum_status();
				if (quorum_status == -1)
				{
					ereport(LOG,
							(errmsg("We do not have enough nodes in cluster")));
					set_state(WD_WAITING_FOR_QUORUM);
				}
				else
				{
					/* check if any node is already standing for coordinator */
					int i;
					for (i=0; i< g_cluster.remoteNodeCount; i++)
					{
						WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
						if (wdNode->state == WD_STAND_FOR_COORDINATOR)
						{
							set_state(WD_PARTICIPATE_IN_ELECTION);
							return 0;
						}
					}
					/* stand for coordinator */
					set_state(WD_STAND_FOR_COORDINATOR);
				}
			}
		}
			break;
			
		case WD_EVENT_PACKET_RCV:
		{
			switch (pkt->type)
			{
				case WD_REJECT_MESSAGE:
					if (wdNode->state == WD_ADD_MESSAGE_SENT)
						ereport(FATAL,
								(errmsg("Add to watchdog cluster request is rejected by node \"%s:%d\"",wdNode->hostname,wdNode->wd_port),
								 errhint("check the watchdog configurations.")));
					break;
				default:
					standard_packet_processor(wdNode, pkt);
					break;
			}
		}

			break;

		default:
			break;
	}
	return 0;
}

static int watchdog_state_machine_standForCord(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	switch (event)
	{
		case WD_EVENT_WD_STATE_CHANGED:
			send_cluster_command(NULL, WD_STAND_FOR_COORDINATOR_MESSAGE, 5);
			/* wait for 5 seconds if someone rejects us*/
			set_timeout(5);
			break;

		case WD_EVENT_COMMAND_FINISHED:
		{
			if (g_cluster.currentCommand.packet.type == WD_STAND_FOR_COORDINATOR_MESSAGE)
			{
				if (g_cluster.currentCommand.commandStatus == COMMAND_FINISHED_ALL_REPLIED ||
					g_cluster.currentCommand.commandStatus == COMMAND_FINISHED_TIMEOUT)
				{
						set_state(WD_COORDINATOR);
				}
				else
				{
					/* command is finished but because of error */
					if (pkt)
					{
						if (pkt->type == WD_ERROR_MESSAGE)
						{
							ereport(LOG,
									(errmsg("our stand for coordinator request is rejected by node \"%s\"",wdNode->nodeName)));
							set_state(WD_JOINING);
						}
						else if (pkt->type == WD_REJECT_MESSAGE)
						{
							ereport(LOG,
									(errmsg("our stand for coordinator request is rejected by node \"%s\"",wdNode->nodeName)));
							set_state(WD_PARTICIPATE_IN_ELECTION);
						}
					}
					else
					{
						ereport(LOG,
								(errmsg("our stand for coordinator request is rejected by node \"%s\"",wdNode->nodeName)));
						set_state(WD_JOINING);
					}
				}
			}
		}
			break;

		case WD_EVENT_CON_OPEN:
			break;
			
		case WD_EVENT_TIMEOUT:
			set_state(WD_COORDINATOR);
			break;
			
		case WD_EVENT_PACKET_RCV:
		{
			switch (pkt->type)
			{
				case WD_STAND_FOR_COORDINATOR_MESSAGE:
					/* decide on base of priority */
					if (g_cluster.localNode->wd_priority > wdNode->wd_priority)
					{
						reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
					}
					else if (g_cluster.localNode->wd_priority == wdNode->wd_priority)
					{
						/* decide on base of starting time */
						if (g_cluster.localNode->tv.tv_sec <= wdNode->tv.tv_sec)/* I am older */
						{
							reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
						}
						else
						{
							reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
							set_state(WD_PARTICIPATE_IN_ELECTION);
						}
					}
					else
					{
						reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
						set_state(WD_PARTICIPATE_IN_ELECTION);
					}
					break;

				case WD_DECLARE_COORDINATOR_MESSAGE:
					/* meanwhile someone has declared itself coordinator accept it*/
					reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
					set_state(WD_JOINING);
					break;
				default:
					standard_packet_processor(wdNode, pkt);
					break;
			}
		}
			break;
			
		default:
			break;
	}
	return 0;
}

static int watchdog_state_machine_coordinator(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	switch (event)
	{
		case WD_EVENT_WD_STATE_CHANGED:
		{
			int i;
			send_cluster_command(NULL, WD_DECLARE_COORDINATOR_MESSAGE, 5);
			set_timeout(10);
			printf("\nI AM SELECTING MYSELF AS COORDINATOR NODE\n");
			for (i=0; i< g_cluster.remoteNodeCount; i++)
			{
				WatchdogNode* wdNode = &(g_cluster.remoteNodes[i]);
				printf("NODE INFORMATION for NODE %d\n",i);
				print_watchdog_node_info(wdNode);
				printf("___________\n");
			}
		}
			break;

		case WD_EVENT_COMMAND_FINISHED:
		{
			if (g_cluster.currentCommand.packet.type == WD_DECLARE_COORDINATOR_MESSAGE)
			{
				if (g_cluster.currentCommand.commandStatus == COMMAND_FINISHED_ALL_REPLIED ||
					g_cluster.currentCommand.commandStatus == COMMAND_FINISHED_TIMEOUT)
				{
					printf("\n**************DECLARE COORDINATOR MESSAGE ACCEPTED**************\n");
					g_cluster.masterNode = g_cluster.localNode;
					g_cluster.escalation_pid = fork_escalation_process();
				}
				else
				{
					/* command is finished but because of error */
					ereport(NOTICE,
							(errmsg("possible split brain scenario detected by \"%s\" node", wdNode->nodeName),
							 (errdetail("re-initializing cluster"))));
					set_state(WD_JOINING);
				}
			}
			
			if (g_cluster.currentCommand.packet.type == WD_IAM_COORDINATOR_MESSAGE)
			{
				if (g_cluster.currentCommand.commandStatus == COMMAND_FINISHED_ALL_REPLIED ||
					g_cluster.currentCommand.commandStatus == COMMAND_FINISHED_TIMEOUT)
				{
					printf("\n**************I AM COORDINATOR MESSAGE ACCEPTED**************\n");
				}
				else
				{
					/* command is finished but because of error */
					ereport(NOTICE,
							(errmsg("possible split brain scenario detected by \"%s\" node", wdNode->nodeName),
							 (errdetail("re-initializing cluster"))));
					set_state(WD_JOINING);
				}
			}
		}
			break;

		case WD_EVENT_CON_OPEN:
			break;
			
		case WD_EVENT_NW_IP_IS_REMOVED:
		{
			//			bool			holding_vip;
			//			bool			network_error;
			//			struct timeval  network_error_time;
			/* check if we were holding the virtual IP and it is now lost */
			if (g_cluster.holding_vip == true)
			{
				List* local_addresses = get_all_local_ips();
				if (local_addresses == NULL)
				{
					/*
					 * We have lost all IP addresses
					 * we are in network trouble. Just move to
					 * in network trouble state
					 */
					set_state(WD_IN_NW_TROUBLE);
				}
				else
				{
					/* We do have some IP addresses assigned
					 * so its not a total black-out
					 */
					ListCell *lc;
					bool vip_exists = false;
					foreach(lc, local_addresses)
					{
						char* ip = lfirst(lc);
						if (!strcmp(ip,g_cluster.localNode->delegate_ip))
						{
							vip_exists = true;
							break;
						}
					}
					if (vip_exists == false)
					{
						/* Okay this is the case when only our VIP is lost
						 * but network interface seems to be working fine
						 * try to re-aquire the VIP
						 */
						wd_IP_up();
					}
					list_free_deep(local_addresses);
					local_addresses = NULL;
				}
			}
		}
			break;
			
		case WD_EVENT_NW_IP_IS_ASSIGNED:
			break;
			
			
		case WD_EVENT_TIMEOUT:
			send_cluster_command(NULL, WD_IAM_COORDINATOR_MESSAGE, 10);
			/* check if the quorum still exists */
			int quorum_status = get_quorum_status();
			if (quorum_status == -1)
			{
				ereport(LOG,
						(errmsg("We do not have enough nodes in cluster")));
				set_state(WD_WAITING_FOR_QUORUM);
			}
			else
				set_timeout(20);
			break;
			
		case WD_EVENT_REMOTE_NODE_LOST:
		{
			ereport(LOG,
					(errmsg("life check reported \"%s\" is lost",wdNode->nodeName)));
			
			/*
			 * we have lost one remote connected node
			 * check if the quorum still exists
			 */
			int quorum_status = get_quorum_status();
			if (quorum_status == -1)
			{
				ereport(LOG,
						(errmsg("We have lost the quorum after loosing \"%s\"",wdNode->nodeName)));
				
				/*
				 * We have lost the qurum, and left with no reason to
				 * continue as a coordinator node
				 */
				set_state(WD_WAITING_FOR_QUORUM);
			}
			else
				ereport(DEBUG1,
						(errmsg("We have lost the node \"%s\" but quorum still holds",wdNode->nodeName)));
		}
			break;
			
		case WD_EVENT_LOCAL_NODE_LOST:
			ereport(NOTICE,
					(errmsg("Lifecheck reported we have been lost, resigning from master ")));
			resign_from_coordinator();
			set_state(WD_LOST);
			break;
			
		case WD_EVENT_PACKET_RCV:
		{
			switch (pkt->type)
			{
				case WD_STAND_FOR_COORDINATOR_MESSAGE:
					reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
					break;
				case WD_DECLARE_COORDINATOR_MESSAGE:
					ereport(NOTICE,
							(errmsg("We are corrdinator and another node tried a coup")));
					reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
					break;
					
				case WD_IAM_COORDINATOR_MESSAGE:
				{
					ereport(NOTICE,
							(errmsg("We are in split brain, resigning from master")));
					reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
					set_state(WD_JOINING);
				}
					break;
					
				default:
					standard_packet_processor(wdNode, pkt);
					break;
			}
		}
			break;
			
		default:
			break;
	}
	return 0;
}

/* We can get into this state if we detect the total
 * network blackout, Here we just keep waiting for the
 * network to come back, and when it does we re-initialize
 * the cluster state.
 */
static int watchdog_state_machine_nw_error(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	switch (event)
	{
		case WD_EVENT_WD_STATE_CHANGED:
			clear_current_command();
			set_timeout(2);
			break;
			
		case WD_EVENT_PACKET_RCV:
			/* Okay this is funny because according to us
			 * we are in network black out but yet we are
			 * able to receive the packet.
			 * Just check may be network is back and we are
			 * unable to detect it
			 */
			/* fall through */
		case WD_EVENT_TIMEOUT:
		case WD_EVENT_NW_IP_IS_ASSIGNED:
		{
			List* local_addresses = get_all_local_ips();
			if (local_addresses == NULL)
			{
				/* How come this is possible ??
				 * but if somehow this happens keep in the
				 * state and ignore the packet
				 */
			}
			else
			{
				/*
				 * Seems like the network is back
				 * just go on initialize the cluster
				 */
				/* we might have broken sockets when the network
				 * gets back. Send the request info message to all
				 * nodes to confirm socket state
				 */
				WDPacketData* pkt = get_minimum_message(WD_IAM_IN_NW_TROUBLE_MESSAGE, NULL);
				send_message(NULL,pkt);
				try_connecting_with_all_unreachable_nodes();
				pfree(pkt);
				list_free_deep(local_addresses);
				local_addresses = NULL;
				set_state(WD_LOADING);
			}
		}
			break;
			
		default:
			break;
	}
	return 0;
}

static void resign_from_coordinator(void)
{
	fork_plunging_process();
}

static int watchdog_state_machine_voting(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	switch (event)
	{
		case WD_EVENT_WD_STATE_CHANGED:
			clear_current_command();
			set_timeout(6);
			break;
			
		case WD_EVENT_CON_OPEN:
			break;
			
		case WD_EVENT_TIMEOUT:
			set_state(WD_JOINING);
			break;
			
		case WD_EVENT_LOCAL_NODE_LOST:
			set_state(WD_JOINING);
			break;
			
		case WD_EVENT_PACKET_RCV:
		{
			if(pkt == NULL)
			{
				ereport(LOG,
						(errmsg("packet is NULL")));
				break;
			}
			switch (pkt->type)
			{
				case WD_STAND_FOR_COORDINATOR_MESSAGE:
				{
					/* Check the node priority */
					if (wdNode->wd_priority >= g_cluster.localNode->wd_priority)
					{
						reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
					}
					else
					{
						reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
						set_state(WD_STAND_FOR_COORDINATOR);
					}
				}
					break;
				case WD_IAM_COORDINATOR_MESSAGE:
					set_state(WD_JOINING);
					break;
				case WD_DECLARE_COORDINATOR_MESSAGE:
					reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
					set_state(WD_INITIALIZING);
					break;
				default:
					standard_packet_processor(wdNode, pkt);
					break;
			}
		}
			break;
			
		default:
			break;
	}
	return 0;
}

static int watchdog_state_machine_standby(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	switch (event)
	{
		case WD_EVENT_WD_STATE_CHANGED:
			send_cluster_command(g_cluster.masterNode, WD_JOIN_COORDINATOR_MESSAGE, 5);
			break;
			
		case WD_EVENT_CON_OPEN:
			break;
			
		case WD_EVENT_TIMEOUT:
			break;
		
		case WD_EVENT_COMMAND_FINISHED:
		{
			if (g_cluster.currentCommand.packet.type == WD_JOIN_COORDINATOR_MESSAGE)
			{
				if (g_cluster.currentCommand.commandStatus == COMMAND_FINISHED_ALL_REPLIED ||
					g_cluster.currentCommand.commandStatus == COMMAND_FINISHED_TIMEOUT)
				{
					WDPacketData* pktAsk = get_minimum_message(WD_ASK_FOR_POOL_CONFIG, NULL);
					send_message(g_cluster.masterNode, pktAsk);
					free_packet(pktAsk);
				}
				else
				{
					ereport(NOTICE,
							(errmsg("our join coordinator is rejected by node \"%s\"",wdNode->nodeName),
							 errhint("rejoining the cluster.")));
					set_state(WD_JOINING);
				}
			}
		}
			break;
		case WD_EVENT_REMOTE_NODE_LOST:
		{
			ereport(LOG,
					(errmsg("life check reported \"%s\" is lost",wdNode->nodeName)));
			
			/*
			 * we have lost one remote connected node
			 * check if the node was coordinator
			 */
			if (g_cluster.masterNode == NULL)
				set_state(WD_JOINING);
			else
			{
				int quorum_status = get_quorum_status();
				if (quorum_status == -1)
				{
					ereport(LOG,
							(errmsg("We have lost the quorum after loosing \"%s\"",wdNode->nodeName)));
					
					/*
					 * We have lost the qurum, and left with no reason to
					 * continue as a coordinator node
					 */
					set_state(WD_WAITING_FOR_QUORUM);
				}
				else
					ereport(DEBUG1,
							(errmsg("We have lost the node \"%s\" but quorum still holds",wdNode->nodeName)));
			}
		}
			break;
		case WD_EVENT_PACKET_RCV:
			switch (pkt->type)
		{
			case WD_STAND_FOR_COORDINATOR_MESSAGE:
			{
				if (g_cluster.masterNode == NULL)
				{
					reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
					set_state(WD_PARTICIPATE_IN_ELECTION);
				}
				else
				{
					reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
					set_state(WD_JOINING);
				}
			}
				break;
			case WD_DECLARE_COORDINATOR_MESSAGE:
				if (wdNode != g_cluster.masterNode)
				{
					/*
					 * we already have a master node
					 * and we got a new node trying to be master
					 * re-initialize the cluster, something is wrong
					 */
					reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
					set_state(WD_JOINING);
				}
				break;
			default:
				standard_packet_processor(wdNode, pkt);
				break;
		}
			break;
		default:
			break;
	}
	return 0;
}

static int watchdog_state_machine_waiting_for_quorum(WD_EVENTS event, WatchdogNode* wdNode, WDPacketData* pkt)
{
	switch (event)
	{
		case WD_EVENT_WD_STATE_CHANGED:
			send_cluster_command(NULL, WD_QUORUM_IS_LOST, 10);
			break;
			
		case WD_EVENT_CON_OPEN:
			break;
			
		case WD_EVENT_TIMEOUT:
			break;
			
		case WD_EVENT_PACKET_RCV:
		{
			standard_packet_processor(wdNode, pkt);
			if (pkt->type == WD_ADD_NODE_MESSAGE)
				set_state(WD_JOINING);
		}
			break;
		case WD_EVENT_REMOTE_NODE_FOUND:
		{
			if (get_quorum_status() >= 0)
			{
				/* quorum is complete again, start cluster initializing */
				ereport(LOG,
						(errmsg("node \"%s\" is found, and quorum is complete again",wdNode->nodeName),
						 errdetail("initializing the cluster")));
				set_state(WD_JOINING);
			}
			break;
		}
			
		case WD_EVENT_LOCAL_NODE_FOUND:
			break;
			
		default:
			break;
	}
	return 0;
	
}
/*
 * The function identifies the current quorum state
 * return values:
 * -1:
 *     quorum is lost or does not exisits
 * 0:
 *     The quorum is on the edge. (when participating cluster is configured
 *     with even number of nodes, and we have exectly 50% nodes
 * 1:
 *     quorum exists
 */
static int get_quorum_status(void)
{
	if ( get_cluster_node_count() > get_mimimum_nodes_required_for_quorum())
		return 1;
	else if ( get_cluster_node_count() == get_mimimum_nodes_required_for_quorum())
	{
		if (g_cluster.remoteNodeCount % 2 != 0)
			return 0; /* on the edge */
		return 1;
	}
	return -1;
}

/* returns the minimum number of remote nodes required for quorum */
static int get_mimimum_nodes_required_for_quorum(void)
{
	/*
	 * Even numner of remote nodes, That means total number of nodes
	 * are odd, so minimum quorum is just remote/2
	 */
	if (g_cluster.remoteNodeCount % 2 == 0)
		return (g_cluster.remoteNodeCount / 2);
	
	/*
	 * Total nodes including self are even, So we consider 50%
	 * nodes as quorum, should we?
	 */
	return ((g_cluster.remoteNodeCount - 1 ) / 2);
}

static int set_state(WD_STATES newState)
{
	WD_STATES oldState = g_cluster.localNode->state;
	ereport(LOG,
			(errmsg("setting watchdog state old state = %s NEW = %s",debug_states[oldState],debug_states[newState])));
	g_cluster.localNode->state = newState;
	/* If we are resigning from being coordinator
	 * kill the escalation child process
	 */
	if (oldState == WD_COORDINATOR && newState != WD_COORDINATOR && g_cluster.escalation_pid > 0)
	{
		printf(" I am resigning from coordinator so killing pid =%d\n",g_cluster.escalation_pid);
		kill(g_cluster.escalation_pid,SIGTERM);
		g_cluster.escalation_pid = -1;
	}
	if (oldState != newState)
		watchdog_state_machine(WD_EVENT_WD_STATE_CHANGED, NULL, NULL);
	return 0;
}


static void allocate_resultNodes_in_IPCCommand(WDIPCCommandData* ipcCommand)
{
	MemoryContext oldCxt;
	int i;
	
	if (ipcCommand->nodeResults != NULL)
		return;
	
	oldCxt = MemoryContextSwitchTo(ipcCommand->memoryContext);
	ipcCommand->nodeResults = palloc0((sizeof(WDCommandNodeResult) * g_cluster.remoteNodeCount));
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		ipcCommand->nodeResults[i].wdNode = &g_cluster.remoteNodes[i];
	}
	MemoryContextSwitchTo(oldCxt);
}

static IPC_CMD_PREOCESS_RES execute_replicate_command(WDIPCCommandData* ipcCommand)
{
	int i;
	IPC_CMD_PREOCESS_RES res;
	
	WDPacketData wdPacket;
	init_wd_packet(&wdPacket);
	set_message_type(&wdPacket, WD_REPLICATE_VARIABLE_REQUEST);
	set_next_commandID_in_message(&wdPacket);
	set_message_data(&wdPacket,ipcCommand->data_buf, ipcCommand->data_len);
	
	allocate_resultNodes_in_IPCCommand(ipcCommand);
	ipcCommand->sendTo_count = 0;
	ipcCommand->reply_from_count = 0;
	ipcCommand->internal_command_id = wdPacket.command_id;
	ipcCommand->type = wdPacket.type;
	
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		WDCommandNodeResult* nodeResult = &ipcCommand->nodeResults[i];
		if (send_message_to_node(nodeResult->wdNode, &wdPacket) == true)
		{
			nodeResult->cmdState = COMMAND_STATE_SENT;
			ipcCommand->sendTo_count++;
		}
		else
			nodeResult->cmdState = COMMAND_STATE_SEND_ERROR;
	}
	
	/* Okay if we are not able to send to minimum nodes required for quorum
	 * we are already failed
	 */
	if (ipcCommand->sendTo_count == 0)
	{
		if (get_mimimum_nodes_required_for_quorum() == 0)
			res = IPC_CMD_COMPLETE;
		else
			res = IPC_CMD_ERROR;
	}
	else if (ipcCommand->sendTo_count < get_mimimum_nodes_required_for_quorum() )
		res = IPC_CMD_ERROR;
	else
		res = IPC_CMD_PROCESSING;
	
	return res;
}


static bool process_pgpool_replicate_command(WatchdogNode* wdNode, WDPacketData* pkt)
{
	/* we need to get the function name */
	json_value *root, *value;
	char* func_name;
	bool is_error = false;
	int node_count = 0;
	int *node_id_list = NULL;
	
	root = json_parse(pkt->data,pkt->len);
	
	/* The root node must be object */
	if (root == NULL || root->type != json_object)
	{
		json_value_free(root);
		ereport(NOTICE,
				(errmsg("unable to parse json data from replicate command")));
		return false;
	}
	func_name = json_get_string_value_for_key(root, "Function");
	if (func_name == NULL)
	{
		json_value_free(root);
		ereport(NOTICE,
				(errmsg("invalid json data"),
				 errdetail("unable to find Watchdog Function Name")));
		return false;
	}
	func_name = pstrdup(func_name);
	/* If it is a node function ?*/
	if (json_get_int_value_for_key(root, "NodeCount", &node_count))
	{
		json_value_free(root);
	}
	
	/* find the WatchdogNodes array */
	value = json_get_value_for_key(root,"NodeIdList");
	if (value == NULL)
	{
		ereport(ERROR,
				(errmsg("invalid json data"),
				 errdetail("unable to find NodeIdList node from data")));
	}
	if (value->type != json_array)
	{
		is_error = true;
		ereport(NOTICE,
				(errmsg("invalid json data"),
				 errdetail("NodeIdList node does not contains Array")));
	}
	if (node_count != value->u.array.length)
	{
		is_error = true;
		ereport(NOTICE,
				(errmsg("invalid json data"),
				 errdetail("NodeIdList array contains %d nodes while expecting %d",value->u.array.length, node_count)));
	}
	
	if (is_error == false)
	{
		int i;
		node_id_list = palloc(sizeof(int) * node_count);
		for (i = 0; i < node_count; i++)
		{
			node_id_list[i] = value->u.array.values[i]->u.integer;
		}
		if (is_error)
		{
			pfree(node_id_list);
			node_id_list = NULL;
			node_count = 0;
		}
	}
	int k;
	json_value_free(root);
	printf("***** NEW WD COMMAND *****\n FUNCTION = \"%s\"\nNode Count = %d\n",func_name, node_count);
	for (k =0; k< node_count; k++)
		printf("NODE ID [%d] = %d\n",k,node_id_list[k]);
	printf("\n");
	return process_wd_command_function(wdNode, pkt, func_name, node_count, node_id_list);
}

static bool process_wd_command_function(WatchdogNode* wdNode, WDPacketData* pkt, char* func_name, int node_count, int* node_id_list)
{
	if (strcasecmp(WD_FUNCTION_START_RECOVERY, func_name) == 0)
	{
		if (*InRecovery != RECOVERY_INIT)
		{
			reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
		}
		else
		{
			*InRecovery = RECOVERY_ONLINE;
			if (Req_info->conn_counter == 0)
			{
				reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
			}
			else if(pool_config->recovery_timeout <= 0)
			{
				reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
			}
			else
			{
				WDFunctionCommandData* wd_func_command;
				MemoryContext oldCxt = MemoryContextSwitchTo(TopMemoryContext);
				
				wd_func_command = palloc(sizeof(WDFunctionCommandData));
				wd_func_command->commandType = pkt->type;
				wd_func_command->commandID = pkt->command_id;
				wd_func_command->funcName = MemoryContextStrdup(TopMemoryContext,func_name);
				wd_func_command->wdNode = wdNode;
				
				/* Add this command for timer tick */
				add_wd_command_for_timer_events(pool_config->recovery_timeout, true, wd_func_command);
				
				MemoryContextSwitchTo(oldCxt);
				
			}
		}
	}
	else if (strcasecmp(WD_FUNCTION_END_RECOVERY, func_name) == 0)
	{
		*InRecovery = RECOVERY_INIT;
		reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
		kill(getppid(), SIGUSR2);
	}
	
	else if (strcasecmp(WD_FUNCTION_FAILBACK_REQUEST, func_name) == 0)
	{
		if (Req_info->switching)
		{
			ereport(LOG,
					(errmsg("sending watchdog response"),
					 errdetail("failover request from other pgpool is canceled because of switching")));
			reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
		}
		else
		{
			reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
			wd_set_node_mask_for_failback_req(node_id_list, node_count);
			send_failback_request(node_id_list[0],false);
		}
	}
	
	else if (strcasecmp(WD_FUNCTION_DEGENERATE_REQUEST, func_name) == 0)
	{
		if (Req_info->switching)
		{
			ereport(LOG,
					(errmsg("sending watchdog response"),
					 errdetail("failover request from other pgpool is canceled because of switching")));
			reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
		}
		else
		{
			reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
			wd_set_node_mask_for_degenerate_req(node_id_list, node_count);
		}
	}
	
	else if (strcasecmp(WD_FUNCTION_PROMOTE_REQUEST, func_name) == 0)
	{
		if (Req_info->switching)
		{
			ereport(LOG,
					(errmsg("sending watchdog response"),
					 errdetail("failover request from other pgpool is canceled because of switching")));
			reply_with_minimal_message(wdNode, WD_REJECT_MESSAGE, pkt);
		}
		else
		{
			reply_with_minimal_message(wdNode, WD_ACCEPT_MESSAGE, pkt);
			wd_set_node_mask_for_promote_req(node_id_list, node_count);
			promote_backend(node_id_list[0]);
		}
	}
	
	else if (strcasecmp("TEST_SYSTEM", func_name) == 0)
	{
		printf("&&&&&&_____[%d] PROCESSING TEST_SYSTEM COMMAND\n",__LINE__);
		WDFunctionCommandData* wd_func_command;
		MemoryContext oldCxt = MemoryContextSwitchTo(TopMemoryContext);
		
		wd_func_command = palloc(sizeof(WDFunctionCommandData));
		wd_func_command->commandType = pkt->type;
		wd_func_command->commandID = pkt->command_id;
		wd_func_command->funcName = MemoryContextStrdup(TopMemoryContext,func_name);
		wd_func_command->wdNode = wdNode;
		
		/* Add this command for timer tick */
		add_wd_command_for_timer_events(10, true, wd_func_command);
		
		MemoryContextSwitchTo(oldCxt);
	}
	else
	{
		/* This is not supported function */
		reply_with_minimal_message(wdNode, WD_ERROR_MESSAGE, pkt);
	}
	return true;
}


static bool reply_is_received_for_pgpool_replicate_command(WatchdogNode* wdNode, WDPacketData* pkt, WDIPCCommandData* ipcCommand)
{
	int i;
	WDCommandNodeResult* nodeResult = NULL;
	/* get the result node for */
	printf("----*****----- [%d] WE HAVE RECEIVED REPLY FOR REPLICATE COMMAND WE ISSUED\n",__LINE__);
	for (i=0; i< g_cluster.remoteNodeCount; i++)
	{
		nodeResult = &ipcCommand->nodeResults[i];
		if (nodeResult->wdNode == wdNode)
			break;
		nodeResult = NULL;
	}
	if (nodeResult == NULL)
	{
		ereport(NOTICE,(errmsg("unable to find node result")));
		return true;
	}
	nodeResult->result_type = pkt->type;
	nodeResult->cmdState = COMMAND_STATE_REPLIED;
	ipcCommand->reply_from_count++;
	
	printf("----*****----- [%d] reply_from_count = %d AND sendTo_count = %d\n",__LINE__,ipcCommand->reply_from_count,ipcCommand->sendTo_count);
	
	if (ipcCommand->reply_from_count >= ipcCommand->sendTo_count)
	{
		/*
		 * we have received results from all nodes
		 * analyze the result
		 */
		
		int res_len = 0;
		char res_type = WD_IPC_CMD_RESULT_OK;
		
		for (i=0; i< g_cluster.remoteNodeCount; i++)
		{
			nodeResult = &ipcCommand->nodeResults[i];
			if (nodeResult->cmdState == COMMAND_STATE_REPLIED &&
				nodeResult->result_type != WD_ACCEPT_MESSAGE)
			{
				res_type = WD_IPC_CMD_RESULT_BAD;
				break;
			}
		}
		printf("----*****----- [%d] replying back to IPC SOCKET\n",__LINE__);
		
		write(ipcCommand->issueing_sock, &res_type, 1);
		write(ipcCommand->issueing_sock, &res_len, 4);
		/* ok we are done, delete this command
		 */
		cleanUpIPCCommand(ipcCommand);
	}
	
	return true; /* do not process this packet further */
}

/*
 * return true if want to cancel timer,
 */
static bool process_wd_command_timer_event(bool timer_expired, WDFunctionCommandData* wd_func_command)
{
	if (wd_func_command->commandType == WD_REPLICATE_VARIABLE_REQUEST)
	{
		if (wd_func_command->funcName && strcasecmp("START_RECOVERY", wd_func_command->funcName) == 0)
		{
			if (Req_info->conn_counter == 0)
			{
				WDPacketData emptyPkt;
				emptyPkt.command_id = wd_func_command->commandID;
				reply_with_minimal_message(wd_func_command->wdNode, WD_ACCEPT_MESSAGE, &emptyPkt);
				// TODO delete command object
				return true;
			}
			else if (timer_expired)
			{
				WDPacketData emptyPkt;
				emptyPkt.command_id = wd_func_command->commandID;
				reply_with_minimal_message(wd_func_command->wdNode, WD_REJECT_MESSAGE, &emptyPkt);
				return true;
			}
			return false;
		}
		
		if (wd_func_command->funcName && strcasecmp("TEST_SYSTEM", wd_func_command->funcName) == 0)
		{
			if (timer_expired)
			{
				printf("****%s:%d Timer Expired TEST_STSTEM function, Sending back accept message\n",__FUNCTION__,__LINE__);
				
				WDPacketData emptyPkt;
				emptyPkt.command_id = wd_func_command->commandID;
				reply_with_minimal_message(wd_func_command->wdNode, WD_ACCEPT_MESSAGE, &emptyPkt);
				return true;
			}
			else
				printf("****%s:%d Timer tick called on TEST_STSTEM function\n",__FUNCTION__,__LINE__);
			return false;
		}
		
	}
	/* Just remove the timer.*/
	return true;
}

static void process_wd_func_commands_for_timer_events(void)
{
	struct timeval currTime;
	ListCell *lc;
	List* timers_to_del = NIL;
	
	gettimeofday(&currTime, NULL);
	
	if (g_cluster.wd_timer_commands != NULL)
		printf("****%s:%d \n",__FUNCTION__,__LINE__);
	
	foreach(lc, g_cluster.wd_timer_commands)
	{
		WDCommandTimerData* timerData = lfirst(lc);
		if (timerData)
		{
			bool del = false;
			if (WD_TIME_DIFF_SEC(currTime,timerData->startTime) >=  timerData->expire_sec)
			{
				del = process_wd_command_timer_event(true, timerData->wd_func_command);
				
			}
			else if (timerData->need_tics)
			{
				del = process_wd_command_timer_event(false, timerData->wd_func_command);
			}
			if (del)
				timers_to_del = lappend(timers_to_del,timerData);
		}
	}
	foreach(lc, timers_to_del)
	{
		g_cluster.wd_timer_commands = list_delete_ptr(g_cluster.wd_timer_commands,lfirst(lc));
	}
}

static void add_wd_command_for_timer_events(unsigned int expire_secs, bool need_tics, WDFunctionCommandData* wd_func_command)
{
	/* create a new Timer struct */
	MemoryContext oldCtx = MemoryContextSwitchTo(TopMemoryContext);
	WDCommandTimerData* timerData = palloc(sizeof(WDCommandTimerData));
	gettimeofday(&timerData->startTime,NULL);
	timerData->expire_sec = expire_secs;
	timerData->need_tics = need_tics;
	timerData->wd_func_command = wd_func_command;
	
	g_cluster.wd_timer_commands = lappend(g_cluster.wd_timer_commands,timerData);
	
	MemoryContextSwitchTo(oldCtx);
	
}

static bool verify_pool_configurations(POOL_CONFIG* config)
{
	int i;
	char *key = "";
	if (config->num_init_children != pool_config->num_init_children)
	{
		key = "num_init_children";
		goto ERROR_EXIT;
	}
	if (config->listen_backlog_multiplier != pool_config->listen_backlog_multiplier)
	{
		key = "listen_backlog_multiplier";
		goto ERROR_EXIT;
	}
	if (config->child_life_time != pool_config->child_life_time)
	{
		key = "child_life_time";
		goto ERROR_EXIT;
	}
	if (config->connection_life_time != pool_config->connection_life_time)
	{
		key = "connection_life_time";
		goto ERROR_EXIT;
	}
	if (config->child_max_connections != pool_config->child_max_connections)
	{
		key = "child_max_connections";
		goto ERROR_EXIT;
	}
	if (config->client_idle_limit != pool_config->client_idle_limit)
	{
		key = "client_idle_limit";
		goto ERROR_EXIT;
	}
	if (config->max_pool != pool_config->max_pool)
	{
		key = "max_pool";
		goto ERROR_EXIT;
	}
	if (config->replication_mode != pool_config->replication_mode)
	{
		key = "replication_mode";
		goto ERROR_EXIT;
	}
	if (config->enable_pool_hba != pool_config->enable_pool_hba){key = "enable_pool_hba";goto ERROR_EXIT;}
	if (config->load_balance_mode != pool_config->load_balance_mode){key = "load_balance_mode";goto ERROR_EXIT;}
	if (config->replication_stop_on_mismatch != pool_config->replication_stop_on_mismatch){key = "replication_stop_on_mismatch";goto ERROR_EXIT;}
	if (config->failover_if_affected_tuples_mismatch != pool_config->failover_if_affected_tuples_mismatch){key = "failover_if_affected_tuples_mismatch";goto ERROR_EXIT;}
	if (config->replicate_select != pool_config->replicate_select){key = "replicate_select";goto ERROR_EXIT;}
	if (config->master_slave_mode != pool_config->master_slave_mode){key = "master_slave_mode";goto ERROR_EXIT;}
	if (config->connection_cache != pool_config->connection_cache){key = "connection_cache";goto ERROR_EXIT;}
	if (config->health_check_timeout != pool_config->health_check_timeout){key = "health_check_timeout";goto ERROR_EXIT;}
	if (config->health_check_period != pool_config->health_check_period){key = "health_check_period";goto ERROR_EXIT;}
	if (config->health_check_max_retries != pool_config->health_check_max_retries){key = "health_check_max_retries";goto ERROR_EXIT;}
	
	if (config->health_check_retry_delay != pool_config->health_check_retry_delay){key = "health_check_retry_delay";goto ERROR_EXIT;}
	if (config->fail_over_on_backend_error != pool_config->fail_over_on_backend_error){key = "fail_over_on_backend_error";goto ERROR_EXIT;}
	if (config->recovery_timeout != pool_config->recovery_timeout){key = "recovery_timeout";goto ERROR_EXIT;}
	if (config->search_primary_node_timeout != pool_config->search_primary_node_timeout){key = "search_primary_node_timeout";goto ERROR_EXIT;}
	if (config->client_idle_limit_in_recovery != pool_config->client_idle_limit_in_recovery){key = "client_idle_limit_in_recovery";goto ERROR_EXIT;}
	if (config->insert_lock != pool_config->insert_lock){key = "insert_lock";goto ERROR_EXIT;}
	
	if (config->parallel_mode != pool_config->parallel_mode){key = "parallel_mode";goto ERROR_EXIT;}
	if (config->memory_cache_enabled != pool_config->memory_cache_enabled){key = "memory_cache_enabled";goto ERROR_EXIT;}
	if (config->use_watchdog != pool_config->use_watchdog){key = "use_watchdog";goto ERROR_EXIT;}
	if (config->clear_memqcache_on_escalation != pool_config->clear_memqcache_on_escalation){key = "clear_memqcache_on_escalation";goto ERROR_EXIT;}
	
	if (config->backend_desc->num_backends != pool_config->backend_desc->num_backends)
	{
		ereport(FATAL,
				(return_code(POOL_EXIT_FATAL),
				 errmsg("configuration error. The configurations on master node is different"),
				 errdetail("pgpool on master node \"%s\" is configured with %d backends while this node has %d backends configured",
						   g_cluster.masterNode->nodeName,
						   config->backend_desc->num_backends,
						   pool_config->backend_desc->num_backends)));
		return false;
	}
	
	for (i=0; i < pool_config->backend_desc->num_backends; i++)
	{
		if (strncasecmp(pool_config->backend_desc->backend_info[i].backend_hostname, config->backend_desc->backend_info[i].backend_hostname, sizeof(pool_config->backend_desc->backend_info[i].backend_hostname)))
		{
			ereport(FATAL,
					(return_code(POOL_EXIT_FATAL),
					 errmsg("configuration error. The configurations on master node is different"),
					 errdetail("pgpool on master node \"%s\" backend[%d] hostname \"%s\" is different from \"%s\" on this node",
							   g_cluster.masterNode->nodeName,
							   i,
							   config->backend_desc->backend_info[i].backend_hostname,
							   pool_config->backend_desc->backend_info[i].backend_hostname)));
			return false;
		}
		if (pool_config->backend_desc->backend_info[i].backend_port != pool_config->backend_desc->backend_info[i].backend_port)
		{
			ereport(FATAL,
					(return_code(POOL_EXIT_FATAL),
					 errmsg("configuration error. The configurations on master node is different"),
					 errdetail("pgpool on master node \"%s\" backend[%d] port \"%d\" is different from \"%d\" on this node",
							   g_cluster.masterNode->nodeName,
							   i,
							   config->backend_desc->backend_info[i].backend_port,
							   pool_config->backend_desc->backend_info[i].backend_port)));
			return false;
		}
	}
	
	if (config->wd_remote_nodes.num_wd != pool_config->wd_remote_nodes.num_wd)
	{
		ereport(FATAL,
				(return_code(POOL_EXIT_FATAL),
				 errmsg("configuration error. The configurations on master node is different"),
				 errdetail("pgpool on master node \"%s\" is configured with %d watchdog nodes while this node has %d nodes configured",
						   g_cluster.masterNode->nodeName,
						   config->wd_remote_nodes.num_wd,
						   pool_config->wd_remote_nodes.num_wd)));
		return false;
	}


	return true;
ERROR_EXIT:
	ereport(FATAL,
			(return_code(POOL_EXIT_FATAL),
			 errmsg("configuration error. The configurations on master node is different"),
			 errdetail("value for key \"%s\" differs",key)));
	
	return false;
}

static bool get_authhash_for_node(WatchdogNode* wdNode, char* authhash)
{
	if (strlen(pool_config->wd_authkey))
	{
		char nodeStr[WD_MAX_PACKET_STRING + 1];
		int len = snprintf(nodeStr, WD_MAX_PACKET_STRING, "state=%d tv_sec=%ld wd_port=%d",
					   wdNode->state, wdNode->tv.tv_sec, wdNode->wd_port);
		
		
		/* calculate hash from packet */
		wd_calc_hash(nodeStr, len, authhash);
		return true;
	}
	return false;
}

static bool verify_authhash_for_node(WatchdogNode* wdNode, char* authhash)
{
	if (strlen(pool_config->wd_authkey))
	{
		char calculated_authhash[MD5_PASSWD_LEN +1];

		char nodeStr[WD_MAX_PACKET_STRING];
		int len = snprintf(nodeStr, WD_MAX_PACKET_STRING, "state=%d tv_sec=%ld wd_port=%d",
						   wdNode->state, wdNode->tv.tv_sec, wdNode->wd_port);
		
		
		/* calculate hash from packet */
		wd_calc_hash(nodeStr, len, calculated_authhash);
		return (strcmp(calculated_authhash,authhash) == 0);
	}
	/* authkey is not enabled.*/
	return true;
}

/* DEBUG */
static void print_watchdog_node_info(WatchdogNode* wdNode)
{
	printf("********\t STATE    = %s\n",debug_states[wdNode->state]);
	printf("********\t HostName = %s\n",wdNode->hostname);
	printf("********\t NodeName = %s\n",wdNode->nodeName);
	printf("********\t WDPort   = %d\n",wdNode->wd_port);
	printf("********\t pgp port = %d\n",wdNode->pgpool_port);
	printf("********\t Priority = %d\n",wdNode->wd_priority);
}

static void print_packet_info(WDPacketData* pkt,WatchdogNode* wdNode)
{
	int i;
	packet_types *pkt_type = NULL;
	for (i =0; ; i++)
	{
		if (all_packet_types[i].type == WD_NO_MESSAGE)
			break;
		
		if (all_packet_types[i].type == pkt->type)
		{
			pkt_type = &all_packet_types[i];
			break;
		}
	}
	printf("\n******Packet [ID:%d]received of type [%s] from \"%s\" My state = (%s) \n",pkt->command_id, pkt_type?pkt_type->name:"UNKNOWN",
		   wdNode->nodeName, debug_states[get_local_node_state()]);
}
