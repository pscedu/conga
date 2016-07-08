/* $Id: conga-procs.h,v 1.9 2014/05/21 15:19:42 akadams Exp $ */

// conga-procs: routines for processing messages in our CONGA.

// Copyright © 2010, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#ifndef CONGA_PROCS_H_
#define CONGA_PROCS_H_

#include <sysexits.h>

#include <vector>
#include <string>
#include <list>
using namespace std;

#include "TCPSession.h"
#include "ConfInfo.h"
#include "MeterInfo.h"
#include "AuthInfo.h"
#include "FlowInfo.h"

#define CONGA_SERVER_PORT 443  // https

struct conga_incoming_msg_args {
  ConfInfo* info;
  SSLContext* ssl_context;
  //const string& dpid;
  list<MeterInfo>* sdn_state;
  pthread_mutex_t* sdn_state_mtx;
  list<AuthInfo>* authenticators;
  pthread_mutex_t* authenticators_mtx;
  list<FlowInfo>* flows;
  pthread_mutex_t* flow_list_mtx;
  list<TCPSession>* to_peers;
  pthread_mutex_t* to_peers_mtx;
  list<TCPSession>::iterator peer;
  list<TCPSession>::iterator peer_end;
  vector<pthread_t>* thread_list;
  pthread_mutex_t* thread_list_mtx;
};

const char kCONGAMsgDelimiter = ':';

bool conga_process_incoming_msg(ConfInfo* info, SSLContext* ssl_context, 
                                //const string& dpid,
                                list<MeterInfo>* sdn_state,
                                pthread_mutex_t* sdn_state_mtx,
                                list<AuthInfo>* api_keys, 
                                pthread_mutex_t* api_keys_mtx,
                                list<FlowInfo>* flows, 
                                pthread_mutex_t* flow_list_mtx,
                                list<TCPSession>* to_peers,
                                pthread_mutex_t* to_peers_mtx, 
                                list<TCPSession>::iterator peer);
void* conga_concurrent_process_incoming_msg(void* args);
void conga_process_response(const ConfInfo& info, const MsgHdr& msg_hdr,
                            const string& msg_body, const File& msg_data,
                            //XXX const string& dpid,
                            list<MeterInfo>* sdn_state,
                            pthread_mutex_t* sdn_state_mtx,
                            list<FlowInfo>* flows, 
                            pthread_mutex_t* flow_list_mtx,
                            list<TCPSession>::iterator peer, 
                            list<MsgHdr>::iterator req_hdr);
string conga_process_post_auth(const ConfInfo& info, const HTTPFraming& http_hdr,
                               const string& msg_body, const File& msg_data,
                               SSLContext* ssl_context, 
                               list<AuthInfo>* api_keys, 
                               pthread_mutex_t* api_keys_mtx,
                               list<TCPSession>::iterator peer);
string conga_process_delete_auth(const ConfInfo& info,
                                 const HTTPFraming& http_hdr,
                                 const string& msg_body, const File& msg_data,
                                 list<AuthInfo>* api_keys, 
                                 pthread_mutex_t* api_keys_mtx,
                                 list<TCPSession>::iterator peer);
string conga_process_get_auth(const ConfInfo& info, const HTTPFraming& http_hdr,
                              const string& msg_body, const File& msg_data,
                              list<AuthInfo>* api_keys, 
                              pthread_mutex_t* api_keys_mtx,
                              list<TCPSession>::iterator peer);
string conga_process_post_allocations(const ConfInfo& info,
                                      const HTTPFraming& http_hdr,
                                      const string& msg_body,
                                      const File& msg_data,
                                      list<AuthInfo>* api_keys, 
                                      pthread_mutex_t* api_keys_mtx,
                                      list<FlowInfo>* flows,
                                      pthread_mutex_t* flow_list_mtx,
                                      list<TCPSession>::iterator peer);
string conga_process_delete_allocations(const ConfInfo& info, 
                                        const HTTPFraming& http_hdr,
                                        const string& msg_body,
                                        const File& msg_data,
                                        list<AuthInfo>* api_keys, 
                                        pthread_mutex_t* api_keys_mtx,
                                        list<FlowInfo>* flows,
                                        pthread_mutex_t* flow_list_mtx,
                                        list<TCPSession>::iterator peer);
string conga_process_get_allocations(const ConfInfo& info, 
                                     const HTTPFraming& http_hdr,
                                     const string& msg_body,
                                     const File& msg_data,
                                     list<AuthInfo>* api_keys, 
                                     pthread_mutex_t* api_keys_mtx,
                                     list<FlowInfo>* flows,
                                     pthread_mutex_t* flow_list_mtx,
                                     list<TCPSession>::iterator peer);

void conga_gen_http_error_response(const ConfInfo& info,
                                   const HTTPFraming& http_hdr, 
                                   list<TCPSession>::iterator peer);
void conga_gen_http_response(const ConfInfo& info,
                             const HTTPFraming& http_hdr, const string msg,
                             list<TCPSession>::iterator peer);

#if 0  // Deprecated.
void conga_process_text_plain_request(const ConfInfo& info, 
                                   const HTTPFraming& http_hdr,
                                   const string& msg_body, const File& msg_data,
                                   RequestInfo* meta_data);  // ErrorHandler
void conga_process_text_xml_request(const ConfInfo& info, 
                                   const HTTPFraming& http_hdr,
                                   const string& msg_body, const File& msg_data,
                                   RequestInfo* meta_data);  // ErrorHandler
void conga_parse_xml(const ConfInfo& info, const string& msg_body,
                    RequestInfo* meta_data);  // ErrorHandler
void conga_process_request(const ConfInfo& info, 
                          RequestInfo* meta_data);  // ErrorHandler
void conga_gen_wsdl_response(const ConfInfo& info, const RequestInfo& meta_data, 
                            const HTTPFraming& http_hdr, 
                            list<TCPSession>::iterator peer);
#endif

#endif  /* #ifndef CONGA_PROCS_H_ */
