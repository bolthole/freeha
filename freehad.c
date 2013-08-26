/* actual workhorse demon for "FreeHA".
 *
 * This program should handle all of the following:
 *
 * -> send status 'heartbeats' to all configured networks
 *
 * -> listen for 'heartbeats' from other nodes
 *
 * -> call 'starthasrv' if it has been determined that this node
 *    should become "active"
 *
 * -> call 'monitorhasrv'  periodically to see if there are errors
 *
 * -> call 'alerthasrv' if an error has been detected.
 *
 * -> call 'stophasrv' if an error has been detected.
 *     (It is up to the sysadmin to make sure 'stophasrv' gets
 *       called when the machine shuts down 'normally')
 *
 * -> stay in 'errored' state once entered, until killed
 *
 */

static char *versionstring="@(#) freehad.c 1.36@(#)";

#include <stdio.h>
#include <unistd.h>
#include <sys/param.h> /* for MAXPATHLEN */
#include <stdlib.h>
#include <errno.h>
#include <time.h>

#if defined(__svr4__) || defined(__linux__)
# include <string.h>
# define bcopy(src,dest,sz) memcpy(dest,src,sz)
# define bzero(dest,sz) memset(dest,0,sz)
#else
# include <strings.h>
#endif /* __svr4__ */

#include <ctype.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef USE_SIGNAL
#include <signal.h>
#endif

#ifdef USE_SYSLOG
#include <syslog.h>
#endif

#include <sys/wait.h> /* should be right on solaris, linux, AND freebsd */

#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#ifndef FIONBIO
#include <sys/filio.h> /* WHY do I need this specifically, when it is
                          included in sys/ioctl.h ? ? ? */
#endif

#include "freehad.h"

/**********************************************************************/
/**********************************************************************/

/* for the heartbeat addrs/nets */
struct in_addr ipaddr[3]; /* for listening */
struct in_addr netaddr[3];/* for sending (broadcast)*/

int listensock[3], sendsock[3];

int freeha_port=0xf33;

char state_file[1024]; /* path to 'state file', that we write */
int state_fd;

struct statelist{
	struct freeha_msg msg;
	struct statelist *next,*prev;
};

/* list of every system we know about */
struct statelist *host_states;

char myhostname[100];
char scriptdir[MAXPATHLEN];

int mystate=INITIAL;	/* the 'HA status' of this node */
int am_main=0;		/* Am I the main node? dont think sooo... */

int num_nodes=2;	/* number of machines in cluster. used for
			 * 'quorum' purposes.
			 * (avoiding split-brain, if more than 2 nodes)
			 */
int quorum_count;	/* filled in by main() */

int heartbeatsec=1;	/* not used currently */
int monitorsec=1;	/* seconds delay between monitor/hb loops */
int timeoutsec=121;	/* seconds after which a node with no heartbeat is
                         * assumed 'Missing In Action' (ie 'dead')
			 */

int need_to_stop=0;	/* flag for main loop to call stop_services() */
int need_to_start=0;	/* flag for main loop to call start_services() */
int need_to_quit=0;	/* If received SIGINT, quit cleanly */
int have_error=0;

/**********************************************************************/

/* I want to keep this SIMPLE, by using getopt()
 * Unfortunately, getopt does not support more complex arg strings, but
 * only single-letter options.
 * Hence why the options are a leetle bit unwieldy
 */
void usage(){
	puts("Usage:");
	puts(" (-a|-b|-c) {IPaddr}   IP addr 1,2 or 3, for src addr");
	puts(" (-A|-B|-C) {addr}     BROADCAST addr 1,2 or 3 for dest broadcast");
	puts(" -h                    help (This text)");
	puts(" -l {statefile}        file to write ongoing state to");
	puts(" -n {nodes}            number of nodes in cluster (default 2)");
	puts(" -p {port}             UDP port for heartbeat(default 0xfee)");
	puts(" -s directory          script directory (default /opt/freeha/bin)");
	puts(" -M {sec}              number of seconds between monitorings(defalt 1)");
	puts(" -T {timeoutsec}       number of seconds to timeout a node(dead)");
	printf("                          (default=%d)\n",timeoutsec);
	puts("");
	puts("Note that -M specifies 'delay beween', not 'do every'");
	puts("");
	printf("Code Release: %s\n",versionstring);
}


