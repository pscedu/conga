/* $Id: server-funcs.h,v 1.4 2014/05/21 15:19:42 akadams Exp $ */

// Server helper functions.

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#ifndef SERVER_FUNCS_H_
#define SERVER_FUNCS_H_

#include <string>
using namespace std;

#include "ConfInfo.h"
#include "TCPSession.h"
#include "SwitchInfo.h"
#include "FlowInfo.h"

// CONGAd.
#define SERVER_VERSION "0.9.3"
#define CONF_FILE_DELIMITER '='

// Networking definitions.

// Global defaults.

// Function declarations.
int parse_command_line(int argc, char* argv[], ConfInfo* info);
void parse_conf_file(ConfInfo* info);
void usage(void);
void initiate_stats_meter_request(const ConfInfo& info, const string& dpid,
                                  list<TCPSession>* to_peers,
                                  pthread_mutex_t* to_peers_mtx);
void initiate_stats_meterconfig_request(const ConfInfo& info, const string& dpid,
                                        list<TCPSession>* to_peers,
                                        pthread_mutex_t* to_peers_mtx);
void initiate_stats_flow_request(const ConfInfo& info, const string& dpid,
                                 const string& new_dst,
                                 const SwitchInfo& controller, 
                                 list<TCPSession>* to_peers,
                                 pthread_mutex_t* to_peers_mtx);
void initiate_post_allocation_response(const ConfInfo& info, 
                                       const FlowInfo& flow,
                                       const string& err_msg,
                                       list<TCPSession>* from_peers,
                                       pthread_mutex_t* from_peers_mtx);

#endif  /* #ifndef SERVER_FUNCS_H_ */
