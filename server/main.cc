/* $Id: main.cc,v 1.7 2014/04/11 17:42:15 akadams Exp $ */

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>          // for exit on client
#include <string.h>          // for memset
#include <sysexits.h>
#include <unistd.h>

#include <string>
#include <list>
#include <map>
using namespace std;

#include "ErrorHandler.h"
#include "Logger.h"
#include "ConfInfo.h"
#include "SwitchInfo.h"
#include "SSLContext.h"
#include "SSLConn.h"
#include "TCPSession.h"
#include "tcp-event-procs.h"
#include "conga-procs.h"
#include "server-funcs.h"


#define DEBUG_MUTEX_LOCK 0

// Global variables.

// Static defaults.
static const int kFramingType = MsgHdr::TYPE_HTTP;
static const int kMaxPeers = 128;
static const ssize_t kDefaultBufSize = TCPSESSION_DEFAULT_BUFSIZE;
static const int state_poll_interval = 86400;  // 24 hours
const in_port_t kServerPort = CONGA_SERVER_PORT;

static const int kPollIntervalStatsMeter = 60;       // 1 minute
static const int kPollIntervalSDNStateReport = 300;  // 5 minutes
static const int kPollTimeoutStatsFlow = 180;        // 3 minutes

static size_t allocation_id_cnt = 0;


// Main event-loop routine: The CONGA server loads & processes
// some configuration variables and then enters its main event-loop
// waiting on (poll(2)) either for an external initiated connection or
// an internal event signaling that work needs to be done.

int main(int argc, char* argv[]) {
  ConfInfo conf_info;           // configuration information
  map<string, SwitchInfo> switches;  // hard-coded info regarding SDN
                                     // switches hashed by DPID
  list<MeterInfo> sdn_state;   // current knowlege of SDN flows
  pthread_mutex_t sdn_state_mtx;
  list<AuthInfo> authenticators;      // credentials currently active
  pthread_mutex_t authenticators_mtx;
  list<FlowInfo> flows;         // flows currently active (indexed by AuthInfo)
  pthread_mutex_t flow_list_mtx;
  list<TCPSession> to_peers;    // initated connections (to other nodes)
  //pthread_mutex_t to_peers_mtx = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_t to_peers_mtx;
  list<TCPSession> from_peers;  // received connections (from accept(2))
  //pthread_mutex_t from_peers_mtx = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_t from_peers_mtx;
  vector<pthread_t> thread_list;  // list to keep track of all threads
  //pthread_mutex_t thread_list_mtx = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_t thread_list_mtx;
  vector<SSLConn> servers;      // listen sockets, vector used to
                                // handle multple listening ports (we
                                // accept(2) or connect(2) to peers,
                                // though), e.g., IPv4, IPv6, aliases
  SSLContext ssl_context;  // TODO(aka) this should have a mutex too,
                           // as its internal reference will be
                           // incremented in both Accept & Socket, but
                           // SSL_Conn::Socket can occur in a thread,
                           // no?

  // Set default values (can be overridden by user).
  conf_info.log_to_stderr_ = 0;
  conf_info.port_ = kServerPort;
  conf_info.uid_ = getuid();
  conf_info.gid_ = getgid();
  /*
  conf_info.database_ = "127.0.0.1";
  conf_info.database_port_ = 3306;
  conf_info.database_db_ = "conga_data";
  conf_info.database_user_ = "conga";
  */
  conf_info.auth_info_list_file_ = "auth_info_list.json";
  conf_info.flow_info_list_file_ = "flow_info_list.json";

  logger.set_proc_name("conga");

  pthread_mutex_init(&authenticators_mtx, NULL);
  pthread_mutex_init(&flow_list_mtx, NULL);
  pthread_mutex_init(&to_peers_mtx, NULL);
  pthread_mutex_init(&from_peers_mtx, NULL);
  pthread_mutex_init(&thread_list_mtx, NULL);

  // Load "user" set-able global variables (ConfInfo) via command line.
  parse_command_line(argc, argv, &conf_info);

#if 0
  // TODO(aka) Setup file logging at some point.
  if (logger.mechanism_level(LOG_TO_FILE)) {
    try {  // t&c for util-local.a
      string* tmp_process_name = 
          new string(process_name(conf_info.Proc_ID()));
      logger.init_file(conf_info.Base_Path(), CONF_DIR, 
                       tmp_process_name->Tolower().Print());
      delete tmp_process_name;
    }
    if (error.Event()) {
      errx(EXIT_FAILURE, "%s", error.print().c_str());
    }
  }

  if (logger.mechanism_level(LOG_TO_SCRIPT))
    logger.init_script(conf_info.Base_Path());

  // It *should* now be safe for file/script logging (if enabled.)
#endif

  // Load configuration file (for additional user set-able globals).
  parse_conf_file(&conf_info);

  SwitchInfo tmp_switch;
  list<string> end_hosts;  // iterator constructor hack
  char const* mi_ptr[] = {"10.10.1.10", "10.10.1.20", "10.10.1.30" };
  list<string> mi_hosts(mi_ptr, mi_ptr + sizeof(mi_ptr) / sizeof(*mi_ptr));
  tmp_switch.Init("MI_dpid", "PSC", "tango.psc.edu", 2048, 4050, 4010,
                  mi_hosts);
  switches.insert(std::make_pair("1229782937975278821", tmp_switch));
  tmp_switch.clear();
  char const* wec_ptr[] = {"10.10.2.10", "10.10.2.30", "10.10.2.50" };
  list<string> wec_hosts(wec_ptr, wec_ptr + sizeof(wec_ptr) / sizeof(*wec_ptr));
  tmp_switch.Init("WEC_dpid", "PSC", "tango.psc.edu", 2048, 4050, 4010,
                  wec_hosts);
  switches.insert(std::make_pair("15730199386661060610", tmp_switch));
  tmp_switch.clear();
  char const* nics_ptr[] = {"10.10.3.111", "10.10.3.112", "10.10.3.113", "10.10.3.114" };
  list<string> nics_hosts(nics_ptr, nics_ptr + sizeof(nics_ptr) / sizeof(*nics_ptr));
  tmp_switch.Init("1_dpid", "NICS", "tango.psc.edu", 2048, 399, -1,
                  nics_hosts);
  switches.insert(std::make_pair("281646654550181", tmp_switch));

  //logger.Log(LOG_DEBUGGING, "main(): .", );

  // Make sure we've got all the *key* information that we need.

  // If STDERR logging has *not* been *explicitly* set by the user, 
  // turn off STDERR logging.  Note, however, that if we chose to run
  // in the 'foreground', and no logging was set by the user, then
  // STDERR will be turned back on when we daemonize();

  if (! conf_info.log_to_stderr_)
    logger.clear_mechanism(LOG_TO_STDERR);

  // Setup/initialize SSL.
  // HACK: const char* server_id = process_name(conf_info.Proc_ID());
  const char* server_id = "conga-tls";
  if (getuid() == 0)  // XXX Need to move cert locations to command-line or config file!
    ssl_context.Init(TLSv1_method(), server_id,
                     "limbo.psc.edu.key.pem", "/etc/ssl/private",
                     SSL_FILETYPE_PEM, NULL, 
                     "limbo.psc.edu.crt.pem", "/etc/ssl/certs",
                     SSL_FILETYPE_PEM, NULL, NULL, 
                     SSL_VERIFY_NONE, 2, SSL_SESS_CACHE_OFF,
                     SSL_OP_CIPHER_SERVER_PREFERENCE);
  else
    ssl_context.Init(TLSv1_method(), server_id,
                     "limbo.psc.edu.key.pem", "/home/pscnoc/conga/certs",
                     SSL_FILETYPE_PEM, NULL, 
                     "limbo.psc.edu.crt.pem", "/home/pscnoc/conga/certs",
                     SSL_FILETYPE_PEM, NULL, NULL,
                     SSL_VERIFY_NONE, 2, SSL_SESS_CACHE_OFF,
                     SSL_OP_CIPHER_SERVER_PREFERENCE);
#if 0  // XXX Additional ssl context suff that should be in Init()
  ssl_context.Load_CA_List(conf_info.Base_Path(), SAMI_CA_CERTS_DIR);
  String egd_path = conf_info.Base_Path() + "tmp/entropy";
  // FUTURE: Change rand file & dh parm to a path like egd path!
  ssl_context.Seed_PRNG(egd_path, conf_info.Base_Path(), "random.pem", "rand");
  ssl_context.Load_DH_Ephemeral_Keys(conf_info.Base_Path(), "dhparam-1024.pem", "rand");
#endif
  if (error.Event())
    errx(EXIT_FAILURE, "%s", error.print().c_str());

  if (!conf_info.v4_enabled_ && !conf_info.v6_enabled_)
    conf_info.v4_enabled_ = true;  // give us something

  // Setup the server's listen socket(s).
  if (conf_info.v4_enabled_) {
    SSLConn tmp_server;
    tmp_server.InitServer(AF_INET);  // listen on all IPv4 interfaces
    tmp_server.set_blocking();
    tmp_server.set_close_on_exec();
    tmp_server.Socket(PF_INET, SOCK_STREAM, 0, &ssl_context);
    tmp_server.Bind(conf_info.port_);
    tmp_server.Listen(TCPCONN_DEFAULT_BACKLOG);
    if (error.Event()) {
      logger.Log(LOG_ERROR, "%s, exiting ...", error.print().c_str());
      error.clear();
      exit(EXIT_FAILURE);
    }
    servers.push_back(tmp_server);  // no need to grab MUTEX, as not MT yet
  } 
  if (conf_info.v6_enabled_) {
    SSLConn tmp_server;
    tmp_server.InitServer(AF_INET6);  // listen on all interfaces ...
    tmp_server.set_blocking();
    tmp_server.set_close_on_exec();
    tmp_server.Socket(PF_INET6, SOCK_STREAM, 0, &ssl_context);
    int v6_only = 1;
    tmp_server.Setsockopt(IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, 
                          sizeof(v6_only));  // ... *only* IPv6 interfaces
    tmp_server.Bind(conf_info.port_);
    tmp_server.Listen(TCPCONN_DEFAULT_BACKLOG);
    if (error.Event()) {
      logger.Log(LOG_ERROR, "%s, exiting ...", error.print().c_str());
      error.clear();
      exit(EXIT_FAILURE);
    }
    servers.push_back(tmp_server);  // no need to grab MUTEX, as not MT yet
  }

  logger.Log(LOG_NOTICE, "conga (%s) starting on port %hu.", 
             SERVER_VERSION, conf_info.port_);

  // Daemonize program.
  if (getuid() == 0) {
    // Process is running as root, drop privileges.
    if (setgid(conf_info.gid_) != 0)
      err(EX_OSERR, "setgid() failed: %s", strerror(errno));
    if (setuid(conf_info.uid_) != 0)
      err(EX_OSERR, "setuid() failed: %s", strerror(errno));

    // Sanity check ...
    if (setuid(0) != -1)
      err(EX_OSERR, "ERROR: Conga managed to re-acquire root privileges!");
  }

  // TODO(aka) Need to add daemonizing stuff.

  // Initialze more globals & externals: PRNG, signals
  srandom(time(NULL));  // TODO(aka) We really need to use srandomdev() here!

#if 0
  sig_handler.Init();  // TODO(aka) need to add signal handling routines
#endif

  // Any work prior to the main event loop?

  // Initiate stats/meter state requests ...
  map<string, SwitchInfo>::iterator switch_itr = switches.begin();
  while (switch_itr != switches.end()) {
#if 0  // For Debugging: end_hosts check
    list<string> end_hosts = switch_itr->second.end_hosts();
    list<string>::iterator host_itr = end_hosts.begin();
    while (host_itr != end_hosts.end()) {
      printf("XXX Testing end_hosts: %s\n", host_itr->c_str());
      host_itr++;
    }
#endif

    initiate_stats_meter_request(conf_info, switch_itr->first,
                                 &to_peers, &to_peers_mtx);

    // And while we're at it, issue the onetime request for the
    // meters' maximum rates.

    initiate_stats_meterconfig_request(conf_info, switch_itr->first,
                                       &to_peers, &to_peers_mtx);
    //curl -X GET http://tango.psc.edu:8080/stats/meterconfig/1229782937975278821
    switch_itr++;
  }

  // Grab existing authenticators.
  auth_info_list_load_state(conf_info.auth_info_list_file_, &authenticators);
  allocation_id_cnt =
      flow_info_list_load_state(conf_info.flow_info_list_file_, &flows);

#if 1  // For Debugging: load state
  if (authenticators.size() > 0) {
    list<AuthInfo>::iterator auth_itr = authenticators.begin();
    string tmp_str(1024, '\0');
    while (auth_itr != authenticators.end()) {
      snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
               1024 - strlen(tmp_str.c_str()), "%s (%s:%s:%s:%d)",
               auth_itr->api_key_.c_str(), auth_itr->user_id_.c_str(), 
               auth_itr->project_id_.c_str(), auth_itr->resource_id_.c_str(),
               auth_itr->end_time_);
      auth_itr++;
      if (auth_itr != authenticators.end())
        snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
                 1024 - strlen(tmp_str.c_str()), ", ");
    }
    logger.Log(LOG_INFO, "main(): Loaded authenticators: %s.", tmp_str.c_str());
  }
  if (flows.size() > 0) {
    list<FlowInfo>::iterator flow_itr = flows.begin();
    string tmp_str(1024, '\0');
    while (flow_itr != flows.end()) {
      snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
               1024 - strlen(tmp_str.c_str()), "%s (%s:%d:%d:%d:%s:%s)",
               flow_itr->allocation_id_.c_str(), flow_itr->api_key_.c_str(),
               flow_itr->meter_, flow_itr->rate_, flow_itr->duration_,
               flow_itr->src_ip_.c_str(), flow_itr->dst_ip_.c_str());
      flow_itr++;
      if (flow_itr != flows.end())
        snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
                 1024 - strlen(tmp_str.c_str()), ", ");
    }
    logger.Log(LOG_INFO, "main(): Loaded flows: %s.", tmp_str.c_str());
  }