#ifdef USE_SIGNAL
void sighandler(int sigarg){
	switch(sigarg){
	    case SIGINT:
	    case SIGTERM:
		need_to_quit=1;
	    case SIGHUP:
		need_to_stop=1;
		break;
	    case SIGUSR1:
		/* This is a FORCED (re)-start. So we clear error flags */
		have_error=0;
		need_to_start=1;
		break;
	    case SIGUSR2:
		/* Just clear the error flag. Dont actually DO anything */
		have_error=0;
		if(mystate==ERRORED){
			mystate=STANDBY;
		}
		break;
	}
}
#endif /* USE_SIGNAL */

void init_state_file(char *statefile_name)
{
	if (statefile_name != NULL){
		strncpy(state_file,statefile_name,1024);
	} else {
		if(access("/var/run", X_OK|W_OK)==0) {
			strcpy(state_file,"/var/run/freeha");
		} else if(access("/var/freeha", X_OK|W_OK)==0){
			strcpy(state_file,"/var/freeha/state");
		} else {
			fprintf(stderr,"ERROR: Do not know where to create state file\n");
			fprintf(stderr,"/var/run and /var/freeha are not accessible\n");
			fprintf(stderr,"Use -l to specify location of state file\n");
		
			exit(1);
		}
	}

	/* this is just to ensure that the creat() call wont fail with EEXIST.
	 * we dont care if there isnt an old statefile there already
	 */
	unlink(state_file);

	state_fd=creat(state_file, S_IRWXU|S_IRGRP|S_IROTH);
	if(state_fd <0)	{
		perror("could not create state file");
		exit(1);
	}
}

/* called as subroutine for opensockets() */
int opensock(struct sockaddr_in *addr_ptr)
{
	int reuse_opt=1;
	int newsock=socket(AF_INET,SOCK_DGRAM,0);
	if(newsock<0){
		perror("could not create new socket?? !!");
		return -1;
	}
	if(bind(newsock, (struct sockaddr*)addr_ptr,
	        sizeof(struct sockaddr_in)) <0)
	{
		printf("Error binding %s:%d\n",inet_ntoa(addr_ptr->sin_addr),
		        freeha_port);
		return -1;
	}
	setsockopt(newsock, SOL_SOCKET,SO_REUSEADDR,
		   &reuse_opt, sizeof(reuse_opt));

	/* Okay, only half the sockets we make, need this option set.
	 * But no harm in setting it, as far as I can see!
	 */
	setsockopt(newsock, SOL_SOCKET,SO_BROADCAST,
		   &reuse_opt, sizeof(reuse_opt));

/* I would think that NONBLOCK would work.
 * Except that it doesnt seem to work for solaris. GRRR.
 * So thats why FIONBIO is here also
 */
#ifdef FIONBIO
	ioctl(newsock,FIONBIO, &reuse_opt);
#else
	fcntl(newsock,F_SETFL, O_NONBLOCK);
#endif

	return newsock;

}


