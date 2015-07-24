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
#include "SSLContext.h"
#include "SSLConn.h"
#include "SSLSession.h"
#include "ssl-event-procs.h"
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


// Main event-loop routine: The CONGA server loads & processes
// some configuration variables and then enters its main event-loop
// waiting on (poll(2)) either for an external initiated connection or
// an internal event signaling that work needs to be done.

int main(int argc, char* argv[]) {
  ConfInfo conf_info;           // configuration information
  list<FlowInfo> flows;         // flows currently active
  pthread_mutex_t flow_list_mtx;
  list<SSLSession> to_peers;    // initated connections (to other nodes)
  //pthread_mutex_t to_peers_mtx = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_t to_peers_mtx;
  list<SSLSession> from_peers;  // received connections (from accept(2))
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
  /*
  conf_info.database_ = "127.0.0.1";
  conf_info.database_port_ = 3306;
  conf_info.database_db_ = "conga_data";
  conf_info.database_user_ = "conga";
  */

  logger.set_proc_name("conga");

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
  ssl_context.Init(TLSv1_method(), server_id,
                   "limbo.psc.edu.key.pem", "/home/pscnoc/conga/certs", SSL_FILETYPE_PEM, NULL, 
                   "limbo.psc.edu.crt.pem", "/home/pscnoc/conga/certs", SSL_FILETYPE_PEM, 
                   SSL_VERIFY_NONE, 2, SSL_SESS_CACHE_OFF, SSL_OP_CIPHER_SERVER_PREFERENCE);
#if 0  // XXX Additional ssl context suff that should be in Init()
  ssl_context.Load_CA_List(conf_info.Base_Path(), SAMI_CA_CERTS_DIR);
  String egd_path = conf_info.Base_Path() + "tmp/entropy";
  // FUTURE: Change rand file & dh parm to a path like egd path!
  ssl_context.Seed_PRNG(egd_path, conf_info.Base_Path(), "random.pem", "rand");
  ssl_context.Load_DH_Ephemeral_Keys(conf_info.Base_Path(), "dhparam-1024.pem", "rand");
#endif
  if (error.Event())
    errx(EXIT_FAILURE, "%s", error.print().c_str());

  // Daemonize program.

  // TODO(aka) Need to add daemonizing stuff.

  // Initialze more globals & externals: PRNG, signals
  srandom(time(NULL));  // TODO(aka) We really need to use srandomdev() here!

#if 0
  sig_handler.Init();  // TODO(aka) need to add signal handling routines
#endif

  if (!conf_info.v4_enabled_ && !conf_info.v6_enabled_)
    conf_info.v4_enabled_ = true;  // give us something

  // Setup the server's listen socket(s).
  if (conf_info.v4_enabled_) {
    SSLConn tmp_server;
    tmp_server.InitServer(ssl_context, AF_INET);  // listen on all IPv4 interfaces
    tmp_server.set_blocking();
    tmp_server.set_close_on_exec();
    tmp_server.Socket(PF_INET, SOCK_STREAM, 0);
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
    tmp_server.InitServer(ssl_context, AF_INET6);  // listen on all interfaces ...
    tmp_server.set_blocking();
    tmp_server.set_close_on_exec();
    tmp_server.Socket(PF_INET6, SOCK_STREAM, 0);
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

  logger.Log(LOG_NOTICE, "conga (%s) starting on port %hu.", SERVER_VERSION, conf_info.port_);

  // Any work prior to the main event loop?
  time_t state_poll_timeout = time(NULL);

  // Main event-loop.
  struct pollfd pollfds[SSL_EVENT_MAX_FDS];
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
    nfds = ssl_event_poll_init(to_peers, from_peers, SSL_EVENT_MAX_FDS, 
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
        if (state_poll_timeout <= now) {
#if 0
          const string ryu_host = "ryu.psc.edu";
          SSLSession tmp_session(MsgInfo::HTTP);
          tmp_session.Init();  // set aside buffer space
          tmp_session.SSLConn::Init(ssl_context, ryu_host.c_str(), AF_INET, 
                                    IPCOMM_DNS_RETRY_CNT);  // init IPComm base class
          tmp_session.set_port(443);
          tmp_session.set_blocking();
          tmp_session.Socket(PF_INET, SOCK_STREAM, 0);
          //tmp_session.set_handle(tmp_session.fd());  // for now, set it to the socket
          if (error.Event()) {
            logger.Log(LOG_ERR, "Failed to initialize peer %s:443: %s", 
                       ryu_host.c_str(), error.print().c_str());
            error.clear();
            
            state_poll_timeout += state_poll_interval;
            continue;
          }

          // Build a (HTTP) framing header and load the framing header into
          // our SSLSession's MsgHdr list.

          URL ryu_url;
          ryu_url.Init("https", ryu_host.c_str(), 443, NULL, 0, NULL);
          HTTPFraming http_hdr;
          http_hdr.InitRequest(HTTPFraming::GET, ryu_url);

          // Add HTTP content-length message-headers (for an empty message-body).
          struct rfc822_msg_hdr mime_msg_hdr;
          mime_msg_hdr.field_name = MIME_CONTENT_LENGTH;
          mime_msg_hdr.field_value = "0";
          http_hdr.AppendMsgHdr(mime_msg_hdr);

          logger.Log(LOG_DEBUGGING, "main(): Generated HTTP headers:\n%s", http_hdr.print_hdr(0).c_str());

          MsgHdr tmp_msg_hdr(MsgHdr::TYPE_HTTP);
          tmp_msg_hdr.Init(++msg_id_hash, http_hdr);

          // Add our REQUEST message to our outgoing TCPSession list, and go
          // back to wait for transmission in the event-loop.

          tmp_session.AddMsgBuf(http_hdr.print_hdr(0).c_str(), http_hdr.hdr_len(), 
                                msg.c_str(), strlen(msg.c_str()), tmp_msg_hdr);

          logger.Log(LOG_VERBOSE, "Sending HTTP REQUEST \'%s\n%s\' to %s.", 
                     http_hdr.print_start_line().c_str(), 
                     http_hdr.print_msg_hdrs().c_str(), 
                     tmp_session.SSLConn::print().c_str());

#if DEBUG_MUTEX_LOCK
          warnx("main(timeout): requesting to_peers lock.");
#endif
          pthread_mutex_lock(&to_peers_mtx);
          to_peers.push_back(tmp_session);
#if DEBUG_MUTEX_LOCK
          warnx("main(timeout): releasing to_peers lock.");
#endif
          pthread_mutex_unlock(&to_peers_mtx);
        }
#endif
        state_poll_timeout += state_poll_interval;
        }  // if (state_poll_timeout <= now) {

        // If any of our *read* data in from_peers is complete, process it ...
#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): requesting from_peers lock.");
#endif
        pthread_mutex_lock(&from_peers_mtx);  // TODO(aka) what if not threaded?

        list<SSLSession>::iterator from_peer = from_peers.begin();
        while (from_peer != from_peers.end()) {
          // If it's an entire message (according to the headers), process it.
          if (from_peer->IsIncomingMsgComplete()) {
            if (from_peer->IsSynchroniationEnabled()) {
              // Note, synchronization makes sure the initiator of a
              // SSLSession uses to_peers, while the receiver uses
              // from_peers for the session.  Since (i) HTTP framing
              // doesn't need this, and (ii) we'd need to lock
              // to_peers, as well, we're making it a ERROR in here.

              /*
              // TOOD(aka) Remove lamport_ from ConfInfo, then we
              // don't need to pass ConfInfo as a ptr!

              ssl_event_synchronize_connection(true, &conf_info, &to_peers,
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
                &conf_info, ssl_context, &flows, &flow_list_mtx,
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
              conga_process_incoming_msg(&conf_info, ssl_context, &flows, &flow_list_mtx,
                                         &to_peers, &to_peers, from_peer);
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
               !from_peer->IsIncomingDataInitialized())) {
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
          if (!from_peer->IsIncomingDataInitialized() && 
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

        // If any of our *read* data in to_peers is complete, process it ...
#if DEBUG_MUTEX_LOCK
        warnx("main(timeout): requesting to_peers lock.");
#endif
        pthread_mutex_lock(&to_peers_mtx);
        list<SSLSession>::iterator to_peer = to_peers.begin();
        while (to_peer != to_peers.end()) {
          // If it's an entire message (according to the headers), process it.
          if (to_peer->IsIncomingMsgComplete()) {
            if (to_peer->IsSynchroniationEnabled()) {
              // Note, synchronization makes sure the initiator of
              // a SSLSession uses to_peers, while the receiver uses
              // from_peers for the session.  Since (i) HTTP framing
              // doesn't need this, and (ii) we'd need to lock
              // to_peers, as well, we're making it a ERROR in here.

              /*
              // TOOD(aka) Remove lamport_ from ConfInfo, then we
              // don't need to pass ConfInfo as a ptr!

              ssl_event_synchronize_connection(true, &conf_info, &to_peers,
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
            conga_process_incoming_msg(&conf_info, to_peer);
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

          // Finally, see if this peer should be removed.
          if (!to_peer->IsConnected() && 
              !to_peer->IsOutgoingDataPending() &&
              !to_peer->rbuf_len() && 
              !to_peer->IsIncomingDataInitialized()) {
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
                to_peer = to_peers.erase(from_peer);
              }
            } else {
              logger.Log(LOG_DEBUG, "main(timeout): Removing peer %s.",
                         to_peer->print().c_str());
              to_peer = to_peers.erase(from_peer);
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
          if (!to_peer->IsIncomingDataInitialized() && 
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
          // all errors internally in ssl_event_accept().

          ssl_event_accept(conf_info, servers[j], kMaxPeers, kFramingType,
                           ssl_context, &from_peers);
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
      if ((i = ssl_event_poll_status(pollfds, nfds, i)) < 0) {
        logger.Log(LOG_ERROR, "ssl_event_poll_status() failed, "
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
          list<SSLSession>::iterator peer =
              ssl_event_poll_get_peer(conf_info, pollfds[i].fd, &to_peers);
          if (peer != to_peers.end()) {
            // Slurp up the waiting data, and initialize (if not yet).
            ssl_event_read(conf_info, peer);
            // ssl_event_read(conf_info, &(*peer));  // note, iterator hack

            if (!peer->IsIncomingDataInitialized()) {
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
                ssl_event_poll_get_peer(conf_info, pollfds[i].fd, &from_peers);
            if (peer == from_peers.end()) {
              logger.Log(LOG_ERR, "main(POLLIN): "
                         "Unable to find peer for file descriptor: %d",
                         pollfds[i].fd);
              // Fall-through to continue processing pollfds[] vector.
            } else {  // if (peer == from_peers.end()) {
              // Slurp up the waiting data, and initialize (if not yet).
              ssl_event_read(conf_info, peer);
              // ssl_event_read(conf_info, &(*peer));  // note, iterator hack

              if (!peer->IsIncomingDataInitialized())
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
          list<SSLSession>::iterator peer =
              ssl_event_poll_get_peer(conf_info, pollfds[i].fd, &to_peers);
          if (peer != to_peers.end()) {
            // Write a chunk of data out the waiting socket.
            ssl_event_write(conf_info, peer);
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
                ssl_event_poll_get_peer(conf_info, pollfds[i].fd, &from_peers);
            if (peer == from_peers.end()) {
              logger.Log(LOG_ERR, "main(POLLOUT): "
                         "Unable to find peer for file descriptor: %d",
                         pollfds[i].fd);
              // Fall-through to continue processing pollfds[] vector.
            } else {  // if (peer == from_peers.end()) {
              // Write a chunk of data out the waiting socket.
              ssl_event_write(conf_info, peer);
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
          list<SSLSession>::iterator peer =
              ssl_event_poll_get_peer(conf_info, pollfds[i].fd, &to_peers);
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
                ssl_event_poll_get_peer(conf_info, pollfds[i].fd, &from_peers);
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
          list<SSLSession>::iterator peer =
              ssl_event_poll_get_peer(conf_info, pollfds[i].fd, &to_peers);
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
                ssl_event_poll_get_peer(conf_info, pollfds[i].fd, &from_peers);
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
          list<SSLSession>::iterator peer =
              ssl_event_poll_get_peer(conf_info, pollfds[i].fd, &to_peers);
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
                ssl_event_poll_get_peer(conf_info, pollfds[i].fd, &from_peers);
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