#endif

  // Setup poll timeout interval(s).
  time_t stats_meter_poll = time(NULL);
  time_t sdn_state_report = time(NULL);

  unsigned int debug_print_cnt = 0;  // For Debugging:

  // Main event-loop.
  struct pollfd pollfds[TCP_EVENT_MAX_FDS];
  int nfds;             // num file descriptors to poll on
  int timeout = 1000;	// in milliseconds
  int n;                // poll return value
  while (1) {
    // Note, errors that can be returned to our peers (as NACKs) will
    // be processed in conga-procs.cc or possibly ssl-event-proces.cc.
    // All other errors will be processed in ssl-event-proces.cc or
    // in here.

    // Load all active sockets into poll(2)'s array.
    nfds = 0;

    // Load *all* our listening sockets (v4 & v6, e.g.) ...
    for (int i = 0; i < (int)servers.size(); i++) {
      pollfds[nfds].fd = servers[i].fd();
      pollfds[nfds].events = POLLIN;  // always listening, always listening ...
      nfds++;
    }

    // Load any fds from any *active* peers.
    nfds = tcp_event_poll_init(to_peers, from_peers, TCP_EVENT_MAX_FDS, 
                               nfds, pollfds);
		
    // Check the fds with poll(2) (or select()).
    n = poll(pollfds, nfds, timeout);

    if (n < 0) {  // check for error
      logger.Log(LOG_DEBUG, "poll() interrupted, errno: %d: %s", 
                 errno, strerror(errno));

      if (errno != EINTR) {  // it's not a signal
        logger.Log(LOG_WARN, "poll() error: %s, returning.", strerror(errno));
      } else {	// it's a signal

        // TODO(aka) Not sure if we should simply check for an
        // un-trapped signal here, or whether we should *actually*
        // process the signal here.
        //
        // TODO(aka) At some point, I thought that NON_BLOCKING calls
        // worked better if we processed the signals at the end of the
        // event loop ...  However, since I can't remember why that
        // was, let's *start* with processing them here!

#if 0
        // TODO(aka) Process signal, *if* we have a handler.
        if (sig_handler.CheckAll())
          event_signal_check(ssl_context);
#endif
      }

      continue;	 // in any event, head back to start of event-loop
    }	// if (n < 0)

    if (n == 0) {  // check for timeout
      try {  // t&c for debugging
        //logger.Log(LOG_INFO, "poll() timeout, continuing ...");

        time_t now = time(NULL);

        // For Debugging:
        if (debug_print_cnt++ == 0)
          logger.Log(LOG_INFO, "main(): TIMEOUT: "
                     "now: %d, stats/meter poll: %d, sdn state poll: %d, "
                     "to_peers(%d), sdn_state(%d), flows(%d), "
                     "authenticators(%d).",
                     (int)now, (int)stats_meter_poll, (int)sdn_state_report,
                     (int)to_peers.size(), (int)sdn_state.size(),
                     (int)flows.size(), (int)authenticators.size());
        else
          if (debug_print_cnt == 20)
            debug_print_cnt = 0;

        // TIMEOUT-1: See if any of our *read* data in from_peers is complete.
#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): requesting from_peers lock.");
#endif
        pthread_mutex_lock(&from_peers_mtx);  // TODO(aka) what if not threaded?

        list<TCPSession>::iterator from_peer = from_peers.begin();
        while (from_peer != from_peers.end()) {
          // If it's an entire message (according to the headers), process it.
          if (from_peer->IsIncomingMsgComplete()) {
            if (from_peer->IsSynchroniationEnabled()) {
              // Note, synchronization makes sure the initiator of a
              // TCPSession uses to_peers, while the receiver uses
              // from_peers for the session.  Since (i) HTTP framing
              // doesn't need this, and (ii) we'd need to lock
              // to_peers, as well, we're making it a ERROR in here.

              /*
              // TOOD(aka) Remove lamport_ from ConfInfo, then we
              // don't need to pass ConfInfo as a ptr!

              tcp_event_synchronize_connection(true, &conf_info, &to_peers,
              &from_peers, from_peer);
              */
              logger.Log(LOG_ERR, "main(): "
                         "peer (%s) requested synchronization in timeout!",
                         from_peer->print().c_str());
              from_peer = from_peers.erase(from_peer);

#if DEBUG_MUTEX_LOCK
              warnx("main(): releasing from_peers lock.");
#endif
              pthread_mutex_unlock(&from_peers_mtx);
              continue;  // skip to next peer
            }

            if (conf_info.multi_threaded_ &&
                !from_peer->IsIncomingMsgBeingProcessed()) {
              // Build struct for function arguments.
              struct conga_incoming_msg_args args = {
                &conf_info, &ssl_context, &sdn_state, &sdn_state_mtx,
                &authenticators, &authenticators_mtx, 
                &flows, &flow_list_mtx,
                &to_peers, &to_peers_mtx, from_peer, from_peers.end(), 
                &thread_list, &thread_list_mtx,
              };

              // Create thread, store ID, and process message concurrently.
              pthread_t tid;
              pthread_create(&tid, NULL, &conga_concurrent_process_incoming_msg,
                             (void*)&args);
              pthread_mutex_lock(&thread_list_mtx);
              thread_list.push_back(tid);
              pthread_mutex_unlock(&thread_list_mtx);

              logger.Log(LOG_DEBUG, "main(timeout): "
                         "thread %d assigned to peer: %s.",
                         tid, from_peer->print().c_str());
            } else if (!conf_info.multi_threaded_) {
              // Process message single threaded.
              conga_process_incoming_msg(&conf_info, &ssl_context,
                                         &sdn_state, &sdn_state_mtx,
                                         &authenticators, &authenticators_mtx, 
                                         &flows, &flow_list_mtx,
                                         &to_peers, &to_peers_mtx, from_peer);
              if (error.Event()) {
                // Note, if we reached here, then we could not NACK the
                // error we encountered processing the message in
                // conga-procs.cc.

                logger.Log(LOG_ERR, "main(): "
                           "conga_process_incoming_msg(from_peer) failed: %s", 
                           error.print().c_str());
                from_peer = from_peers.erase(from_peer);
                error.clear();
#if DEBUG_MUTEX_LOCK
                warnx("main(timeout): releasing from_peers lock.");
#endif
                pthread_mutex_unlock(&from_peers_mtx);
                continue;  // start processing next peer
              }
            }  // } else if (!conf_info.multi_threaded_) {

            // Head back to while() to give
            // conga_process_incoming_msg() time to update peer's
            // meta-data.

#if DEBUG_MUTEX_LOCK
            warnx("main(timeout): releasing from_peers lock.");
#endif
            pthread_mutex_unlock(&from_peers_mtx);
            from_peer++;
            continue;  // start processing next peer
          }  // } else if (from_peer->IsIncomingMsgComplete()) {

          // Note, we don't check for non-initiated connections, as we
          // assume senders would be using to_peers.

          //logger.Log(LOG_INFO, "main(timeout): checking peer: %s, thread list: %d.", from_peer->print().c_str(), thread_list.size());
          
          // Finally, see if this peer should be removed.  Note, if
          // the connection to from_peers is closed, the game is
          // already over ...

          if (!from_peer->IsConnected() ||  
              (!from_peer->IsOutgoingDataPending() &&
               !from_peer->rbuf_len() && 
               !from_peer->IsIncomingMsgInitialized() &&
               (from_peer->timeout() < now))) {
            // Make sure, if we're multi-threaded, that the thread
            // isn't still running.

            if (conf_info.multi_threaded_) {
              bool found = false;
              for (vector<pthread_t>::iterator tid = thread_list.begin();
                   tid != thread_list.end(); tid++) {
                if (*tid == from_peer->rtid()) {
                  found = true;
                  break;
                }
              }
              if (found) {
                logger.Log(LOG_DEBUG, "main(timeout): can't remove peer: %s, "
                           "thread still active.", 
                           from_peer->print().c_str());
                from_peer++;
              } else {
                if (from_peer->IsOutgoingDataPending())
                  logger.Log(LOG_WARN, "Removing peer %s, "
                             "even though a response is pending.", 
                             from_peer->print().c_str());
                else
                  logger.Log(LOG_DEBUG, "main(timeout): Removing peer %s.", 
                             from_peer->print().c_str());
                from_peer = from_peers.erase(from_peer);
              }
            } else {
              //printf("XXX IsConnect: %d, IsOutgoingDatapending: %d, rbuf_len: %d, IsIncomingMsgInit: %d.\n", from_peer->IsConnected(), from_peer->IsOutgoingDataPending(), from_peer->rbuf_len(), from_peer->IsIncomingMsgInitialized());
              logger.Log(LOG_DEBUG, "main(timeout): Removing peer %s.", 
                         from_peer->print().c_str());
              from_peer = from_peers.erase(from_peer);
            }

#if DEBUG_MUTEX_LOCK
            warnx("main(timeout): releasing from_peers lock.");
#endif
            pthread_mutex_unlock(&from_peers_mtx);
            continue;  // head back to while()
          }  // if (!from_peer->IsConnected() ||  

          /*
          // TODO(aka) Spot for final check to see if something whet wrong ...
          if (!from_peer->IsIncomingMsgInitialized() && 
          from_peer->rbuf_len() > 0) {
          // Something went wrong, report the error and remove the peer.
          logger.Log(LOG_ERR, "main(): "
          "peer (%s) has data in timeout, but not initialized!",
          from_peer->print().c_str());
          from_peer = from_peers.erase(from_peer);
          #if DEBUG_MUTEX_LOCK
          warnx("main(): releasing from_peers lock.");
          #endif
          pthread_mutex_unlock(&from_peers_mtx);
          continue;  // head back to while()
          } 
          */

          from_peer++;
        }  // while (from_peer != from_peers.end()) {

#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): releasing from_peers lock.");
#endif
        pthread_mutex_unlock(&from_peers_mtx);

        // TIMEOUT-2: See if any of our *read* data in to_peers is
        // complete, then see if we have outgoing data ready for
        // transmission.

#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): requesting to_peers lock.");
#endif
        pthread_mutex_lock(&to_peers_mtx);
        list<TCPSession>::iterator to_peer = to_peers.begin();
        while (to_peer != to_peers.end()) {
          // If it's an entire message (according to the headers), process it.
          if (to_peer->IsIncomingMsgComplete()) {
            if (to_peer->IsSynchroniationEnabled()) {
              // Note, synchronization makes sure the initiator of
              // a TCPSession uses to_peers, while the receiver uses
              // from_peers for the session.  Since (i) HTTP framing
              // doesn't need this, and (ii) we'd need to lock
              // to_peers, as well, we're making it a ERROR in here.

              /*
              // TOOD(aka) Remove lamport_ from ConfInfo, then we
              // don't need to pass ConfInfo as a ptr!

              tcp_event_synchronize_connection(true, &conf_info, &to_peers,
              &from_peers, from_peer);
              */
              logger.Log(LOG_ERR, "main(): "
                         "peer (%s) requested synchronization in timeout!",
                         to_peer->print().c_str());
              to_peer = to_peers.erase(to_peer);
#if DEBUG_MUTEX_LOCK
              warnx("main(timeout): releasing to_peers lock.");
#endif
              pthread_mutex_unlock(&to_peers_mtx);
              continue;  // head back to while()
            }

            // TODO(aka) Note, we currently process all *initiated*
            // connections within conga-procs as sequential, blocking
            // processes.  When we decide to move away from that,
            // we'll need to setup our multi-threading processing here
            // (as we did with from_peers).

            // Process message single threaded.
            conga_process_incoming_msg(&conf_info, &ssl_context,
                                       &sdn_state, &sdn_state_mtx,
                                       &authenticators, &authenticators_mtx, 
                                       &flows, &flow_list_mtx,
                                       &to_peers, &to_peers_mtx, to_peer);
            if (error.Event()) {
              // Note, if we reached here, then we could not NACK the
              // error we encountered processing the message in
              // conga-procs.cc.

              logger.Log(LOG_ERR, "main(): "
                         "conga_process_incoming_msg(to_peer) failed: %s", 
                         error.print().c_str());
              to_peer = to_peers.erase(to_peer);
              error.clear();

#if DEBUG_MUTEX_LOCK
              warnx("main(timeout): releasing to_peers lock.");
#endif
              pthread_mutex_unlock(&to_peers_mtx);
              continue;  // head back to while()
            }

            // Head to next peer to give conga_proccess_incoming_msg()
            // time to update peer's meta-data.

#if DEBUG_MUTEX_LOCK
            warnx("main(timeout): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);
            to_peer++;
            continue;
          }  // } else if (to_peer->IsIncomingMsgComplet()) {

          // See if we need to initiate a connection.
          if (to_peer->wbuf_len() && 
              to_peer->IsOutgoingDataPending() && 
              !to_peer->IsConnected()) {
            to_peer->Connect();
            if (error.Event()) {
              // Bleh.  Can't connect.
              logger.Log(LOG_ERR, "main(timeout): "
                         "Unable to connect to %s: %s", 
                         to_peer->print().c_str(), error.print().c_str());
              // peer->PopOutgoingMsgQueue();  // can't send, so pop message
              to_peer = to_peers.erase(to_peer);  // to_peers is not MT
              error.clear();
     
#if DEBUG_MUTEX_LOCK
              warnx("main(timeout): releasing to_peers lock.");
#endif
              pthread_mutex_unlock(&to_peers_mtx);
              continue;  // head back to while()
            } 
          }

          // Finally, see if this peer should be removed.  Note, lack
          // of to_peer->timeout() check.

          if (!to_peer->IsConnected() && 
              !to_peer->IsOutgoingDataPending() &&
              !to_peer->rbuf_len() && 
              !to_peer->IsIncomingMsgInitialized()) {
            //logger.Log(LOG_INFO, "main(timeout): checking if to_peer: %s, can be removed.", to_peer->print().c_str());

            if (conf_info.multi_threaded_) {
              bool found = false;
              for (vector<pthread_t>::iterator tid = thread_list.begin();
                   tid != thread_list.end(); tid++) {
                if (*tid == to_peer->rtid()) {
                  found = true;
                  break;
                }
              }
              if (found) {
                logger.Log(LOG_DEBUG, "main(timeout): can't remove peer: %s, "
                           "thread still active.", 
                           to_peer->print().c_str());
                to_peer++;
              } else {
                logger.Log(LOG_DEBUG, "main(timeout): Removing peer %s.",
                           to_peer->print().c_str());
                to_peer = to_peers.erase(to_peer);
              }
            } else {
              logger.Log(LOG_DEBUG, "main(timeout): Removing peer %s.",
                         to_peer->print().c_str());
              to_peer = to_peers.erase(to_peer);
            }

            // Skip to next peer.
#if DEBUG_MUTEX_LOCK
            warnx("main(timeout): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);
            continue;
          }

          /*
          // TODO(aka) Spot for final check to see if something whet wrong ...
          if (!to_peer->IsIncomingMsgInitialized() && 
          to_peer->rbuf_len() > 0) {
          // Something went wrong, report the error and remove the peer.
          logger.Log(LOG_ERR, "main(): "
          "peer (%s) has data in timeout, but not initialized!",
          to_peer->print().c_str());
          to_peer = to_peers.erase(to_peer);
          #if DEBUG_MUTEX_LOCK
          warnx("main(): releasing to_peers lock.");
          #endif
          pthread_mutex_unlock(&to_peers_mtx);
          continue;  // head back to while()
          } 
          */

          to_peer++;
        }  // while (to_peer != to_peers.end()) {
#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): releasing to_peers lock.");
#endif
        pthread_mutex_unlock(&to_peers_mtx);

        // TIMEOUT-3: See if we should request a state update.
        if (stats_meter_poll <= now) {
          map<string, SwitchInfo>::iterator switch_itr = switches.begin();
          while (switch_itr != switches.end()) {
            initiate_stats_meter_request(conf_info, switch_itr->first, &to_peers,
                                         &to_peers_mtx);
            if (error.Event()) {
              logger.Log(LOG_ERR, "main(timeout): "
                         "Failed to initialize peer: %s", error.print().c_str());
              error.clear();
            }
            switch_itr++;
          }

          stats_meter_poll += kPollIntervalStatsMeter;
        }  // if (stats_meter_poll <= now) {

#if 0  // XXX Old TIMEOUT-4 that looked for meters that matched our
       // *match* parameters.  See new TIMEOUT-4 below.

        // TIMEOUT-4: See if we need to poll for a flow (to learn meter-id).
#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): requesting flow list lock.");
#endif
        pthread_mutex_lock(&flow_list_mtx);
        list<FlowInfo>::iterator flow_itr = flows.begin();
        while (flow_itr != flows.end()) {
          //printf("XXX checking flow (%s), current meter: %d, polled: %d.\n", flow_itr->allocation_id_.c_str(), flow_itr->meter_, (int)flow_itr->polled_);
          if (flow_itr->meter_ <= 0 &&
              (flow_itr->polled_ + kPollTimeoutStatsFlow) <= now) {
            // Figure out which switch (based on flow's dst) we want to poll.
            string tmp_dpid;
            map<string, SwitchInfo>::iterator switch_itr = switches.begin();
            while (switch_itr != switches.end()) {
              list<string> end_hosts = switch_itr->second.end_hosts();
              list<string>::iterator host_itr = end_hosts.begin();
              while (host_itr != end_hosts.end()) {
                // Since hosts are dotted-quad, no need to
                // transform() to lower-case before compare()!

                if (!flow_itr->dst_ip_.compare(*host_itr))
                  break;
                host_itr++;
              }
              if (host_itr != end_hosts.end()) {
                // Nice, found the correct switch, so send out our
                // request to learn the meter-id!

                if (flow_itr->peer_ == 0) {
                  logger.Log(LOG_WARNING, 
                             "main(): No peer found for Flow (%s -> %s).",
                             flow_itr->src_ip_.c_str(),
                             flow_itr->dst_ip_.c_str());
                } else {
                  initiate_stats_flow_request(conf_info, switch_itr->first,
                                              flow_itr->dst_ip_,
                                              switch_itr->second,
                                              &to_peers, &to_peers_mtx);
                  if (error.Event()) {
                    logger.Log(LOG_ERR, "main(timeout): "
                               "Failed to initialize peer: %s",
                               error.print().c_str());
                    error.clear();
                  }
                  flow_itr->polled_ = now;
                }

                break;  // leave switch loop and move on to the next flow
              }  // if (host_itr != end_hosts.end()) {

              switch_itr++;
            }  // while (switch_itr != switches.end()) {
          }  // if (flow_itr->allocation_id_.size() <= 0) {

          flow_itr++;
        }  // while (flow_itr != flows.end()) {
#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): releasing flow list lock.");
#endif
        pthread_mutex_unlock(&flow_list_mtx);
#endif  // #if 0

        // TIMEOUT-4: Check what flows have not been associated to a meter.
        //
        // Note, in this new version, FlowInfo.polled_ now represents
        // successful acknowledgement (i.e., 200) from the Ryu
        // controller after we've requested our *match* parameters be
        // associated with the chosen meter.

#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): requesting flow list lock.");
#endif
        pthread_mutex_lock(&flow_list_mtx);
        list<FlowInfo>::iterator flow_itr = flows.begin();
        while (flow_itr != flows.end()) {
          if (flow_itr->meter_ <= 0) {
            printf("XXX flow (%s) has no meter and a requested bandwidth of: %d.\n", flow_itr->allocation_id_.c_str(), (int)flow_itr->rate_);

            // First, figure out the correct switch (based on flow's dst).
            string tmp_dpid;
            map<string, SwitchInfo>::iterator switch_itr = switches.begin();
            while (switch_itr != switches.end()) {
              list<string> end_hosts = switch_itr->second.end_hosts();
              list<string>::iterator host_itr = end_hosts.begin();
              while (host_itr != end_hosts.end()) {
                // Since hosts are dotted-quad, no need to
                // transform() to lower-case before compare()!

                if (!flow_itr->dst_ip_.compare(*host_itr))
                  break;
                host_itr++;
              }
              if (host_itr != end_hosts.end())
                break;  // nice, found the correct switch

              switch_itr++;  // continue on to look at next switch
            }  // while (switch_itr != switches.end()) {
            if (switch_itr == switches.end()) {
              logger.Log(LOG_WARNING, 
                         "main(): No DPID found for Flow (%s -> %s).",
                         flow_itr->src_ip_.c_str(),
                         flow_itr->dst_ip_.c_str());

              flow_itr++;
              continue;  // process next flow
            }

            string dpid = switch_itr->first;  // for convenience, store our dpid
            //flow_itr->dpid_ = switch_itr->first;  // store our dpid

            // Next, find a meter that we can use (i.e., 0 flow_count,
            // same dpid, same rate/bandwidth ...

#if DEBUG_MUTEX_LOCK
            warnx("main(timeout): requesting sdn_state lock.");
#endif
            pthread_mutex_lock(&sdn_state_mtx);

            //string tmp_msg(1024, '\0');
            list<MeterInfo>::iterator meter_itr = sdn_state.begin();
            while (meter_itr != sdn_state.end()) {
              printf("XXX main(): checking meter (%d), dpid (%s), rate (%u) to flow dpid (%s), bandwidth (%d).\n", meter_itr->meter_, meter_itr->dpid_.c_str(), (unsigned int)meter_itr->rate_, dpid.c_str(), flow_itr->rate_);
              if (!meter_itr->dpid_.compare(dpid) &&
                  meter_itr->rate_ == (uint64_t)flow_itr->rate_ &&
                  meter_itr->flow_count_ == 0) {
                flow_itr->meter_ = meter_itr->meter_;
                break;  // found one
              }

              meter_itr++;
            }
            if (meter_itr == sdn_state.end()) {
              logger.Log(LOG_ERR, "main(): "
                         "Failed to find an unsused meter on %s "
                         "for Flow (%s -> %s).",
                         dpid.c_str(), flow_itr->src_ip_.c_str(),
                         flow_itr->dst_ip_.c_str());
              flow_itr++;
              continue;  // check next flow
            }

#if DEBUG_MUTEX_LOCK
            warnx("main(timeout): releasing sdn_state lock.");
#endif
            pthread_mutex_unlock(&sdn_state_mtx);

            // Okay, we have an available meter, let's request the
            // switch to assign our flow to that meter.

            string err_msg(1024, '\0');
            allocation_id_cnt = 
                conga_request_stats_flowentry_modify(conf_info, dpid,
                                                     switch_itr->second,
                                                     *meter_itr, now,
                                                     allocation_id_cnt,
                                                     &ssl_context,
                                                     flow_itr);
            if (error.Event()) {
              snprintf((char*)err_msg.c_str(), 1024,
                       "Failed to assign flow dst (%s) to meter (%d) "
                       "on controller (%s): %s",
                       flow_itr->dst_ip_.c_str(), meter_itr->meter_,
                       switch_itr->second.name_.c_str(), error.print().c_str()); 
              error.clear();

              logger.Log(LOG_NOTICE, "main(timeout): %s", err_msg.c_str());

              // Return rejection and erase flow.
              initiate_post_allocation_response(conf_info, *flow_itr, err_msg,
                                                &from_peers,
                                                &from_peers_mtx);
              if (error.Event()) {
                logger.Log(LOG_ERR, "main(timeout): "
                           "Failed to intialize response to peer: %s",
                           error.print().c_str());
                error.clear();
              }

              flow_itr = flows.erase(flow_itr);
              flow_info_list_save_state(conf_info.flow_info_list_file_, flows);
              continue;  // check next flow
            }

            // Success, so inform the user!
            initiate_post_allocation_response(conf_info, *flow_itr, err_msg,
                                              &from_peers,
                                              &from_peers_mtx);
            if (error.Event()) {
              logger.Log(LOG_ERR, "main(timeout): "
                         "Failed to intialize response to peer: %s",
                         error.print().c_str());
              error.clear();
            }

            flow_info_list_save_state(conf_info.flow_info_list_file_, flows);
          }  // if (flow_itr->meter_ <= 0) {

          flow_itr++;
        }  // while (flow_itr != flows.end()) {
#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): releasing flow list lock.");
#endif
        pthread_mutex_unlock(&flow_list_mtx);

#if 0 // TODO(aka): This timeout's stuff has been moved the last half
      // of TIMEOUT-4.  Eventually, we need to add this (or some
      // alternative) throughput test, as opposed to just seeing if we
      // have a meter available.

        // TIMEOUT-5: Check (waiting) flows for meter-ids.
#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): requesting flow list lock.");
#endif
        pthread_mutex_lock(&flow_list_mtx);
        flow_itr = flows.begin();
        while (flow_itr != flows.end()) {
          if (flow_itr->allocation_id_.size() <= 0 && flow_itr->meter_ > 0) {
            // Check SDN state for our meter and see if we have bandwidth!
#if DEBUG_MUTEX_LOCK
            warnx("main(timeout): requesting sdn_state lock.");
#endif
            pthread_mutex_lock(&sdn_state_mtx);

            string tmp_msg(1024, '\0');
            list<MeterInfo>::iterator meter_itr = sdn_state.begin();
            while (meter_itr != sdn_state.end()) {
              if (flow_itr->meter_ == meter_itr->meter_)
                break;  // found it

              meter_itr++;
            }
            if (meter_itr == sdn_state.end()) {
              logger.Log(LOG_ERR, "main(): "
                         "Failed to find meter (%d) for Flow (%s -> %s).",
                         flow_itr->meter_, flow_itr->src_ip_.c_str(),
                         flow_itr->dst_ip_.c_str());
              flow_itr++;
              continue;  // check next flow
            }

            // See if we have enough bandwidth to fulfill request.

            logger.Log(LOG_WARNING, "main(): TODO(aka) In TIMEOUT-5, we also need to check existing allocations.  It would make sense to add this to MeterInfo, but need to figure out.  Perhaps hashed on expiration?");

            int throughput = 
                ((int)(meter_itr->byte_in_count_ - 
                       meter_itr->prev_byte_in_count_) /
                 (int)(meter_itr->time_ - meter_itr->prev_time_));
            if (throughput > 0)
              //printf("XXX meter %d, %db/s based on %lldb (%lld - %lld) in %lds.\n", meter_itr->meter_, throughput, (meter_itr->byte_in_count_ - meter_itr->prev_byte_in_count_), meter_itr->byte_in_count_, meter_itr->prev_byte_in_count_, (meter_itr->time_ - meter_itr->prev_time_));

#if DEBUG_MUTEX_LOCK
            warnx("main(timeout): releasing sdn_state lock.");
#endif
            pthread_mutex_unlock(&sdn_state_mtx);

            logger.Log(LOG_WARNING, "main(): TODO(aka) Need to substract throughput (%d) from rate (unknown) to see if greater than request (%d)!", throughput, flow_itr->rate_);

            string err_msg(1024, '\0');
            if (1) {  // TODO(aka) if ((throughput + bandwidth) < rate)
              // Generate a unique allocation id & send it back to requestor.
              char tmp_buf[64];  // assuming no more than 64 digits
                                 // (64 bits ~= 20 digits)
              snprintf(tmp_buf, 64, "%lu", (unsigned long)++allocation_id_cnt);
              flow_itr->allocation_id_ = tmp_buf;
              flow_itr->expiration_ = now + flow_itr->duration_;
              initiate_post_allocation_response(conf_info, *flow_itr, err_msg,
                                                &from_peers,
                                                &from_peers_mtx);
              if (error.Event()) {
                logger.Log(LOG_ERR, "main(timeout): "
                           "Failed to intialize response to peer: %s",
                           error.print().c_str());
                error.clear();
              }

              flow_info_list_save_state(conf_info.flow_info_list_file_, flows);
            } else {
              // Return rejection and erase flow.
              snprintf((char*)err_msg.c_str(), 1024, 
                       "bandwidth > rate - throughput");
              initiate_post_allocation_response(conf_info, *flow_itr, err_msg,
                                                &from_peers,
                                                &from_peers_mtx);
              if (error.Event()) {
                logger.Log(LOG_ERR, "main(timeout): "
                           "Failed to intialize response to peer: %s",
                           error.print().c_str());
                error.clear();
              }

              flow_itr = flows.erase(flow_itr);
              flow_info_list_save_state(conf_info.flow_info_list_file_, flows);
              continue;
            }
          }  // if (flow_itr->allocation_id_.size() <= 0 && flow_itr->meter_ > 0) {

          flow_itr++;
        }  // while (flow_itr != flows.end()) {
#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): releasing flow list lock.");
#endif
        pthread_mutex_unlock(&flow_list_mtx);
#endif // #if 0

        // TIMEOUT-6: Kill off expired authenticators & flows.
#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): requesting authenticators lock.");
#endif
        pthread_mutex_lock(&authenticators_mtx);

        list<AuthInfo>::iterator auth_itr = authenticators.begin();
        while (auth_itr != authenticators.end()) {
          if (auth_itr->end_time_ < now) {
            auth_itr = authenticators.erase(auth_itr);
          } else {
            auth_itr++;
          }
        }

#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): releasing authenticators lock.");
#endif
        pthread_mutex_unlock(&authenticators_mtx);

        // XXX TODO(aka) How do we verify a flow stopped?

        // TIMEOUT-7: Report switch throughputs.
        if (sdn_state_report <= now) {
#if DEBUG_MUTEX_LOCK
          warnx("main(timeout): requesting sdn_state lock.");
#endif
          pthread_mutex_lock(&sdn_state_mtx);

          string tmp_msg(1024, '\0');
          list<MeterInfo>::iterator meter_itr = sdn_state.begin();
          while (meter_itr != sdn_state.end()) {
            // Skip meters that have not updated in a while.
            if (meter_itr->time_ < (now - (kPollIntervalStatsMeter * 3))) {
              printf("XXX Skipping %d, time: %d < %d (now (%d) - 3 * Poll (%d)).\n", meter_itr->meter_, (int)meter_itr->time_, (int)(now - (kPollIntervalStatsMeter * 3)), (int)now, (int)(kPollIntervalStatsMeter * 3));
              meter_itr++;
              continue;
            }

            int throughput = 
                ((int)(meter_itr->byte_in_count_ - 
                       meter_itr->prev_byte_in_count_) /
                 (int)(meter_itr->time_ - meter_itr->prev_time_));
            if (throughput > 0)
              printf("XXX meter %d, %db/s based on %lldb (%lld - %lld) in %lds.\n", meter_itr->meter_, throughput, (meter_itr->byte_in_count_ - meter_itr->prev_byte_in_count_), meter_itr->byte_in_count_, meter_itr->prev_byte_in_count_, (meter_itr->time_ - meter_itr->prev_time_));

            snprintf((char*)tmp_msg.c_str() + strlen(tmp_msg.c_str()), 
                     1024 - strlen(tmp_msg.c_str()),
                     "Meter %d (dpid: %s, rate: %lu, flow count: %d) "
                     "throughput: %d (%s)", 
                     meter_itr->meter_, meter_itr->dpid_.c_str(),
                     (unsigned long)meter_itr->rate_, meter_itr->flow_count_,
                     throughput, meter_itr->flag_rate_.c_str());
            meter_itr++;
            if (meter_itr == sdn_state.end())
              snprintf((char*)tmp_msg.c_str() + strlen(tmp_msg.c_str()), 
                       1024 - strlen(tmp_msg.c_str()), ".");
            else 
              snprintf((char*)tmp_msg.c_str() + strlen(tmp_msg.c_str()), 
                       1024 - strlen(tmp_msg.c_str()), ", ");
          }

          logger.Log(LOG_NOTICE, "%s", tmp_msg.c_str());

#if DEBUG_MUTEX_LOCK
          warnx("main(timeout): releasing sdn_state lock.");
#endif
          pthread_mutex_unlock(&sdn_state_mtx);
          sdn_state_report += kPollIntervalSDNStateReport;
        }  // if (sdn_state_report <= now) {

        // TIMEOUT-8: Clean-up.

        // XXX TODO(aka) In short, we need to check each meter, and if
        // it wasn't updated (look at time & prev_time), then we
        // should remove it from the sdn state!

      } catch (...) {
        logger.Log(LOG_ERR, "main(): "
                   "Unexpected exception thrown during TIMEOUT proccessing.");
      }

      continue;  // head back to poll()
    }  // if (n == 0) {

    // Must have (at least) one ready fd.

    //logger.Log(LOG_DEBUGGING, "poll() fired, num ready: %d", n);

    int i = 0;  // index into pollfds[] array

    // First, check our listen socket(s), i.e.,
    // pollfds[0..(servers.size()-1)] ...

    for (int j = 0; j < (int)servers.size(); j++) {
      if (pollfds[j].revents && pollfds[j].revents & POLLIN) {
        try {  // t&c for debugging
#if DEBUG_MUTEX_LOCK
          warnx("main(listen): requesting from_peers lock.");
#endif
          pthread_mutex_lock(&from_peers_mtx);

          // As we haven't added anything to from_peers yet, we handle
          // all errors internally in tcp_event_accept().

          tcp_event_accept(conf_info, servers[j], kMaxPeers, kFramingType,
                           &ssl_context, &from_peers);
#if DEBUG_MUTEX_LOCK
          warnx("main(listen): releasing from_peers lock.");
#endif
          pthread_mutex_unlock(&from_peers_mtx);

        } catch (...) {
          logger.Log(LOG_ERR, "main(): "
                     "Unexpected exception thrown in listen socket POLLIN "
                     "proccessing.");
        }

        n--;
      } else if (pollfds[j].revents) {
        logger.Log(LOG_WARN, "Server[%d]: %d, revents: %d, continuing ...",
                   j, pollfds[j].fd, pollfds[j].revents);
        n--;
      }
    }

    i = servers.size();  // remember that "i = 0" is the first listen socket

    // Now, check the rest of the fds ...
    while (n > 0) {

      // Note, under OSX, poll() returns the *number of events*, not
      // the number of ready file descriptors like the man page says.
      // Big difference if and when a single fd has both read and
      // write ready data on it!

      // TODO(aka) If any other O/S do this, as well, then we'll
      // change the code to decrement n after every event.

      //logger.Log(LOG_INFO, "main(event): Processing event %d, total left: %d.", i, n);

      int tmp_i = i;  // For Debugging ...
      if ((i = tcp_event_poll_status(pollfds, nfds, i)) < 0) {
        logger.Log(LOG_ERROR, "tcp_event_poll_status() failed, "
                   "nfds = %d, i = %d, prev i = %d, n = %d.", 
                   nfds, i, tmp_i, n);
        break;
      }

      // Check POLLIN first ...
      if (pollfds[i].revents & POLLIN) {
        try {  // t&c for debugging
          // Check to_peers for our fd (first grab lock!) ...
#if DEBUG_MUTEX_LOCK
          warnx("main(POLLIN): requesting to_peers lock.");
#endif
          pthread_mutex_lock(&to_peers_mtx);
          list<TCPSession>::iterator peer =
              tcp_event_poll_get_peer(conf_info, pollfds[i].fd, &to_peers);
          if (peer != to_peers.end()) {
            // Slurp up the waiting data, and initialize (if not yet).
            tcp_event_read(conf_info, peer);
            // tcp_event_read(conf_info, &(*peer));  // note, iterator hack

            if (!peer->IsIncomingMsgInitialized()) {
              peer->InitIncomingMsg();
              //logger.Log(LOG_INFO, "main(POLLIN): post-init to_peer: %s.", peer->print().c_str());
            }
            if (error.Event()) {
              logger.Log(LOG_ERR, "main(POLLIN): "
                         "Unable to read incoming data from %s: %s.",
                         peer->print().c_str(), error.print().c_str());
              to_peers.erase(peer);  // to_peers is not multi-threaded
              error.clear();

              // Fall-through to release locks.
            } else {
#if CLIENT_CONNECTION
#endif
            }

            // Since we have to release our to_peers lock prior to
            // grabbing our from_peers lock, we also need to release it
            // here, in this equivalent nested branch.

#if DEBUG_MUTEX_LOCK
            warnx("main(POLLIN): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);
          } else {  // if (peer != to_peers.end()) {
            // Grab from_peers lock (first, relase to_peers lock).
#if DEBUG_MUTEX_LOCK
            warnx("main(POLLIN): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);

#if DEBUG_MUTEX_LOCK
            warnx("main(POLLIN): requesting from_peers lock.");
#endif
            pthread_mutex_lock(&from_peers_mtx);
            peer = 
                tcp_event_poll_get_peer(conf_info, pollfds[i].fd, &from_peers);
            if (peer == from_peers.end()) {
              logger.Log(LOG_ERR, "main(POLLIN): "
                         "Unable to find peer for file descriptor: %d",
                         pollfds[i].fd);
              // Fall-through to continue processing pollfds[] vector.
            } else {  // if (peer == from_peers.end()) {
              // Slurp up the waiting data, and initialize (if not yet).
              tcp_event_read(conf_info, peer);
              // tcp_event_read(conf_info, &(*peer));  // note, iterator hack

              if (!peer->IsIncomingMsgInitialized())
                peer->InitIncomingMsg();
              if (error.Event()) {
                logger.Log(LOG_ERR, "main(POLLIN): "
                           "unable to read incoming data from %s: %s.",
                           peer->print().c_str(), error.print().c_str());
                from_peers.erase(peer);  // threading should not have occured
                error.clear();
              }
            }  // else (peer == from_peers.end()) {
#if DEBUG_MUTEX_LOCK
            warnx("main(POLLIN): releasing from_peers lock.");
#endif
            pthread_mutex_unlock(&from_peers_mtx);
          }  // else (peer != to_peers.end()) {

          // Interestingly, from this point on we can report all ERRORS
          // *remotely* via NACKs, as we *know* we can read correctly read the
          // framing protocol over the socket.  Moreover, we'll be able to
          // read an EOF, *if* the remote end decides to shutdown the
          // connection after receiving our NACK + error message.

        } catch (...) {
          logger.Log(LOG_ERR, "main(): "
                     "Unexpected exception thrown during POLLIN proccessing.");
        }
      }  // if (pollfds[i].revents & POLLIN) {

      // ... then check POLLOUT.
      if (pollfds[i].revents & POLLOUT) {
        try {  // t&c for debugging
          // Check to_peers for our fd (first grab lock!) ...
#if DEBUG_MUTEX_LOCK
          warnx("main(POLLOUT): requesting to_peers lock.");
#endif
          pthread_mutex_lock(&to_peers_mtx);
          list<TCPSession>::iterator peer =
              tcp_event_poll_get_peer(conf_info, pollfds[i].fd, &to_peers);
          if (peer != to_peers.end()) {
            // Write a chunk of data out the waiting socket.
            tcp_event_write(conf_info, peer);
            if (error.Event()) {
              logger.Log(LOG_ERR, "main(POLLOUT): "
                         "Unable to write data to %s: %s.",
                         peer->print().c_str(), error.print().c_str());
              to_peers.erase(peer);  // no threading on to_peers
              error.clear();
            } else if (peer->IsOutgoingMsgSent()) {
              peer->PopOutgoingMsgQueue();  // clean up first pending message
              if (error.Event()) {
                logger.Log(LOG_ERR, "main(POLLOUT): "
                           "Failed to clear outgoing msg queue to %s: %s", 
                           peer->print().c_str(), error.print().c_str());
                to_peers.erase(peer);  // no threading on to_peers
                error.clear();
              }
            }

            // Since we have to release our to_peers lock prior to
            // grabbing our from_peers lock, we also need to release it
            // here, in this equivalent nested branch.

#if DEBUG_MUTEX_LOCK
            warnx("main(POLLOUT): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);
          } else {  // if (peer != to_peers.end()) {
            // Grab from_peers lock (first, relase to_peers lock).
#if DEBUG_MUTEX_LOCK
            warnx("main(POLLOUT): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);

#if DEBUG_MUTEX_LOCK
            warnx("main(POLLOUT): requesting from_peers lock.");
#endif
            pthread_mutex_lock(&from_peers_mtx);
            peer = 
                tcp_event_poll_get_peer(conf_info, pollfds[i].fd, &from_peers);
            if (peer == from_peers.end()) {
              logger.Log(LOG_ERR, "main(POLLOUT): "
                         "Unable to find peer for file descriptor: %d",
                         pollfds[i].fd);
              // Fall-through to continue processing pollfds[] vector.
            } else {  // if (peer == from_peers.end()) {
              // Write a chunk of data out the waiting socket.
              tcp_event_write(conf_info, peer);
              if (error.Event()) {
                logger.Log(LOG_ERR, "main(POLLOUT): "
                           "Unable to write data to %s: %s.",
                           peer->print().c_str(), error.print().c_str());
                if (peer->rtid() == TCPSESSION_THREAD_NULL)
                  from_peers.erase(peer);
                error.clear();
              } else if (peer->IsOutgoingMsgSent()) {
                peer->PopOutgoingMsgQueue();  // clean up first pending message
                if (error.Event()) {
                  logger.Log(LOG_ERR, "main(POLLOUT): "
                             "Failed to clear outgoing msg queue to %s: %s", 
                             peer->print().c_str(), error.print().c_str());
                  if (peer->rtid() == TCPSESSION_THREAD_NULL)
                    from_peers.erase(peer);
                  error.clear();
                }
              }
            }  // else (peer == from_peers.end()) {
#if DEBUG_MUTEX_LOCK
            warnx("main(POLLOUT): releasing from_peers lock.");
#endif
            pthread_mutex_unlock(&from_peers_mtx);
          }  // else (peer != to_peers.end()) {
        } catch (...) {
          logger.Log(LOG_ERR, "main(): "
                     "Unexpected exception thrown during POLLOUT proccessing.");
        }
      }  // if (pollfds[i].revents & POLLOUT) {

      // ERROR conditions ...
      if (pollfds[i].revents & POLLERR) {
        try {  // t&c for debugging
          // Check to_peers for our fd (first grab lock!) ...
#if DEBUG_MUTEX_LOCK
          warnx("main(): requesting to_peers lock.");
#endif
          pthread_mutex_lock(&to_peers_mtx);
          list<TCPSession>::iterator peer =
              tcp_event_poll_get_peer(conf_info, pollfds[i].fd, &to_peers);
          if (peer != to_peers.end()) {
            // Report the error and remove the peer.
            logger.Log(LOG_ERR, "main(): "
                       "Peer (%s) returned POLLERR on socket %d.",
                       peer->print().c_str(), pollfds[i].fd);
            to_peers.erase(peer);  // to_peers is not multi-threaded

            // Since we have to release our to_peers lock prior to
            // grabbing our from_peers lock, we also need to release it
            // here, in this equivalent nested branch.

#if DEBUG_MUTEX_LOCK
            warnx("main(): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);
          } else {  // if (peer != to_peers.end()) {
            // Grab from_peers lock (first, relase to_peers lock).
#if DEBUG_MUTEX_LOCK
            warnx("main(): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);

#if DEBUG_MUTEX_LOCK
            warnx("main(): requesting from_peers lock.");
#endif
            pthread_mutex_lock(&from_peers_mtx);
            peer = 
                tcp_event_poll_get_peer(conf_info, pollfds[i].fd, &from_peers);
            if (peer == from_peers.end()) {
              logger.Log(LOG_ERR, "main(): "
                         "Unable to find peer for file descriptor: %d",
                         pollfds[i].fd);
              // Fall-through to continue processing pollfds[] vector.
            } else {  // if (peer == from_peers.end()) {
              // Report the error and remove the peer.
              logger.Log(LOG_ERR, "main(): "
                         "Peer (%s) returned POLLERR on socket %d.",
                         peer->print().c_str(), pollfds[i].fd);
              if (peer->rtid() == TCPSESSION_THREAD_NULL)
                from_peers.erase(peer);
            }  // else (peer == from_peers.end()) {
#if DEBUG_MUTEX_LOCK
            warnx("main(): releasing from_peers lock.");
#endif
            pthread_mutex_unlock(&from_peers_mtx);
          }  // else (peer != to_peers.end()) {
        } catch (...) {
          logger.Log(LOG_ERR, "main(): "
                     "Unexpected exception thrown during POLLERR proccessing.");
        }
      }  // if (pollfds[i].revents & POLLERR) {
				
      if (pollfds[i].revents & POLLHUP) {
        try {  // t&c for debugging
          // Check to_peers for our fd (first grab lock!) ...
#if DEBUG_MUTEX_LOCK
          warnx("main(): requesting to_peers lock.");
#endif
          pthread_mutex_lock(&to_peers_mtx);
          list<TCPSession>::iterator peer =
              tcp_event_poll_get_peer(conf_info, pollfds[i].fd, &to_peers);
          if (peer != to_peers.end()) {
            // If peer is still connected, report the genuine error.
            if (peer->IsConnected()) {
              logger.Log(LOG_ERR, "main(): "
                         "Peer (%s) returned POLLHUP on socket %d.",
                         peer->print().c_str(), pollfds[i].fd);
              to_peers.erase(peer);  // to_peers is not multi-threaded
            }

            // Since we have to release our to_peers lock prior to
            // grabbing our from_peers lock, we also need to release it
            // here, in this equivalent nested branch.

#if DEBUG_MUTEX_LOCK
            warnx("main(): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);
          } else {  // if (peer != to_peers.end()) {
            // Grab from_peers lock (first, relase to_peers lock).
#if DEBUG_MUTEX_LOCK
            warnx("main(): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);

#if DEBUG_MUTEX_LOCK
            warnx("main(): requesting from_peers lock.");
#endif
            pthread_mutex_lock(&from_peers_mtx);
            peer = 
                tcp_event_poll_get_peer(conf_info, pollfds[i].fd, &from_peers);
            if (peer == from_peers.end()) {
              logger.Log(LOG_ERR, "main(): "
                         "Unable to find peer for file descriptor: %d",
                         pollfds[i].fd);
              // Fall-through to continue processing pollfds[] vector.
            } else {  // if (peer == from_peers.end()) {
              // If peer is still connected, report the genuine error.
              if (peer->IsConnected()) {
                logger.Log(LOG_ERR, "main(): "
                           "Peer (%s) returned POLLHUP on socket %d.",
                           peer->print().c_str(), pollfds[i].fd);
                if (peer->rtid() == TCPSESSION_THREAD_NULL)
                  from_peers.erase(peer);
              }
            }  // else (peer == from_peers.end()) {
#if DEBUG_MUTEX_LOCK
            warnx("main(): releasing from_peers lock.");
#endif
            pthread_mutex_unlock(&from_peers_mtx);
          }  // else (peer != to_peers.end()) {

        } catch (...) {
          logger.Log(LOG_ERR, "main(): "
                     "Unexpected exception thrown during POLLHUP proccessing.");
        }
      }  // if (pollfds[i].revents & POLLHUP) {
				
      if (pollfds[i].revents & POLLNVAL) {
        try {  // t&c for debugging
          // Check to_peers for our fd (first grab lock!) ...
#if DEBUG_MUTEX_LOCK
          warnx("main(): requesting to_peers lock.");
#endif
          pthread_mutex_lock(&to_peers_mtx);
          list<TCPSession>::iterator peer =
              tcp_event_poll_get_peer(conf_info, pollfds[i].fd, &to_peers);
          if (peer != to_peers.end()) {
            // Report the error and remove the peer.
            logger.Log(LOG_ERR, "main(): "
                       "Peer (%s) returned POLLNVAL on socket %d.",
                       peer->print().c_str(), pollfds[i].fd);
            to_peers.erase(peer);  // to_peers is not multi-threaded

            // Since we have to release our to_peers lock prior to
            // grabbing our from_peers lock, we also need to release it
            // here, in this equivalent nested branch.

#if DEBUG_MUTEX_LOCK
            warnx("main(): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);
          } else {  // if (peer != to_peers.end()) {
            // Grab from_peers lock (first, relase to_peers lock).
#if DEBUG_MUTEX_LOCK
            warnx("main(): releasing to_peers lock.");
#endif
            pthread_mutex_unlock(&to_peers_mtx);

#if DEBUG_MUTEX_LOCK
            warnx("main(): requesting from_peers lock.");
#endif
            pthread_mutex_lock(&from_peers_mtx);
            peer = 
                tcp_event_poll_get_peer(conf_info, pollfds[i].fd, &from_peers);
            if (peer == from_peers.end()) {
              logger.Log(LOG_ERR, "main(): "
                         "Unable to find peer for file descriptor: %d",
                         pollfds[i].fd);
              // Fall-through to continue processing pollfds[] vector.
            } else {  // if (peer == from_peers.end()) {
              // Report the error and remove the peer.
              logger.Log(LOG_ERR, "main(): "
                         "Peer (%s) returned POLLNVAL on socket %d.",
                         peer->print().c_str(), pollfds[i].fd);
              if (peer->rtid() == TCPSESSION_THREAD_NULL)
                from_peers.erase(peer);
            }  // else (peer == from_peers.end()) {
#if DEBUG_MUTEX_LOCK
            warnx("main(): releasing from_peers lock.");
#endif
            pthread_mutex_unlock(&from_peers_mtx);
          }  // else (peer != to_peers.end()) {
        } catch (...) {
          logger.Log(LOG_ERR, "main(): "
                     "Unexpected exception thrown during POLLNVAL proccessing.");
        }
      }  // if (pollfds[i].revents & POLLNVAL) {

      // Check for *other* events ...
      if (pollfds[i].revents & POLLPRI ||
          pollfds[i].revents & POLLRDNORM ||
          pollfds[i].revents & POLLRDBAND ||
          pollfds[i].revents & POLLWRBAND) {
        logger.Log(LOG_WARN, "Received event: %d, on fd: %d, "
                   "unable to process, continuing ...",
                   pollfds[i].revents, pollfds[i].fd);
      }

      // We processed something, bump counters.
      i++;
      n--;
    }  // while (n > 0)

    // And finally, after processing *all* the ready fds, check 
    // for signals.

#if 0
    // TODO(aka) Add signal checking!
    event_signal_check();
#endif
  }  // while (1)

  // Clean up.

  return 0;
}