/* Open the sending sockets, and the listening sockets */
void opensockets()
{
	struct sockaddr_in addr;
	int loop;
	int addrcount=0;

	bzero(&addr, sizeof(struct sockaddr_in));

	addr.sin_family=AF_INET;
	addr.sin_port=htons(freeha_port);


	for(loop=0; loop<3; loop++) {
		addr.sin_addr.s_addr = ipaddr[loop].s_addr;
		if(addr.sin_addr.s_addr == INADDR_ANY)  continue;

		printf("Trying to bind for send %s:%d\n",
			         inet_ntoa(addr.sin_addr),
			         freeha_port);

		sendsock[loop]=opensock(&addr);
		if(sendsock[loop] >=0){
			addrcount++;
		}

		addr.sin_addr.s_addr = netaddr[loop].s_addr;
		if(addr.sin_addr.s_addr == INADDR_ANY){
			netaddr[loop].s_addr = ipaddr[loop].s_addr;
			/* well durnit. try to GUESS at broadcast,
			 * since unspecified.
			 * assume they want class C broadcast.
			 */
			netaddr[loop].s_addr = htonl(netaddr[loop].s_addr);
			/* We KNOW it is in MSB format now */
			netaddr[loop].s_addr |= 0x000000ff;

			netaddr[loop].s_addr = ntohl(netaddr[loop].s_addr);
			addr.sin_addr.s_addr = netaddr[loop].s_addr;

			printf("Have adjusted broadcast to %s\n",
			        inet_ntoa(addr.sin_addr));
		}

		printf("Trying to bind LISTEN for %s:%d\n",
			         inet_ntoa(addr.sin_addr),
			         freeha_port);
		listensock[loop]=opensock(&addr);
		if(listensock[loop] <0){
			perror("ERROR trying to bind listen socket");
			fprintf(stderr,"failed for %s:%d\n",
			         inet_ntoa(addr.sin_addr),
			         freeha_port);
			exit(1);
		}

	}

	if(addrcount==0){
		fprintf(stderr,"ERROR: could not bind any addresses\n");
		exit(1);
	}
	
}


/* This wrapper exists to call the external alerthasrv script (if it exists)
 * when we change our internal state.
 * It first changes the global state variable, 'mystate'
 * It then calls the external script, with:
 *   alerthasrv [statenum] [statename]
 */
 
void changestate(int newstate)
{
	static int oldstate=UNKNOWNSTATE;
	char *statestring;
	char runstring[50];

	if(newstate==oldstate){
		return;
	}
	oldstate=newstate;
	mystate=newstate;

	/* Using switch statement instead of array, for error checking */
	switch(newstate){
		case UNKNOWNSTATE: statestring="UNKNOWN_STATE"; break;
		case INITIAL: statestring="INITIAL"; break;
		case RUNNING: statestring="RUNNING"; break;
		case STOPPING: statestring="STOPPING"; break;
		case STARTING: statestring="STARTING"; break;
		case STANDBY: statestring="STANDBY"; break;
		case ERRORED: statestring="ERRORED"; break;
		case TIMEDOUT: statestring="TIMEDOUT"; break;
		default: statestring="INTERNAL_ERROR"; break;
	}
	sprintf(runstring,"%s/alerthasrv %d %s",scriptdir, newstate,
	        statestring);
	system(runstring);
	/* technically this could fail, but... oh well */
	
	
}

/* Called only by storemsg().
 * We have a state message from a machine we dont recognize.
 * In this case, we automatically add that host's state into our list of
 * host states
 */
void addhost(struct freeha_msg* msg)
{
	struct statelist *newentry,*oldentry;
	int compare=0;

	newentry=(struct statelist*)malloc(sizeof(struct statelist));
	if(newentry==NULL){
		changestate(ERRORED);
		fprintf(stderr,"ERROR: addhost out of memory!\n");
		return;
	}

	newentry->next=newentry->prev=NULL;

	bcopy(msg, &newentry->msg, sizeof(struct freeha_msg));
	if(host_states==NULL){
		host_states=newentry;
		return;
	}
	oldentry=host_states;

	/*We need to alphabetize insert! So compare...*/
	do {
		compare=strcasecmp(oldentry->msg.srchost,
		        newentry->msg.srchost);
		       
		if(compare >0){
			newentry->next=oldentry;
			newentry->prev=oldentry->prev;
			if(oldentry->prev==NULL){
				host_states=newentry;
			} else {
				oldentry->prev->next=newentry;
			}
			oldentry->prev=newentry;
			return;
		}
		if(oldentry->next==NULL){
			break;
		}  else {
			oldentry=oldentry->next;
		}
	} while(1);

	/* oh well. slap on the end, then. */
	oldentry->next=newentry;
	newentry->prev=oldentry;

}

/* Once we have received a status (heartbeat) message from another system,
 * store it in our state table.
 * If system is not already present, then add it.
 */
