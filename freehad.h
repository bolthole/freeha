
/* message block definition */

#define HA_VERSION 2
#define HA_HOSTLEN 32
#define MAX_HA_HOSTS 16


/* states of an HA node. 
 * Ideally, should indicate the state of a named service.
 * But since we only support a single servicetype across the entire 'cluster',
 * state-of-service==state-of-node
 */
enum { UNKNOWNSTATE, INITIAL, RUNNING, STOPPING, STARTING, STANDBY, ERRORED, TIMEDOUT, ENDLIST };

struct freeha_msg {
	int version; /* freeha protocol rev == HA_VERSION */
	char srchost[HA_HOSTLEN];
	int status;
	time_t timestamp; /* filled in by receiving host, not sending host */
	                  /* used to determine stale/old data */
};