void storemsg(struct freeha_msg* msg)
{
	int version;
	struct statelist *hoststate=host_states;
	version=ntohl(msg->version);
	if(version != HA_VERSION) {
		printf("ERROR: storemsg got msg version=%d; expecting %d\n",
		       version, HA_VERSION);
		return;
	}

	msg->timestamp=time(NULL);

	while(hoststate !=NULL){
		if(strncmp(hoststate->msg.srchost, msg->srchost, HA_HOSTLEN)==0){
			bcopy(msg, &hoststate->msg, sizeof(struct freeha_msg));
			return;
		}
		hoststate=hoststate->next;
	}
	/* HOST not found. so auto-add to our state list. */
	addhost(msg);
}

/* dump the states of all hosts we know about.
 * we dump this to the state file. So the state file should ideally be
 * on a tmpfs filesystem.
 * /var/run usually fulfills this purpose well
 */
void dump_states()
{
	char hoststate[50], *statestring;
	struct statelist *stateptr=host_states;
	int state;

	/* 'rewind' the file */
	lseek(state_fd, 0, SEEK_SET);

#ifdef DEBUG
	puts("Writing to state file:");
#endif
	while(stateptr !=NULL){
		state=ntohl(stateptr->msg.status);
		switch(state){
		    case INITIAL:
			statestring="Initial";
			break;
		    case RUNNING:
			statestring="Running";
			break;
		    case STOPPING:
			statestring="Stopping";
			break;
		    case STARTING:
			statestring="Starting"; /* starting services */
			break;
		    case STANDBY:
			statestring="Standby";
			break;
		    case ERRORED:
			statestring="ERRORED";
			break;
		    case TIMEDOUT:
			statestring="TIMEOUT";
			break;
		    default:
			statestring="[invalid state]";
			break;
		}
		sprintf(hoststate,"%.32s [%s] %s",
		        stateptr->msg.srchost, statestring,
			ctime(&(stateptr->msg.timestamp)) );

		/* remember, ctime adds \n */

		write(state_fd,hoststate,strlen(hoststate));
#ifdef DEBUG
		printf(" %s",hoststate);
#endif

		stateptr=stateptr->next;
	}
}

/* used to see if we have enough active nodes in cluster to
 * constitute "quorum". We want to avoid mini-clusters occuring
 * in our cluster machines.
 * Mainly used on initial demon startup only.
 */
int count_activenodes()
{
	struct statelist *stateptr=host_states;
	int activecount=0;


	while(stateptr !=NULL){
		int state=ntohl(stateptr->msg.status);
		switch(state){
		    case RUNNING:
		    case STANDBY:
		    case INITIAL:
		    case STOPPING:
		    case STARTING:
		    case ERRORED:
			activecount+=1;
		}
		stateptr=stateptr->next;
	}
	return activecount;
}

/* Determine if services are either running, or in process
 * of shutting down, SOMEWHERE in the cluster
 * return 1 if yes, 0 if no.
 */
int check_running()
{
	struct statelist *stateptr=host_states;
	int services_up=0;

	while(stateptr !=NULL){
		int state=ntohl(stateptr->msg.status);
		switch(state){
		    case RUNNING:
		    case STARTING:
		    case STOPPING:
			services_up=1;
		}
		stateptr=stateptr->next;
	}

	return services_up;
}

void sendheartbeat(){
	struct freeha_msg message;
	int msglen=sizeof(message);
	int loop;
	struct sockaddr_in addr;

	message.version=htonl(HA_VERSION);
	message.status=htonl(mystate);
	message.timestamp=0;
	strcpy(message.srchost, myhostname);

	addr.sin_family=AF_INET;
	addr.sin_port=htons(freeha_port);

	for(loop=0; loop<3; loop++) {
		if(sendsock[loop]==-1) continue;

		addr.sin_addr.s_addr = netaddr[loop].s_addr;
		if(sendto(sendsock[loop], &message, msglen,
		       NULL, (struct sockaddr*)&addr,
		       sizeof(struct sockaddr_in)) != msglen)
		{
			perror("heartbeat send failed");
		}
	}
}


/* return 1 on successfull read, 0 on none, -1 on error */
int readheartbeat(int socket)
{
	struct freeha_msg incoming;
	int readstat;

	if(socket<0) return 0;
	
	/* Note that this would get very unhappy if we somehow read less
	 * than a full heatbeat.
	 * At some point, will use NREADY, etc.
	 */

	readstat=recvfrom(socket, &incoming,sizeof(incoming),
	         NULL, NULL,0);

	if(readstat < (int)sizeof(incoming)){
		switch(errno){
#ifdef EWOULDBLOCK
		    case EWOULDBLOCK:
#else
		    default:
#endif
			return 0;
		}
		perror("readheartbeat: Error reading socket!");
		return -1;
	}

	storemsg(&incoming);
	return 1;
}

/* Kinda ugly, but...
 * basically, read in all network traffic for port that is queued,
 * for each interface/addr that we know about.
 * (Just in case we have fallen behind in our processing.
 *  Which is quite likely, with long monitor scripts)
 */
void readheartbeats()
{
	while(readheartbeat(listensock[0])==1);
	while(readheartbeat(listensock[1])==1);
	while(readheartbeat(listensock[2])==1);
}


/* This is the wrapper around the 'starthasrv' script.
 * It calls the script, checks the status of it, and
 * then sets mystate to either RUNNING, or ERRORED, as appropriate.
 */
void start_services()
{
	char startscript[MAXPATHLEN+20];
	int startstate;

	need_to_start=0;
	if(mystate == RUNNING){
		puts("ERROR: start_services called when already RUNNING");
		return;
	}


#ifdef USE_SYSLOG
	syslog(LOG_NOTICE,"starting services");
#else
	puts("DEBUG: start_services() calling start script");
#endif

	sprintf(startscript,"%s/%s",scriptdir, "starthasrv");
	startstate=system(startscript);
	if(startstate==0) {
		changestate(RUNNING);
#ifdef USE_SYSLOG
		syslog(LOG_NOTICE,"starthasrv returned successfully");
#else
		puts("DEBUG: start_services() returning successfully");
#endif
		return;
	}


#ifdef USE_SYSLOG
	syslog(LOG_ERR,"Error hit from starthasrv script");
#else
	fprintf(stderr,"ERROR: start_services (): starthasrv failed!(err %d)\n",
	         startstate);
#endif
	changestate(ERRORED);

}

/* stop_services: called by main loop, as a result of
 * either the monitor script, or indirectly by signal handler
 * if we get a HUP or INT signal
 * 
 *  - changes state to STOPPING
 *  - sends heartbeat
 *  - calls stophasrv script
 *  - changes state to STANDBY
 *  - sends heartbeat
 */
void stop_services()
{
	char stopscript[MAXPATHLEN+20];

	switch(mystate){
		case RUNNING:
		case STOPPING:
		case INITIAL:
		case ERRORED:
			break;
		default:
			puts("ERROR: stop_services called in unexpected state");
			return;
	}

	need_to_start=0; /* stop takes precedence over start */
	need_to_stop=0;

#ifdef USE_SYSLOG
	syslog(LOG_NOTICE,"stopping services");
#endif
	changestate(STOPPING);
	sendheartbeat();
	dump_states();

	sprintf(stopscript,"%s/%s",scriptdir, "stophasrv");
	system(stopscript);

}

/* Run monitor script.
 * This monitors the state of actively running services.
 * Change mystate to 'ERRORED', if monitor fails.
 * On ERRORED, will also set need_to_stop=1, and have_error=1
 *
 * Called specially, on first startup (startup=1), to determine if
 * demon has been started on a node where services are already running.
 * In this case, will either set mystate to RUNNING, or do nothing.
 */
void monitor_services(int firsttime)
{
	char monitorscript[MAXPATHLEN+20];
	int mon_status;

	sprintf(monitorscript,"%s/%s",scriptdir, "monitorhasrv");


	/* only run monitor, if we have reason to do so! */
	if(firsttime==0){
		switch(mystate)	{
			case RUNNING:
				break;
			default:
				return;
		}
	}

	mon_status=system(monitorscript);
	mon_status = WEXITSTATUS(mon_status);


	switch(mon_status){
		case 0: /* Services are running OK */
			if(firsttime==1){
#ifdef USE_SYSLOG
				syslog(LOG_NOTICE,"services detected as already running");
#else
				puts("DEBUG: services already running");
#endif
				changestate(RUNNING);
				return;
			}
			break;

		default:

			if(firsttime==0){
#ifdef USE_SYSLOG
				syslog(LOG_ERR,"monitorhasrv detected error");
#else
				printf("monitor status=%d: ERRORED!\n",mon_status);
#endif
				changestate(ERRORED);
				have_error=1;
			} else {
				printf("Services not running.");
				printf("Will Run stop_services for cleanup\n");
			}

			/* Sneaky hack: if firsttime started, and services
			 * are not running... flag that we should run
			 * stop_services, to clean up anything left behind
			 */
			need_to_stop=1;
	}
	
}


/* Run through list of systems, and see if we should timeout
 * any of them.
 */
void do_timeouts()
{
	struct statelist *stateptr=host_states;
	time_t timenow=time(NULL);
	int timeoutstate=htonl(TIMEDOUT);

	while(stateptr !=NULL){
		/* Skip my own line */
		if(strncmp(stateptr->msg.srchost, myhostname, HA_HOSTLEN)==0){
			stateptr=stateptr->next;
			continue;
		}
		if(stateptr->msg.status==timeoutstate){
			/* already in timeout state */
			stateptr=stateptr->next;
			continue;
		}

		if((timenow - stateptr->msg.timestamp) > timeoutsec){
#ifdef USE_SYSLOG
			syslog(LOG_ERR,"timeout for %s",stateptr->msg.srchost);
#else

			printf("DEBUG: timeout for system %s\n",
			       stateptr->msg.srchost);
#endif
			stateptr->msg.status=timeoutstate;
		}
		stateptr=stateptr->next;
	}
	
}

/* Determine who should be 'main' system.
 * If me, then flag 'need_to_start' so that main loop
 * will trigger service_start
 *
 * Will only flag need_to_start, if
 *  -  All nodes, or at minimum a 'quorum' (50%+1) are heard from
 * AND
 *  -  we are alphabetically first in the list of machines that arent screwed up
 * AND
 *  -  we are in 'STANDBY' state
 * AND
 *  - there is no other machine in state RUNNING or STOPPING or STARTING
 *
 */
void check_main()
{
	struct statelist *stateptr=host_states;

	if(am_main){
		return;
	}
	if(mystate!=STANDBY){
		return;
	}

	/* Check for if we're in the majority half of the cluster,
	 * in case of partial network failure.
	 * 2-node cluster is special case. We dont meetnormal quorum count,
	 *   of 50%+1.
	 * But on the other hand, we still have to try to start services,
	 * or whats the point of having a cluster in the first place!
	 */
	if(num_nodes>2){
		if(count_activenodes() <quorum_count){
			return;
		}
	}
	if(check_running()==1){
		/* cant do anything, if another node is already
		 * running services!
		 */
		return;
	}

	/* Now find first active, clean node, and if it is me,
	 * flag service start.
	 * Right now, that boils down to "first node in STANDBY state".
	 */
	while(stateptr !=NULL){
		int state=ntohl(stateptr->msg.status);
		switch(state){
		    case INITIAL:
			/* wait for other node to finish coming up */
			return;

		    case STANDBY:
			/* This is "first active node". Is it me? */
			if(strcasecmp(stateptr->msg.srchost, myhostname)==0){
				need_to_start=1;
			}
			return;
		}
		stateptr=stateptr->next;
	}

}


int main(int argc, char *argv[])
{
	int optc;
	char *statefile=NULL,*nameparse;
	FILE *pipefp;

	bzero(ipaddr,sizeof(struct in_addr) *3);
	bzero(netaddr,sizeof(struct in_addr) *3);

	listensock[0]=-1;
	listensock[1]=-1;
	listensock[2]=-1;
	sendsock[0]=-1;
	sendsock[1]=-1;
	sendsock[2]=-1;

	pipefp=popen("uname -n","r");
	fgets(myhostname,99,pipefp);
	myhostname[99]='\0';
	nameparse=&myhostname[0];
	/* okay, "_" isnt legal, or didnt used to be. but people use it */
	while(isalnum((int)*nameparse) ||
	      *nameparse=='-'|| *nameparse=='_'){
		nameparse++;
	}
	*nameparse='\0';

	printf("DEBUG: myhostname is '%s'\n",myhostname);
	pclose(pipefp); /* you might need fclose() instead */


	/* Need to:
	    - parse options
	    - bind UDP port
	    - broadcast our status
	    - listen for other machines' statuses
	       and then all the other stuff of course
	 */

	strcpy(scriptdir,BINDIR);

	while((optc=getopt(argc,argv,"a:b:c:A:B:C:l:mn:p:s:H:M:T:")) != -1){
		switch(optc){
		case 'a':
			inet_aton(optarg, &ipaddr[0]);
			break;
		case 'b':
			inet_aton(optarg, &ipaddr[1]);
			break;
		case 'c':
			inet_aton(optarg, &ipaddr[2]);
			break;

		case 's':
			strncpy(scriptdir,optarg,MAXPATHLEN-2);
			printf("DEBUG: script dir now set to %s\n",optarg);
			break;

		case 'l':
			statefile=optarg;
			printf("DEBUG: statefile now set to %s\n",statefile);
			break;

		case 'm':
			am_main=1;
			puts("DEBUG: I think I am the main node");
			puts("   I will start services in a few seconds.");
			break;

		case 'n':
			num_nodes=atoi(optarg);
			printf("DEBUG: number of nodes set to %d\n",num_nodes);
			break;

		case 'p':
			freeha_port=atoi(optarg);
			printf("DEBUG: port set to %d\n",freeha_port);
			break;

		case 'A':
			inet_aton(optarg, &netaddr[0]);
			break;
		case 'B':
			inet_aton(optarg, &netaddr[1]);
			break;
		case 'C':
			inet_aton(optarg, &netaddr[2]);
			break;
		case 'M':
			monitorsec=atoi(optarg);
			printf("DEBUG: monitor delay set to %d\n",
			        monitorsec);
			break;
		case 'T':
			timeoutsec=atoi(optarg);
			printf("DEBUG: timeout seconds set to %d\n",
			        timeoutsec);
			break;

		default:
			usage();
			exit(1);
		}
	}

	/* determine min number of nodes to have cluster happy */
	quorum_count=(num_nodes/2) + 1;

	{ /* check to see we have valid script directory */
		char scriptfile[MAXPATHLEN];
		sprintf(scriptfile,"%s/%s",scriptdir,"starthasrv");
		if(access(scriptfile,X_OK)!=0){
			fprintf(stderr,"ERROR: cannot access %s\n",scriptfile);
			exit(1);
		}
		sprintf(scriptfile,"%s/%s",scriptdir,"stophasrv");
		if(access(scriptfile,X_OK)!=0){
			fprintf(stderr,"ERROR: cannot access %s\n",scriptfile);
			exit(1);
		}
		sprintf(scriptfile,"%s/%s",scriptdir,"monitorhasrv");
		if(access(scriptfile,X_OK)!=0){
			fprintf(stderr,"ERROR: cannot access %s\n",scriptfile);
			exit(1);
		}
	}
	
	printf("addr1=%s\n", inet_ntoa(ipaddr[0]));
	printf("net1=%s\n", inet_ntoa(netaddr[0]));

	init_state_file(statefile);
	opensockets();

#ifdef USE_SIGNAL
# ifdef __svr4__
	sigset(SIGUSR1,sighandler); /* start services */
	sigset(SIGUSR2,sighandler); /* start services */
	sigset(SIGHUP,sighandler); /* stop services */
	sigset(SIGINT,sighandler); /* stop services and QUIT */
	sigset(SIGTERM,sighandler); /* stop services and QUIT */
# else
	signal(SIGUSR1,sighandler); /* start services */
	signal(SIGUSR2,sighandler); /* start services */
	signal(SIGHUP,sighandler); /* stop services */
	signal(SIGINT,sighandler); /* stop services and QUIT */
	signal(SIGTERM,sighandler); /* stop services and QUIT */
# endif /* __svr4__*/
#endif /* USE_SIGNAL */

#ifdef USE_SYSLOG
	openlog("FreeHA", LOG_CONS|LOG_PID, LOG_LOCAL1);
	syslog(LOG_NOTICE, "demon starting");
#else
	puts("demon starting");
#endif

	/* In theory, on initial start, should loop for a few seconds
	 * to figure out who gets to be top dog.
	 * THEN start up services, if I am main node.
	 * THEN go into main polling loop
	 * Or then again.. just force startup script to tell us that
	 * We are normally main node.
	 * Probably that is safer.
	 */

	if(am_main){
#ifdef USE_SYSLOG
		syslog(LOG_WARNING, "demon starting in FORCE-MAIN mode!!!");
#else
		puts("DEBUG: called in force-main mode");
		puts("  broadcasting primary role");
#endif
		changestate(STARTING);
		/* send a quickie msg right away, to try to inform
		 * other nodes that may be coming up, "IM DOING IT!"
		 * We get called if -m flag was added at startup.
		 * Note that we dont do any cross-checks.
		 * This makes using -m flag VERY DANGEROUS!!!
		 */
		sendheartbeat();
		dump_states();
		sleep(1);
		sendheartbeat();
		start_services();
	} else {
		/* Seeding cluster state tables for other nodes.
		 * Try for 200 seconds to see if we have all nodes
		 * communicating.
		 * If we see all nodes, continue immediately.
		 */
		int loop=200;

		lseek(state_fd, 0, SEEK_SET);
		write(state_fd,"Waiting for quorum of nodes to come online\n",43);

		while(loop-- >0){
			sendheartbeat();
			readheartbeats();
			if(count_activenodes() == num_nodes){
				break;
			}
			if(need_to_quit){
				exit(0);
			}
			sleep(1);
		}

		if(count_activenodes() < quorum_count){
#ifdef USE_SYSLOG
			syslog(LOG_ERR,"Cannot meet quorum node count. Quitting.");
#else
			fprintf(stderr,"ERROR: min nodes in cluster is %d\n",
				quorum_count);
			fprintf(stderr,"  Only have %d nodes. Cannot continue.\n",
				count_activenodes());
#endif
			exit(1);
		}

		monitor_services(1); /* are services already running? */
		if(need_to_stop){
			/* clean up, if neccessary */
			stop_services();
			changestate(STANDBY);
		} /* else should already be mystate==RUNNING*/
	}


	/****  MAIN LOOP ****/
	while(1){
		sendheartbeat();
		readheartbeats();

		do_timeouts();
		dump_states();
		monitor_services(0);

		if(need_to_stop){
			/* If need_to_stop flag set, call stop_services()
			 *   even if error detected.
			 * Perhaps only one service of multiple has failed.
			 * Need to cleanly shut down remaining services.
			 */
			if((mystate==RUNNING) || (mystate == ERRORED)){
				changestate(STOPPING);
				sendheartbeat();
				dump_states();
				stop_services();
			}
			if(need_to_quit){
#ifdef USE_SYSLOG
				syslog(LOG_NOTICE,"demon stopping");
#else
				puts("DEBUG: caught SIGINT. Quitting cleanly");
#endif
				exit(0);
				/* do NOT send heartbeat with STANDBY state,
				 * so that it is clear during the timeout
				 * that this machine is going offline
				 */
			}

			if(have_error==0){
				changestate(STANDBY);
			} else {
				changestate(ERRORED);
			}
			sendheartbeat();

		} else {
			/* who 'should' be main node? me?*/
			check_main(); 
			if(need_to_start){
				changestate(STARTING);
				sendheartbeat();
				dump_states();
				start_services();
			}
		}

		sleep(monitorsec);
	}
	/* End of MAIN LOOP */
	
}
