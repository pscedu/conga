/* $Id: conga-procs.cc,v 1.35 2014/05/21 15:19:42 akadams Exp $ */

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <ctime>
#include <err.h>
#include <fcntl.h>
#include <math.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>      // for lower-casing std::string TODO(aka) Why do I need this again?
using namespace std;

/*
#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/dom/DOMDocument.hpp>
#include <xercesc/dom/DOM.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/XMLUni.hpp>
#include <xercesc/util/OutOfMemoryException.hpp>
using namespace xercesc;
*/

#include "ErrorHandler.h"
#include "Logger.h"
#include "File.h"
#include "URL.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"

#include "defines.h"       // TODO(aka) not sure if we need it anymore ...

#include "conga-procs.h"


#define DEBUG_NETWORKING 0
#define DEBUG_XML 0
#define DEBUG_MUTEX_LOCK 0

static const char* kServiceAllocations = "allocations";
static const char* kServiceAuth = "auth";
static const char* kServiceDancesAuth = "dances/api/v1";

static const char* kDetailAPIKey = "api_key";
static const char* kDetailUserID = "user_id";
static const char* kDetailProjectID = "project_id";
static const char* kDetailResourceID = "resource_id";
static const char* kDetailAllocationID = "allocation_id";
//static const char* kDetailRequestID = "request_id";
//static const char* kDetailExpiresIn = "expires_in";
//static const char* kDetailServices = "services";
static const char* kDetailState = "state";
static const char* kDetailSrcIP = "src_ip";
static const char* kDetailSrcPort = "src_port";
static const char* kDetailDstIP = "dst_ip";
static const char* kDetailDstPort = "dst_port";
//static const char* kDetailDataSize = "data_size";
static const char* kDetailStartTime = "start_time";
static const char* kDetailEndTime = "end_time";
static const char* kDetailDuration = "duration";
static const char* kDetailRate = "rate";

static const char* kDetailIsActive = "is_active";

static const size_t kAPIKeySize = 16;

// RYU elements.
static const char* kNameActions = "actions";
static const char* kNameMeterID = "meter_id";
static const char* kNameFlowCount = "flow_count";
static const char* kNameBandStats = "band_stats";
static const char* kNameBands = "bands";
static const char* kNameFlags = "flags";
static const char* kNameRate = "rate";
static const char* kNameByteBandCount = "byte_band_count";
static const char* kNameByteInCount = "byte_in_count";
static const char* kNameMatch = "match";
static const char* kNameNwDst = "nw_dst";

static const char* kRyuControllerName = "tango.psc.edu";
static const in_port_t kRyuControllerPort = 8080;
static const char* kRyuQueryStats = "stats";
static const char* kRyuQueryFlowentry = "flowentry";
static const char* kRyuQueryModify = "modify";

// Routine to process a ready (incoming) message in our TCPSession
// object.  This routine must deal with both the message framing *and*
// the application (to know what routines to call for message
// processing).
//
// This routine can set an ErrorHandler event.
bool conga_process_incoming_msg(ConfInfo* info, SSLContext* ssl_context, 
                                //const string& dpid,
                                list<MeterInfo>* sdn_state,
                                pthread_mutex_t* sdn_state_mtx,
                                list<AuthInfo>* authenticators, 
                                pthread_mutex_t* authenticators_mtx,
                                list<FlowInfo>* flows,
                                pthread_mutex_t* flow_list_mtx,
                                list<TCPSession>* to_peers,
                                pthread_mutex_t* to_peers_mtx, 
                                list<TCPSession>::iterator peer) {
  if (&(*peer) == NULL) {  // note iterator hack
    error.Init(EX_SOFTWARE, "conga_process_incoming_msg(): peer is NULL");
    return false;
  }

  try {  // for debugging

    //logger.Log(LOG_DEBUG, "conga_process_incoming_msg(): Working with header: %s.", peer->rhdr().print().c_str());

    // First, make a copy of the incoming msg and remove the *original*
    // data from the TCPSession (i.e., either rbuf_ or rfile_ (along
    // with rhdr_)).  We *trade-off* the cost of the buffer copy in-order
    // for us to multi-thread different messages within the same TCPSession,
    // i.e., we need to clear out the incoming message ASAP!

    const MsgHdr msg_hdr = peer->rhdr();
    string msg_body;
    File msg_data;
    if (peer->IsIncomingDataStreaming())
      msg_data = peer->rfile();  // TODO(aka) I doubt this will ever happen ...
    else
      msg_body.assign(peer->rbuf(), peer->rhdr().body_len());

    //logger.Log(LOG_DEBUG, "conga_process_incoming_msg(): Cleaning %ld byte msg-body for request (%s)/response (%s) from peer %s.", peer->rhdr().msg_len(), req_hdr->print_hdr(0).c_str(), peer->rhdr().print_hdr(0).c_str(), peer->print().c_str());

    peer->ClearIncomingMsg();  // remove *now copied* message from peer

    // See what type of message this is; if this is a REQUEST, call the
    // appropriate process_request_msg() for our application.  If,
    // however, this is a RESPONSE, then we additionally need to find
    // its associated REQUEST message-header (in peer->whdrs) to
    // correctly process the message.

    int response_flag = 0;
    switch (msg_hdr.type()) {
      case MsgHdr::TYPE_BASIC :
        if (msg_hdr.basic_hdr().type > MSG_REQ_FILE)
          response_flag++;  // all msgs > REQ_FILE must be ACKs
        break;

      case MsgHdr::TYPE_HTTP :
        if (msg_hdr.http_hdr().msg_type() == HTTPFraming::RESPONSE)
          response_flag++;
        break;

      default :
        error.Init(EX_DATAERR, "conga_process_incoming_msg(): "
                   "unknown type: %d", msg_hdr.type());
        return false;  // msg in peer was already cleared up above
    }

    if (response_flag) {
      // Presumably, we are a client, and not the server ...

      // Find REQUEST message-header in whdrs.
      list<MsgHdr> request_hdrs = peer->whdrs();  // work on a copy of the list
      list<MsgHdr>::iterator req_hdr = request_hdrs.begin(); 
      while (req_hdr != request_hdrs.end()) {
        bool found = false;
        switch (req_hdr->type()) {
          case MsgHdr::TYPE_BASIC :
            if (req_hdr->msg_id() == msg_hdr.msg_id())
              found = true;
            break;

          case MsgHdr::TYPE_HTTP :
            // TOOD(aka) Until we find a way to embed our message ids into
            // the HTTP headers, we can't compare message ids here, we just
            // assume (since HTTP is a sequential protocol) that the first
            // header whdrs_() is our request header.

            found = true;  // first time in, leave
            break;

          default :
            ;  // NOT-REACHABLE (test was already done up above)
        }

        if (found)
          break;

        req_hdr++;
      }  // while (req_hdr != request_hdrs.end()) {

      if (req_hdr != request_hdrs.end()) {
        logger.Log(LOG_DEBUG, "conga_process_incoming_msg(): "
                   "Using REQUEST message-header %s "
                   "for current message-header %s.", 
                   req_hdr->print().c_str(), msg_hdr.print().c_str());

        // Process message based on our application.
        conga_process_response(*info, msg_hdr, msg_body, msg_data,
                               sdn_state, sdn_state_mtx, flows, flow_list_mtx,
                               peer, req_hdr);

        peer->delete_whdr(req_hdr->msg_id());  // clean-up whdrs,
                                               // since we found the
                                               // hdr
      } else {
        logger.Log(LOG_ERROR, "conga_process_incoming_msg(): TODO(aka) "
                   "Unable to find our REQUEST header associated with the "
                   "received RESPONSE message-header: %s, from %s.", 
                   msg_hdr.print().c_str(), peer->print().c_str());
        // Fall-through to clean-up peer.
      }

      // Since this was a RESPONSE, if we don't have any more business
      // with this peer we can shutdown the connection.  (The TCPSession
      // will be removed in tcp_event_chk_stale_connections()).

      if (peer->rbuf_len() || peer->IsOutgoingDataPending() ||
          peer->whdrs().size()) {
        logger.Log(LOG_WARNING, "conga_process_incoming_msg(): "
                   "peer (%s) still has %ld bytes in rbuf, or "
                   "%d messages in wpending, or %d REQUEST headers left, "
                   "so not removing from queue.", 
                   peer->print().c_str(), peer->rbuf_len(), 
                   peer->IsOutgoingDataPending(), peer->whdrs().size());
      } else {
        peer->Close();  // close the connection
      }
    } else {  // if (response_flag) {
      // Process message based on our framing and application.
      switch (msg_hdr.type()) {
        case MsgHdr::TYPE_BASIC :  // not used
          break;

        case MsgHdr::TYPE_HTTP :
          {  // block protect case statement inside of case statement

            // Note, if we encounter any errors from this point forward,
            // we need to issue an HTTP ERROR RESPONSE (see
            // conga_gen_http_error_response()).

            // TODO(aka) Also, don't we need a multipart and/or
            // chunking data test here!?!

            HTTPFraming http_hdr = msg_hdr.http_hdr();
            URL url = http_hdr.uri();

            logger.Log(LOG_INFO, "Received REQUEST (%s) from %s, "
                       "content-type: %s.",
                       http_hdr.print_start_line(false).c_str(),
                       peer->hostname().c_str(), 
                       http_hdr.content_type().c_str());

            string service = url.path();
            std::transform(service.begin(), service.end(), service.begin(), 
                           ::tolower);

            // TODO(aka) Deprecated.
            // First, see if this is a WSDL service REQUEST, if so, mark it.
            //request_info.wsdl_request_ = http_hdr.IsWSDLRequest();

            string ret_msg;
            
            // Process message-body based on HTTP method & content-type.
            switch (http_hdr.method()) {
              case HTTPFraming::POST :
                {
                  if (!service.compare(0, strlen(kServiceAuth), kServiceAuth)) {
                    ret_msg = conga_process_post_auth(*info, http_hdr, 
                                                      msg_body, msg_data,
                                                      ssl_context,
                                                      authenticators,
                                                      authenticators_mtx,
                                                      peer);
                  } else if (!service.compare(0, strlen(kServiceAllocations),
                                              kServiceAllocations)) {
                    ret_msg =
                        conga_process_post_allocations(*info, http_hdr, 
                                                       msg_body, msg_data,
                                                       authenticators,
                                                       authenticators_mtx,
                                                       flows, flow_list_mtx,
                                                       peer);
                  } else {
                    // Report the error.  NACK sent outside of switch() {} block.
                    error.Init(EX_SOFTWARE, "conga_process_incoming_msg(): "
                               "No support for POST service \'%s\'",
                               service.c_str());
                  }
                }
                break;

              case HTTPFraming::DELETE :
                {
                  printf("XXX service: %s.\n", service.c_str());
                  if (!service.compare(0, strlen(kServiceAuth), kServiceAuth)) {
                    ret_msg = conga_process_delete_auth(*info, http_hdr, 
                                                        msg_body, msg_data,
                                                        authenticators,
                                                        authenticators_mtx, 
                                                        peer);
                  } else if (!service.compare(0, strlen(kServiceAllocations),
                                              kServiceAllocations)) {
                    ret_msg =
                        conga_process_delete_allocations(*info, http_hdr,
                                                         msg_body, msg_data, 
                                                         authenticators,
                                                         authenticators_mtx,
                                                         flows, flow_list_mtx,
                                                         peer);
                  } else {
                    // Report the error.  NACK sent outside of switch() {} block.
                    error.Init(EX_SOFTWARE, "conga_process_incoming_msg(): "
                               "No support for DELETE service \'%s\'",
                               service.c_str());
                  }
                }
                break;

              case HTTPFraming::GET :
                {
                  if (!service.compare(0, strlen(kServiceAuth), kServiceAuth)) {
                    ret_msg = conga_process_get_auth(*info, http_hdr, 
                                                     msg_body, msg_data,
                                                     authenticators,
                                                     authenticators_mtx, 
                                                     peer);
                  } else if (!service.compare(0, strlen(kServiceAllocations),
                                              kServiceAllocations)) {
                    ret_msg =
                        conga_process_get_allocations(*info, http_hdr,
                                                      msg_body, msg_data, 
                                                      authenticators,
                                                      authenticators_mtx,
                                                      flows, flow_list_mtx,
                                                      peer);
                  } else {
                    // Report the error.  NACK sent outside of switch() {} block.
                    error.Init(EX_SOFTWARE, "conga_process_incoming_msg(): "
                               "No support for GET service \'%s\'",
                               service.c_str());
                  }
                }
                break;

              case HTTPFraming::PUT :
                {
                  // Report the error.  NACK sent outside of switch() {} block.
                  error.Init(EX_SOFTWARE, "conga_process_incoming_msg(): "
                             "No support for PUT service \'%s\'",
                             service.c_str());
                }
                break;

              default :
                // Report the error.  NACK sent outside of switch() {} block.
                error.Init(EX_SOFTWARE, "conga_process_incoming_msg(): "
                           "unknown method: %d in REQUEST %s", 
                           http_hdr.method(), 
                           http_hdr.print_hdr(0, false).c_str());
                break;
            }  // switch (msg_hdr.basic_hdr().method()) {

            // Catch any locally generated error events (i.e.,
            // connection is healthy).

            if (error.Event()) {
              // We failed to process the REQUEST, so send our NACK
              // back.  Note, if the communication channel has since
              // somehow got corrupted, all we can do is cleanup peer
              // and wait for its removal back in the main event-loop.

              error.AppendMsg("conga_process_incoming_msg()");
              conga_gen_http_error_response(*info, http_hdr, peer);
              if (error.Event()) {
                // Report the non-NACKable error.
                error.Init(EX_SOFTWARE, "conga_process_incoming_msg(): "
                           "unable to send NACK to %s: %s",
                           peer->print().c_str(), error.print().c_str());
              }
#if 0  // Deprecated
            } else if (request_info.wsdl_request_) {
              // Build the message RESPONSE for WSDL services.
              conga_gen_wsdl_response(*info, request_info, http_hdr, peer);
#endif
            } else {
              // If ret_msg was set, process the response, else, head
              // back to the main event-loop to do more work before
              // processing the response.

              if (ret_msg.size()) {
                // Build the message RESPONSE as an HTTP message.
                conga_gen_http_response(*info, http_hdr, ret_msg, peer);

                // ... and log what we processed.
                logger.Log(LOG_NOTICE, "Processed HTTP REQUEST (%s) from %s; "
                         "is awaiting delivery.",
                         http_hdr.print_start_line(false).c_str(), 
                         peer->print().c_str());
              }
            }

            // Note, although it might seem like a good idea to delete
            // any tmp files created in making the response, reality
            // is that we are probably sending it back to requester,
            // so we need it around until *they* close the connection!

          }  // block protect for case MsgHdr::TYPE_HTTP :
          break;

        default :
          ; // NOT-REACHABLE (test was already done up above)
      }  // switch (msg_hdr.type()) {
    }  //  else (if (response_flag)) {
  } catch (...) {
    error.Init(EX_SOFTWARE, "conga_process_incoming_msg(): "
               "Unexpected exception thrown");
  }

  return true;
}

// Routine to act as a wrapper for pthread_create(), as we want to
// pass more than one argument into conga_process_incoming_msg().
void* conga_concurrent_process_incoming_msg(void* ptr) {
  pthread_detach(pthread_self());

  // Grab our function's parameters from the thread's stack and make a
  // copy of them incase they change back in the main event loop!

  // TODO(aka) Change this so that all we pass in is the index to the
  // global that holds our arguments.  Main can then *clean-up* the
  // global storage once this thread exits (by marking the boolean
  // flag in the global!  Uh, I think this was done ...

  struct conga_incoming_msg_args* args = (struct conga_incoming_msg_args*)ptr;

  // Mark that *this* thead is dealing with the next available
  // incoming message.
  
  args->peer->set_rtid(pthread_self());

  logger.Log(LOG_INFO, "Thread %lu processing REQUEST from %s.", 
             pthread_self(), args->peer->hostname().c_str());

  // Process the *complete* message.
  conga_process_incoming_msg(args->info, args->ssl_context, 
                             args->sdn_state, args->sdn_state_mtx,
                             args->authenticators, args->authenticators_mtx,
                             args->flows, args->flow_list_mtx,
                             args->to_peers, args->to_peers_mtx, args->peer);
  if (error.Event()) {
    logger.Log(LOG_ERR, "conga_concurrent_process_incoming_msg(): "
               "Thread %d failed to process REQUEST from %s: %s.", 
               pthread_self(), args->peer->hostname().c_str(), 
               error.print().c_str());
    // peer should have been cleaned in conga_process_incoming_msg()
  }

  logger.Log(LOG_INFO, "Thread %lu finished processing REQUEST from %s.", 
             pthread_self(), args->peer->hostname().c_str());

  // Clean up global thread list (to signal to main event-loop that
  // we're finished).

  args->peer->set_rtid(TCPSESSION_THREAD_NULL);

  pthread_mutex_lock(args->thread_list_mtx);
  bool found = false;
  for (vector<pthread_t>::iterator tid = args->thread_list->begin();
       tid != args->thread_list->end(); tid++) {
    if (*tid == pthread_self()) {
      found = true;
      args->thread_list->erase(tid);
      break;
    }
  }
  pthread_mutex_unlock(args->thread_list_mtx);

  if (!found)
    logger.Log(LOG_WARNING, "conga_concurrent_process_incoming_msg(): "
               "TODO(aka) Unable to find thead id (%d).", pthread_self());

  //pthread_exit();  // implicitly called when we return
  return (NULL);
}

// Routine to process a RESPONSE "message-body".
//
// TOOD(aka) We only need the HTTPFraming header in here, not MsgHdr ...
void conga_process_response(const ConfInfo& info, const MsgHdr& msg_hdr,
                            const string& msg_body, const File& msg_data,
                            // XXX const string& dpid,
                            list<MeterInfo>* sdn_state,
                            pthread_mutex_t* sdn_state_mtx,
                            list<FlowInfo>* flows, 
                            pthread_mutex_t* flow_list_mtx,
                            list<TCPSession>::iterator peer, 
                            list<MsgHdr>::iterator req_hdr) {
  if (msg_hdr.http_hdr().status_code() == 200) {
    // Ryu should never send us a file (at least I'm not programming for one).
    if (msg_data.Exists(NULL) && msg_data.size(NULL) > 0) {
      error.Init(EX_DATAERR, "conga_process_response(): "
                 "Recevied a file in message-body from %s, "
                 "but unable to process", peer->hostname().c_str());
      return;
    }

    logger.Log(LOG_NOTICE, 
               "Received RESPONSE \'%d %s\', with %db message-body "
               "from %s for REQUEST: %s.", 
               msg_hdr.http_hdr().status_code(), 
               status_code_phrase(msg_hdr.http_hdr().status_code()),
               (int)msg_body.size(), peer->TCPConn::print().c_str(), 
               req_hdr->http_hdr().print_start_line(false).c_str());

#if 0
    // For Debugging: To see what RapidJSON values are.
    static const char* kTypeNames[] = { "Null", "False", "True", "Object",
                                        "Array", "String", "Number" };
#endif

    //printf("XXX DEBUG: parsing: %s.\n", msg_body.c_str());
    // Parse JSON message-body.
    rapidjson::Document response;
    if (response.Parse(msg_body.c_str()).HasParseError()) {
      error.Init(EX_DATAERR, "conga_process_response(): "
                 "Failed to parse JSON from %s: %s",
                 peer->hostname().c_str(), msg_body.c_str());
      return;
    }

    // Now, see what type & how to process JSON message-body.
    if (!response.IsObject()) {
      error.Init(EX_DATAERR, "conga_process_response(): invalid JSON object: %s",
                 msg_body.c_str());
      return;
    }

    rapidjson::Value::ConstMemberIterator dpid_itr = response.MemberBegin();
    if (dpid_itr == response.MemberEnd() || !dpid_itr->value.IsArray()) {
      error.Init(EX_DATAERR, "conga_process_response(): "
                 "First element NULL, empty or not an array: %s",
                 msg_body.c_str());
      return;
    }

    MeterInfo tmp_meter;
    string tmp_dpid = dpid_itr->name.GetString();

    // Each tuple in a stats/meter response should consist of:
    //
    // { "duration_sec": 2577221, 
    //   "band_stats": [{
    //      "byte_band_count": 0, 
    //      "packet_band_count": 0}],
    //   "meter_id": 1,
    //   "flow_count": 0,
    //   "packet_in_count": 0,
    //   "duration_nsec": 979000000,
    //   "len": 56,
    //   "byte_in_count": 0}
    //
    // While stats/meterconfig looks like:
    //
    // { "bands": [{
    //      "burst_size": 0,
    //      "rate": 1000000,
    //      "type": "DROP"}],
    //   "flags": ["STATS", "KBPS"],
    //   "meter_id": 1}
    //
    // Finally, when we get an allocation request, we'll need to
    // process a stats/flow response in order to figure out what meter
    // the flow is in.  It looks like this below (note, those idiots
    // used the same elements, i.e., "flags" & "duration_sec" to
    // represent different data types!):
    //
    // { "actions": [
    //      "METER:100",
    //      "SET_FIELD: { vlan_vid:8146 }",
    //      "SET_QUEUE:0",
    //      "OUTPUT:22"],
    //   "idle_timeout": 0,
    //   "cookie": 0,
    //   "packet_count": 0,
    //   "hard_timeout": 0,
    //   "byte_count": 0,
    //   "length": 128,
    //   "duration_nsec": 352000000,
    //   "priority": 32768,
    //   "duration_sec": 2788274,
    //   "table_id": 30,
    //   "flags": 0,
    //   "match": {
    //      "dl_type": 2048,
    //      "dl_vlan": "4010",
    //      "nw_dst": "10.10.3.113"}}

    // Walk over each dpid (although I suspect there'll be only one).

    // RAPIDJSON: Uses SizeType instead of size_t.
    const rapidjson::Value& dpid = response[tmp_dpid.c_str()];
    for (rapidjson::SizeType i = 0; i < dpid.Size(); ++i) {
      tmp_meter.dpid_ = dpid_itr->name.GetString();

      // We can process the stats/meter & stats/meterconfig the same,
      // however, we need to process the stats/flow differently.

      // PROCESS: stats/flow
      if (dpid[i].HasMember(kNameActions)) {
        const rapidjson::Value& actions = dpid[i][kNameActions];
        if (!actions.IsArray()) {
          logger.Log(LOG_WARNING, "conga_process_response(): "
                     "%s is not an array in JSON from %s: %s",
                     kNameActions, peer->hostname().c_str(), msg_body.c_str());
          continue;
        }

        // Since it's a stats/flow, we're looking for the element that
        // begins "METER:" ...

#if 0  // For Debugging:
        static const char* kTypeNames[] = { "Null", "False", "True", "Object", "Array", "String", "Number" };
        for (rapidjson::Value::ConstMemberIterator action_itr = actions.MemberBegin(); action_itr != response.MemberEnd(); ++action_itr)
          printf("DEBUG: XXX Working on %s (%s) ...\n", action_itr->name.GetString(), kTypeNames[action_itr->value.GetType()]);
#endif

        int tmp_meter = -1;
        for (rapidjson::SizeType k = 0; k < actions.Size(); ++k) {
          string tmp_element = actions[k].GetString();
          if (tmp_element.compare(0, 6, "METER:") == 0) {
            tmp_meter = atoi(tmp_element.substr(6).c_str());
            break;
          }
        }
        if (tmp_meter == -1) {
          logger.Log(LOG_WARNING, "conga_process_response(): TODO(aka) "
                     "Received stats/flow response, but unable to find METER "
                     "in JSON from %s: %s.", 
                     peer->hostname().c_str(), msg_body.c_str());
          return;
        } 

        // In order to associate this meter with the correct flow,
        // we need to grab the nw_dst element in the match element
        // ...

        if (!dpid[i].HasMember(kNameMatch)) {
          logger.Log(LOG_WARNING, "conga_process_response(): "
                     "%s is not in JSON from %s: %s",
                     kNameMatch, peer->hostname().c_str(), msg_body.c_str());
          return;
        }

        const rapidjson::Value& match = dpid[i][kNameMatch];
        if (!match.IsObject() || !match.HasMember(kNameNwDst)) {
          logger.Log(LOG_WARNING, "conga_process_response(): "
                     "%s is not an object or %s is not in JSON from %s: %s",
                     kNameMatch, kNameNwDst,
                     peer->hostname().c_str(), msg_body.c_str());
          return;
        }

        const rapidjson::Value& nw_dst = match[kNameNwDst];
        if (!nw_dst.IsString()) {
          logger.Log(LOG_WARNING, "conga_process_response(): "
                     "%s is not a string in JSON from %s: %s",
                     kNameNwDst, peer->hostname().c_str(), msg_body.c_str());
          return;
        }

        string dst = nw_dst.GetString();

#if DEBUG_MUTEX_LOCK
        warnx("conga_process_response: requesting flow list lock.");
#endif
        pthread_mutex_lock(flow_list_mtx);
        list<FlowInfo>::iterator flow_itr = flows->begin();
        while (flow_itr != flows->end()) {
          printf("DEBUG: XXX comparing %s to %s.\n", flow_itr->dst_ip_.c_str(), dst.c_str());
          if (flow_itr->dst_ip_.compare(dst) == 0) {
            flow_itr->meter_ = tmp_meter;
            break;
          }
          flow_itr++;
        }
        if (flow_itr == flows->end()) {
          logger.Log(LOG_WARNING, "conga_process_response(): TODO(aka) "
                     "Unable to find dst %s in flow list", dst.c_str());
          return;
        }
#if DEBUG_MUTEX_LOCK
        warnx("conga_process_response: releasing flow list lock.");
#endif
        pthread_mutex_unlock(flow_list_mtx);
        continue;  // head back to for loop, but we only ever have 1 dpid
      }  // if (dpid[i].HasMember(kNameActions)) {

      // PROCESS: stats/meter[config]
      if (!dpid[i].HasMember(kNameMeterID) || 
          !dpid[i][kNameMeterID].IsNumber()) {
        logger.Log(LOG_INFO, "conga_process_response(): "
                   "No %s in JSON from %s: %s",
                   kNameMeterID, peer->hostname().c_str(), msg_body.c_str());
        continue;
      }

      // If we made it here, we have a meter id, so let's get any
      // other metrics wer're concerned with.

      tmp_meter.meter_ = dpid[i][kNameMeterID].GetInt();

      if (dpid[i].HasMember(kNameFlowCount)) {
        if (!dpid[i][kNameFlowCount].IsNumber()) {
          logger.Log(LOG_INFO, "conga_process_response(): "
                     "%s in unknown type in JSON from %s: %s",
                     kNameFlowCount, peer->hostname().c_str(),
                     msg_body.c_str());
          continue;
        }
        tmp_meter.flow_count_ = dpid[i][kNameFlowCount].GetInt();
      }

      if (dpid[i].HasMember(kNameByteInCount)) {
        // Make sure it's a number.
        if (!dpid[i][kNameByteInCount].IsNumber()) {
          logger.Log(LOG_WARNING, "conga_process_response(): "
                     "%s is not a number in JSON from %s: %s",
                     kNameByteInCount, peer->hostname().c_str(), 
                     msg_body.c_str());
          continue;
        }
        tmp_meter.byte_in_count_ = dpid[i][kNameByteInCount].GetInt64();
      }

      if (dpid[i].HasMember(kNameBandStats)) {
        // Make sure it's an array before processing its values.
        const rapidjson::Value& band_stats = dpid[i][kNameBandStats];
        if (!band_stats.IsArray()) {
          logger.Log(LOG_WARNING, "conga_process_response(): "
                     "%s is not an array in JSON from %s: %s",
                     kNameBandStats, peer->hostname().c_str(), msg_body.c_str());
          continue;
        }

        // Grab byte_band_count.
        for (rapidjson::SizeType j = 0; j < band_stats.Size(); ++j) {
          if (band_stats[j].HasMember(kNameByteBandCount)) {
            if (!band_stats[j][kNameByteBandCount].IsNumber()) {
              logger.Log(LOG_WARNING, "conga_process_response(): "
                         "%s is not a numbrer in JSON from %s: %s",
                         kNameByteBandCount, peer->hostname().c_str(),
                         msg_body.c_str());
              continue;
            }
            tmp_meter.byte_band_count_ = 
                (uint64_t)band_stats[j][kNameByteBandCount].GetInt64();
          }
        }
      }  // if (dpid[i].HasMember(kNameBandStats)) {

      if (dpid[i].HasMember(kNameBands)) {
        // Make sure it's an array before processing its values.
        const rapidjson::Value& bands = dpid[i][kNameBands];
        if (!bands.IsArray()) {
          logger.Log(LOG_WARNING, "conga_process_response(): "
                     "%s is not an array in JSON from %s: %s",
                     kNameBands, peer->hostname().c_str(), msg_body.c_str());
          continue;
        }

        // Grab rate.
        for (rapidjson::SizeType j = 0; j < bands.Size(); ++j) {
          if (bands[j].HasMember(kNameRate)) {
            if (!bands[j][kNameRate].IsNumber()) {
              logger.Log(LOG_WARNING, "conga_process_response(): "
                         "%s is not a numbrer in JSON from %s: %s",
                         kNameRate, peer->hostname().c_str(),
                         msg_body.c_str());
              continue;
            }

            tmp_meter.rate_ = (uint64_t)bands[j][kNameRate].GetInt64();
          }
        }
      }  // if (dpid[i].HasMember(kNameBands)) {

      if (dpid[i].HasMember(kNameFlags)) {
        // Make sure it's an array before processing its values.
        const rapidjson::Value& flags = dpid[i][kNameFlags];
        if (!flags.IsArray()) {
          logger.Log(LOG_WARNING, "conga_process_response(): "
                     "%s is not an array in JSON from %s: %s",
                     kNameFlags, peer->hostname().c_str(), msg_body.c_str());
          continue;
        }

        // Grab elements, hopefully one is a rate classification type.
        for (rapidjson::SizeType j = 0; j < flags.Size(); ++j) {
          if (!flags[j].IsString()) {
            logger.Log(LOG_WARNING, "conga_process_response(): "
                       "flags[%d] is not a string in JSON from %s: %s",
                       j, peer->hostname().c_str(),
                       msg_body.c_str());
            continue;
          }

          string tmp_flag = flags[j].GetString();

          // And let's skip "STATS".
          if (!tmp_flag.compare("STATS"))
            tmp_meter.flag_rate_ = tmp_flag;  // TODO(aka) technically
                                              // if something other
                                              // than a rate or STATS
                                              // is in here, we could
                                              // get overwritten!
        }
      }  // if (dpid[i].HasMember(kNameFlags)) {

      // Now, see if we add this meter to our sdn_state, or simply update it.
#if DEBUG_MUTEX_LOCK
      warnx("conga_process_response(): requesting sdn_state lock.");
#endif
      pthread_mutex_lock(sdn_state_mtx);

      //printf("XXX Parsed meter %d from %s.\n", tmp_meter.meter_, tmp_meter.dpid_.c_str());
      list<MeterInfo>::iterator meter_itr = sdn_state->begin();
      while (meter_itr != sdn_state->end()) {
        //printf("XXX Comparing the above to exising: %d, %s.\n", meter_itr->meter_, meter_itr->dpid_.c_str());
        if (!tmp_meter.dpid_.compare(meter_itr->dpid_) 
            && tmp_meter.meter_ == meter_itr->meter_)
          break;  // found our meter
        meter_itr++;
      }
      if (meter_itr == sdn_state->end()) {
        // Add new entry to our SDN state.
        tmp_meter.time_ = time(NULL);
        sdn_state->push_back(tmp_meter);
        logger.Log(LOG_NOTICE, "Added new meter (%d) from %s, "
                   "rate: %lu, bytes in: %lld",
                   tmp_meter.meter_, tmp_meter.dpid_.c_str(),
                   (unsigned long)tmp_meter.rate_, tmp_meter.byte_in_count_);
      } else {
        // Update our SDN state.
        if (tmp_meter.rate_ > meter_itr->rate_) {
          logger.Log(LOG_NOTICE, "Updating meter (%d) from %s with rate: %lu",
                     tmp_meter.meter_, tmp_meter.dpid_.c_str(),
                     (unsigned long)tmp_meter.rate_);
          meter_itr->rate_ = tmp_meter.rate_;  // TODO(aka): Minor HACK
        }
        meter_itr->flow_count_ = tmp_meter.flow_count_;
        meter_itr->prev_time_ = meter_itr->time_;
        meter_itr->prev_byte_band_count_ = meter_itr->byte_band_count_;
        meter_itr->prev_byte_in_count_ = meter_itr->byte_in_count_;
        meter_itr->time_ = time(NULL);
        meter_itr->byte_band_count_ = tmp_meter.byte_band_count_;
        meter_itr->byte_in_count_ = tmp_meter.byte_in_count_;

#if 0  // For Debugging:
        uint64_t throughput = 
            (meter_itr->byte_in_count_ - meter_itr->prev_byte_in_count_) /
            (meter_itr->time_ - meter_itr->prev_time_);

        logger.Log(LOG_NOTICE, "Updated meter %d from %s, throughput: %lld",
                   tmp_meter.meter_, tmp_meter.dpid_.c_str(), throughput);
#endif
      }  // else if (meter_itr != sdn_state->end()) {
#if DEBUG_MUTEX_LOCK
      warnx("conga_process_response(): releasing sdn_state lock.");
#endif
      pthread_mutex_unlock(sdn_state_mtx);

      tmp_meter.clear();
    }  // for (rapidjson::SizeType i = 0; i < dpid.Size(); ++i) {

#if 0
    {
      error.Init(EX_DATAERR, "conga_process_response(): unknown JSON: %s", 
                 msg_body.c_str());
      return;
    }
#endif

  } else if (msg_hdr.http_hdr().status_code() == 201 ||
             msg_hdr.http_hdr().status_code() == 202 ||
             msg_hdr.http_hdr().status_code() == 204) {
    logger.Log(LOG_NOTICE, 
               "Received RESPONSE \'%d %s\' from %s for REQUEST: %s.", 
               msg_hdr.http_hdr().status_code(), 
               status_code_phrase(msg_hdr.http_hdr().status_code()),
               peer->TCPConn::print().c_str(), 
               req_hdr->http_hdr().print_start_line(false).c_str());
  } else {  // if (msg_hdr.http_hdr().status_code() == 200) {
    if (msg_body.size() > 0)
      logger.Log(LOG_NOTICE, 
                 "Received ERROR response \'%d %s\' from %s "
                 "for REQUEST: %s: %s.",
                 msg_hdr.http_hdr().status_code(), 
                 status_code_phrase(msg_hdr.http_hdr().status_code()),
                 peer->TCPConn::print().c_str(),
                 req_hdr->http_hdr().print_hdr(0, false).c_str(),
                 msg_body.c_str());
    else
      logger.Log(LOG_NOTICE, 
                 "Received ERROR response \'%d %s\' from %s for REQUEST: %s.",
                 msg_hdr.http_hdr().status_code(), 
                 status_code_phrase(msg_hdr.http_hdr().status_code()),
                 peer->TCPConn::print().c_str(),
                 req_hdr->http_hdr().print_hdr(0, false).c_str());
  }
}

// Process requests based on RESTful API.

// Routine to authorize a user for a future flow generation.
//
// Note, this routine blocks on the waiting HTTP response.
string conga_process_post_auth(const ConfInfo& info, const HTTPFraming& http_hdr,
                               const string& msg_body, const File& msg_data,
                               SSLContext* ssl_context,
                               list<AuthInfo>* authenticators, 
                               pthread_mutex_t* authenticators_mtx,
                               list<TCPSession>::iterator peer) {
  URL url = http_hdr.uri();

  // Parse JSON message-body.
  rapidjson::Document details;
  if (details.Parse(msg_body.c_str()).HasParseError()) {
    error.Init(EX_DATAERR, "conga_process_post_auth(): "
               "Failed to parse JSON: %s", msg_body.c_str());
    return "";
  }

  if (!details.HasMember(kDetailUserID) ||
      !details[kDetailUserID].IsString() ||
      !details.HasMember(kDetailProjectID) ||
      !details[kDetailProjectID].IsString() ||
      !details.HasMember(kDetailResourceID) ||
      !details[kDetailResourceID].IsString()) {
    error.Init(EX_DATAERR, "conga_process_post_auth(): "
               "%s, %s or %s is invalid: %s", 
               kDetailUserID, kDetailProjectID, 
               kDetailResourceID, msg_body.c_str());
    return "";
  }

  // See if they requested a renewal.
  if (details.HasMember(kDetailAPIKey) && details[kDetailAPIKey].IsString()) {
    // Since we're just checking (reading) authenticators, we don't need a lock.
    list<AuthInfo>::iterator api_key_itr = authenticators->begin();
    while (api_key_itr != authenticators->end()) {
      string key = api_key_itr->api_key_;
      // API key is *not* case-sensitive!
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      if (!key.compare(details[kDetailAPIKey].GetString()))
        break;

      api_key_itr++;
    }

    if (api_key_itr == authenticators->end()) {
      // They requested a renewal, but we have no record of this key!
      error.Init(EX_DATAERR, "conga_process_post_auth(): "
                 "API Key: %s, not found", details[kDetailAPIKey].GetString());
      return "";
    }
  }

  logger.Log(LOG_DEBUG, "conga_process_post_auth(): "
             "working on user: %s, project: %s, resource: %s.",
             details[kDetailUserID].GetString(), 
             details[kDetailProjectID].GetString(),
             details[kDetailResourceID].GetString());

  // See if user is (still) an authorized user.

  // Setup SSL connection.
  const string auth_db_host = "dirsdev.psc.edu";
  TCPSession tmp_session(MsgHdr::TYPE_HTTP);
  tmp_session.Init();  // set aside buffer space
  tmp_session.SSLConn::Init(auth_db_host.c_str(), AF_INET, 
                            IPCOMM_DNS_RETRY_CNT);  // init IPComm base class
  tmp_session.set_port(443);
  tmp_session.set_blocking();
  tmp_session.Socket(PF_INET, SOCK_STREAM, 0, ssl_context);
  //tmp_session.set_handle(tmp_session.fd());  // for now, set it to the socket
  if (error.Event()) {
    error.AppendMsg("conga_process_post_auth(): ");
    return "";
  }

  // Build a (HTTP) framing header and load the framing header into
  // our TCPSession's MsgHdr list.
  //
  // example: https://dirsdev.psc.edu/dances/api/v1/resource/blacklight.psc.teragrid/username/akadams/grant_number/TG-MCB110157

  URL auth_db_url;
  char tmp_buf[kURLMaxSize];
  snprintf(tmp_buf, kURLMaxSize, "%s/resource/%s/username/%s/grant_number/%s",
           kServiceDancesAuth, details[kDetailResourceID].GetString(),
           details[kDetailUserID].GetString(),
           details[kDetailProjectID].GetString());
  auth_db_url.Init("https", auth_db_host.c_str(), 443,
                   tmp_buf, strlen(tmp_buf), NULL, 0, NULL);
  HTTPFraming auth_http_hdr;
  auth_http_hdr.InitRequest(HTTPFraming::GET, auth_db_url);

#if 0  // TODO(aka) Empty message body, so not needed.
  // Add HTTP content-length message-headers (for an empty message-body).
  struct rfc822_msg_hdr mime_msg_hdr;
  mime_msg_hdr.field_name = MIME_CONTENT_LENGTH;
  mime_msg_hdr.field_value = "0";
  auth_http_hdr.AppendMsgHdr(mime_msg_hdr);
#endif

  // Add Host message-header.
  struct rfc822_msg_hdr mime_msg_hdr;
  mime_msg_hdr.field_name = MIME_HOST;
  mime_msg_hdr.field_value = auth_db_host;
  auth_http_hdr.AppendMsgHdr(mime_msg_hdr);

  logger.Log(LOG_DEBUG, "conga_process_post_auth(): Generated HTTP headers:\n%s", auth_http_hdr.print_hdr(0, false).c_str());

  MsgHdr tmp_msg_hdr(MsgHdr::TYPE_HTTP);
  tmp_msg_hdr.Init(++msg_id_hash, auth_http_hdr);
  tmp_session.AddMsgBuf(auth_http_hdr.print_hdr(0, false).c_str(),
                        auth_http_hdr.hdr_len(false), "", 0, tmp_msg_hdr);

  // HACK: Normally, we would add our REQUEST message to our outgoing
  // TCPSession list (to_peers), and then go back to wait for
  // transmission in the event-loop, processing the results in
  // conga_process_response().  However, doing that would require us
  // to be able to associate the AuthInfo between the two SSL lists
  // (which I think could be done via the MsgHdr msg_id!).  So, for
  // now, we're just going to sequentially turn around and ask the
  // auth server for a response in here.

  logger.Log(LOG_INFO, "Sending REQUEST: \'%s\n%s\' to %s.", 
             auth_http_hdr.print_start_line(false).c_str(), 
             auth_http_hdr.print_msg_hdrs().c_str(), 
             tmp_session.SSLConn::print().c_str());

  // Okay, try and connect, then send out our request.
  tmp_session.Connect();
  tmp_session.Write();
  if (error.Event()) {
    error.AppendMsg("conga_process_post_auth(): ");
    return "";
  }

  // If we made it here, hang around to get our response ...
  bool eof = false;
  ssize_t bytes_read = 0;
  while (!eof) {
    bytes_read = tmp_session.Read(&eof);
    if (error.Event()) {
      error.AppendMsg("conga_process_post_auth(): ");
      return "";
    }
    if (bytes_read > 0) {
      if (!tmp_session.IsIncomingMsgInitialized())
        tmp_session.InitIncomingMsg();
      if (error.Event()) {
        error.AppendMsg("conga_process_post_auth(): ");
        return "";
      }

      if (tmp_session.IsIncomingMsgComplete())
        break;
    }
  }
  tmp_session.Close();

  logger.Log(LOG_DEBUG, "conga_process_post_auth(): Read %ld byte(s) from %s, rbuf_len: %ld, eof: %d.", bytes_read, tmp_session.hostname().c_str(), tmp_session.rbuf_len(), eof);

  // Process the response.
  if (!tmp_session.IsIncomingMsgInitialized()) {
    error.Init(EX_DATAERR, "conga_process_post_auth(): "
               "Not INITIALIZED: Failed to parse response from %s", 
               tmp_session.hostname().c_str());
    return "";
  }

  const MsgHdr msg_hdr = tmp_session.rhdr();
  if (msg_hdr.http_hdr().status_code() != 200) {
    error.Init(EX_DATAERR, "conga_process_post_auth(): "
               "Communication failure with %s: %s",
               tmp_session.hostname().c_str(),
               tmp_session.rhdr().print().c_str());
    return "";
  }

  // TODO(aka) Do we need to check for any specific message-headers?

  // Get the message-body.
  string auth_msg_body;
  auth_msg_body.assign(tmp_session.rbuf(), tmp_session.rhdr().body_len());
  tmp_session.ClearIncomingMsg();  // clean-up, just incase it gets used again

  logger.Log(LOG_DEBUG, "conga_process_post_auth(): "
             "Received RESPONSE \'%d %s\' from %s for REQUEST: %s, "
             "message-body: %s.", 
             msg_hdr.http_hdr().status_code(), 
             status_code_phrase(msg_hdr.http_hdr().status_code()),
             tmp_session.TCPConn::print().c_str(), 
             auth_http_hdr.print_start_line(false).c_str(),
             auth_msg_body.c_str());

  // Parse JSON message-body.
  rapidjson::Document response;
  if (response.Parse(auth_msg_body.c_str()).HasParseError()) {
    error.Init(EX_DATAERR, "conga_process_post_auth(): "
               "Failed to parse JSON from %s: %s",
               tmp_session.hostname().c_str(), auth_msg_body.c_str());
    return "";
  }

  if (!response.HasMember(kDetailIsActive) ||
      !response[kDetailIsActive].IsBool()) {
    error.Init(EX_DATAERR, "conga_process_post_auth(): %s is invalid: %s", 
               kDetailIsActive, tmp_session.rbuf());
    return "";
  }

  if (!response[kDetailIsActive].GetBool()) {
    error.Init(EX_DATAERR, "Authorization failure using request %s",
               auth_db_url.print().c_str());
    return "";
  }

  // Setup auth variables.
  string ret_msg(kHTTPMsgBodyMaxSize, '\0');
  int status = 0;
  string state = "running";  // TODO(aka) Does this even make sense in an auth request?
  int duration = info.duration_;
  time_t start_time = time(NULL);
  time_t end_time = start_time + (time_t)duration;

  // Build POST AUTH results, depending on whether this is a new
  // request, or the user has an existing api-key.

  if (details.HasMember(kDetailAPIKey) && details[kDetailAPIKey].IsString()) {
    // Reaquire the iterator from authenticators for our key, as another
    // thread may have mucked with authenticators since our earlier check.

#if DEBUG_MUTEX_LOCK
    warnx("conga_process_post_auth: requesting api keys lock.");
#endif
    pthread_mutex_lock(authenticators_mtx);
    list<AuthInfo>::iterator api_key_itr = authenticators->begin();
    while (api_key_itr != authenticators->end()) {
      string key = api_key_itr->api_key_;
      // API key is *not* case-sensitive!
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      if (!key.compare(details[kDetailAPIKey].GetString()))
        break;

      api_key_itr++;
    }

    if (api_key_itr == authenticators->end()) {
      // This should never happen, but hey, who knows in MT land.
      error.Init(EX_DATAERR, "conga_process_post_auth(): "
                 "API Key: %s, is now missing", details[kDetailAPIKey].GetString());
#if DEBUG_MUTEX_LOCK
      warnx("conga_process_post_auth: releasing api keys lock.");
#endif
      pthread_mutex_unlock(authenticators_mtx);
      return "";
    }

    // Update our expiration (note, we leave start_time_ unchanged).
    api_key_itr->end_time_ = end_time;

    snprintf((char*)ret_msg.c_str(), kHTTPMsgBodyMaxSize - 1, "{ \"status\":%d, \"results\": [ { "
             "\"%s\":\"%s\", "
             "\"%s\": %d, \"%s\": %d, "
             "\"%s\":\"%s\", \"%s\":\"%s\", \"%s\":\"%s\""
             "} ] }", 
             status, kDetailAPIKey, api_key_itr->api_key_.c_str(), 
             kDetailStartTime, api_key_itr->start_time_, 
             kDetailEndTime, api_key_itr->end_time_,
             kDetailUserID, api_key_itr->user_id_.c_str(),
             kDetailProjectID, api_key_itr->project_id_.c_str(),
             kDetailResourceID, api_key_itr->resource_id_.c_str());

#if DEBUG_MUTEX_LOCK
    warnx("conga_process_post_auth: releasing api keys lock.");
#endif
    pthread_mutex_unlock(authenticators_mtx);

    logger.Log(LOG_NOTICE, "Processed POST auth (%s:%s, %s:%s, %s:%s) from %s.",
               kDetailUserID, api_key_itr->user_id_.c_str(),
               kDetailProjectID, api_key_itr->project_id_.c_str(),
               kDetailResourceID, api_key_itr->resource_id_.c_str(),
               peer->hostname().c_str());
  } else {
    AuthInfo new_key;
    new_key.api_key_ = gen_random_string(kAPIKeySize);
    new_key.user_id_ = details[kDetailUserID].GetString();
    new_key.project_id_ = details[kDetailProjectID].GetString();
    new_key.resource_id_ = details[kDetailResourceID].GetString();
    //new_key.msg_hdr_id_ = msg_id_hash;  // XXX Do we need this?

    new_key.start_time_ = start_time;
    new_key.end_time_ = end_time;

    snprintf((char*)ret_msg.c_str(), kHTTPMsgBodyMaxSize - 1,
             "{ \"status\":%d, \"results\": [ { "
             "\"%s\":\"%s\", "
             "\"%s\": %d, \"%s\": %d, "
             "\"%s\":\"%s\", \"%s\":\"%s\", \"%s\":\"%s\""
             "} ] }", 
             status, kDetailAPIKey, new_key.api_key_.c_str(), 
             kDetailStartTime, new_key.start_time_, 
             kDetailEndTime, new_key.end_time_,
             kDetailUserID, new_key.user_id_.c_str(),
             kDetailProjectID, new_key.project_id_.c_str(),
             kDetailResourceID, new_key.resource_id_.c_str());

#if DEBUG_MUTEX_LOCK
    warnx("conga_process_post_auth: requesting api keys lock.");
#endif
    pthread_mutex_lock(authenticators_mtx);
    authenticators->push_back(new_key);
    auth_info_list_save_state(info.auth_info_list_file_, *authenticators);

#if DEBUG_MUTEX_LOCK
    warnx("conga_process_post_auth: releasing api keys lock.");
#endif
    pthread_mutex_unlock(authenticators_mtx);

    logger.Log(LOG_NOTICE, "Processed POST auth (%s:%s, %s:%s, %s:%s) from %s.",
               kDetailUserID, new_key.user_id_.c_str(),
               kDetailProjectID, new_key.project_id_.c_str(),
               kDetailResourceID, new_key.resource_id_.c_str(),
               peer->hostname().c_str());
  }

  // Head back to conga_process_incoming_msg() to send RESPONSE out.
  return ret_msg;
}

// Routine to delete an API key.
string conga_process_delete_auth(const ConfInfo& info,
                                 const HTTPFraming& http_hdr,
                                 const string& msg_body, const File& msg_data,
                                 list<AuthInfo>* authenticators, 
                                 pthread_mutex_t* authenticators_mtx,
                                 list<TCPSession>::iterator peer) {
  URL url = http_hdr.uri();

  // Grab the API Key from the path (should be the last value).
  size_t last_slash = url.path().find_last_of("/");
  string api_key = url.path().substr(last_slash + 1);
  if (api_key.size() <= 0) {
    // No key, no work.
    error.Init(EX_DATAERR, "conga_process_delete_auth(): "
               "API Key not included in URL query: %s", url.print().c_str());
    return "";
  }

  // Search authenticators, locking it.
#if DEBUG_MUTEX_LOCK
  warnx("conga_process_delete_auth: requesting api keys lock.");
#endif
  pthread_mutex_lock(authenticators_mtx);

  list<AuthInfo>::iterator api_key_itr = authenticators->begin();
  while (api_key_itr != authenticators->end()) {
    string key = api_key_itr->api_key_;
    // API key is *not* case-sensitive!
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (!key.compare(api_key))
      break;

    api_key_itr++;
  }

  if (api_key_itr == authenticators->end()) {
    // We have no record of this key (anymore?).
    error.Init(EX_DATAERR, "conga_process_delete_auth(): "
               "API Key: %s, not found", api_key.c_str());
#if DEBUG_MUTEX_LOCK
    warnx("conga_process_delete_auth: releasing api keys lock.");
#endif
    pthread_mutex_unlock(authenticators_mtx);
    return "";
  }

  logger.Log(LOG_DEBUG, "conga_process_delete_auth(): "
             "working on key: %s, user: %s, project: %s, resource: %s.",
             api_key_itr->api_key_.c_str(),
             api_key_itr->user_id_.c_str(), api_key_itr->project_id_.c_str(),
             api_key_itr->resource_id_.c_str());

  AuthInfo tmp_auth(*api_key_itr);  // save a copy before blowing it away
  authenticators->erase(api_key_itr);
  auth_info_list_save_state(info.auth_info_list_file_, *authenticators);

#if DEBUG_MUTEX_LOCK
  warnx("conga_process_delete_auth: releasing api keys lock.");
#endif
  pthread_mutex_unlock(authenticators_mtx);

  string ret_msg(kHTTPMsgBodyMaxSize, '\0');
  int status = 0;

  // Build the response.
  snprintf((char*)ret_msg.c_str(), kHTTPMsgBodyMaxSize - 1,
           "{ \"status\":%d, \"results\": [ { "
           "\"%s\":\"%s\""
           "} ] }", 
           status, kDetailAPIKey, api_key.c_str());

  logger.Log(LOG_NOTICE, "Processed DELETE auth (%s:%s, %s:%s, %s:%s) from %s.",
             kDetailAPIKey, tmp_auth.api_key_.c_str(),
             kDetailUserID, tmp_auth.user_id_.c_str(),
             kDetailProjectID, tmp_auth.project_id_.c_str(),
             peer->hostname().c_str());

  // Head back to conga_process_incoming_msg() to send RESPONSE out.
  return ret_msg;
}

// Routine to ge the status of a user's token (api-key).
string conga_process_get_auth(const ConfInfo& info, const HTTPFraming& http_hdr,
                              const string& msg_body, const File& msg_data,
                              list<AuthInfo>* authenticators, 
                              pthread_mutex_t* authenticators_mtx,
                              list<TCPSession>::iterator peer) {
  URL url = http_hdr.uri();

  // Grab the API Key from the path (should be the last value).
  size_t last_slash = url.path().find_last_of("/");
  string api_key = url.path().substr(last_slash + 1);
  if (api_key.size() <= 0) {
    // No key, no work.
    error.Init(EX_DATAERR, "conga_process_get_auth(): "
               "API Key not included in URL query: %s", url.print().c_str());
    return "";
  }

#if 0  // Deprecated code: API Key use to be in the query as opposed to the path.
  list<struct url_query_info> queries = url.query();
  list<struct url_query_info>::iterator key_itr = queries.begin();
  while (key_itr != queries.end()) {
    string key = key_itr->key;
    // API key is *not* case-sensitive!
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (!key.compare(kDetailAPIKey)) {
      request_info->api_key_ = key_itr->value;
    } else {
      logger.Log(LOG_WARNING, "conga_process_get_auth(): Received unknown query: %s=%s.",
                 key.c_str(), key_itr->value.c_str());
    }

    key_itr++;
  }

  if (key_itr == queries.end()) {
    // They requested a renewal, but we have no record of this key!
    error.Init(EX_DATAERR, "conga_process_get_auth(): "
               "API Key not included in URL query: %s", url.print().c_str());
    return "";
  }
#endif

  // Search authenticators, locking it.
#if DEBUG_MUTEX_LOCK
  warnx("conga_process_get_auth: requesting api keys lock.");
#endif
  pthread_mutex_lock(authenticators_mtx);
  list<AuthInfo>::iterator api_key_itr = authenticators->begin();
  while (api_key_itr != authenticators->end()) {
    string key = api_key_itr->api_key_;
    // API key is *not* case-sensitive!
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (!key.compare(api_key))
      break;

    api_key_itr++;
  }

  if (api_key_itr == authenticators->end()) {
    // We have no record of this key (anymore?).
    error.Init(EX_DATAERR, "conga_process_get_auth(): "
               "API Key: %s, not found", api_key.c_str());
#if DEBUG_MUTEX_LOCK
    warnx("conga_process_get_auth: releasing api keys lock.");
#endif
    pthread_mutex_unlock(authenticators_mtx);
    return "";
  }

  logger.Log(LOG_DEBUG, "conga_process_get_auth(): "
             "working on user: %s, project: %s, resource: %s.",
             api_key_itr->user_id_.c_str(), api_key_itr->project_id_.c_str(),
             api_key_itr->resource_id_.c_str());

  string ret_msg(kHTTPMsgBodyMaxSize, '\0');
  int status = 0;
  string state = "running";

  // Build the response.
  snprintf((char*)ret_msg.c_str(), kHTTPMsgBodyMaxSize - 1,
           "{ \"status\":%d, \"results\": [ { "
           "\"%s\":\"%s\", "
           "\"%s\": %d, \"%s\": %d, "
           "\"%s\":\"%s\", \"%s\":\"%s\", \"%s\":\"%s\""
           "} ] }", 
           status, kDetailAPIKey, api_key_itr->api_key_.c_str(), 
           kDetailStartTime, api_key_itr->start_time_, 
           kDetailEndTime, api_key_itr->end_time_,
           kDetailUserID, api_key_itr->user_id_.c_str(),
           kDetailProjectID, api_key_itr->project_id_.c_str(),
           kDetailResourceID, api_key_itr->resource_id_.c_str());


#if DEBUG_MUTEX_LOCK
  warnx("conga_process_get_auth: releasing api keys lock.");
#endif
  pthread_mutex_unlock(authenticators_mtx);

  logger.Log(LOG_NOTICE, "Processed GET auth (%s:%s, %s:%s, %s:%s) from %s.",
             kDetailAPIKey, api_key_itr->api_key_.c_str(),
             kDetailUserID, api_key_itr->user_id_.c_str(),
             kDetailProjectID, api_key_itr->project_id_.c_str(),
             peer->hostname().c_str());

  // Head back to conga_process_incoming_msg() to send RESPONSE out.
  return ret_msg;
}

// Routine to handle POST allocation requests.  If the user includes
// an allocation_id within the RESTful request, then it's treated as a
// renewal.
//
// TODO(aka) This routine can return void!
string conga_process_post_allocations(const ConfInfo& info,
                                      const HTTPFraming& http_hdr,
                                      const string& msg_body,
                                      const File& msg_data,
                                      list<AuthInfo>* authenticators, 
                                      pthread_mutex_t* authenticators_mtx,
                                      list<FlowInfo>* flows,
                                      pthread_mutex_t* flow_list_mtx,
                                      list<TCPSession>::iterator peer) {
  URL url = http_hdr.uri();

  // First, see if the allocation_id is specified.

#if 0  // Deprecated code: allocation_id use to be in the query as opposed to the path.
  list<struct url_query_info> queries = url.query();
  list<struct url_query_info>::iterator itr = queries.begin();
  while (itr != queries.end()) {
    string key = itr->key;
    // Allocation ID is *not* case-sensitive!
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (!key.compare(kDetailAllocationID)) {
      request_info->allocation_id_ = itr->value;
    } else {
      logger.Log(LOG_WARNING, "conga_process_post_allocations(): "
                 "Received unknown query: %s=%s.",
                 key.c_str(), itr->value.c_str());
    }

    itr++;
  }
#endif

#if 0  // And even more Deprecated code: Moved allocation_id to the json, so it behaved the same as api_key!
  size_t last_slash = url.path().find_last_of("/");
  string allocation_id = url.path().substr(last_slash + 1);
#endif

  // Parse JSON message-body.
  rapidjson::Document details;
  if (details.Parse(msg_body.c_str()).HasParseError()) {
    error.Init(EX_DATAERR, "conga_process_post_allocations(): "
               "Failed to parse JSON: %s", msg_body.c_str());
    return "";
  }

  // Make sure our API key is valid.
  if (!details.HasMember(kDetailAPIKey) || !details[kDetailAPIKey].IsString()) {
    error.Init(EX_DATAERR, "conga_process_post_allocations(): TODO(aka) "
               "%s, %s, %s, %s or %s is invalid: %s", 
               kDetailAPIKey, kDetailProjectID, 
               kDetailSrcIP, kDetailDstIP, kDetailRate, msg_body.c_str());
    return "";
  } 

#if DEBUG_MUTEX_LOCK
  warnx("conga_process_post_allocations: requesting api keys lock.");
#endif
  pthread_mutex_lock(authenticators_mtx);
  list<AuthInfo>::iterator api_key_itr = authenticators->begin();
  while (api_key_itr != authenticators->end()) {
    string key = api_key_itr->api_key_;
    // API key is *not* case-sensitive!
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (!key.compare(details[kDetailAPIKey].GetString()))
      break;

    api_key_itr++;
  }
  if (api_key_itr == authenticators->end()) {
    // We have no record of this key (anymore?).
    error.Init(EX_DATAERR, "conga_process_post_allocations(): "
               "API Key: %s, not found", details[kDetailAPIKey].GetString());
#if DEBUG_MUTEX_LOCK
    warnx("conga_process_post_allocations: releasing api keys lock.");
#endif
    pthread_mutex_unlock(authenticators_mtx);
    return "";
  }

  AuthInfo our_auth(*api_key_itr);  // save a copy so we can release our lock

#if DEBUG_MUTEX_LOCK
  warnx("conga_process_post_allocations: releasing api keys lock.");
#endif
  pthread_mutex_unlock(authenticators_mtx);

  // Now, get the allocation id. 
  string allocation_id;
  if (details.HasMember(kDetailAllocationID) &&
      details[kDetailAllocationID].IsString())
    allocation_id = details[kDetailAllocationID].GetString();

  // TODO(aka) Might want to get HTTP header in this debug ...
  logger.Log(LOG_DEBUG, "conga_process_post_allocations(): "
             "working on user: %s, project: %s, resource: %s "
             "and possible allocation id: %s.",
             our_auth.user_id_.c_str(), our_auth.project_id_.c_str(),
             our_auth.resource_id_.c_str(), allocation_id.c_str());

  // And see if this is a create or renewal request ...
  if (allocation_id.size() > 0) {
    // User requested a renewal.

    // TODO(aka) This is tough, because we have to some how tell the
    // main event-loop that this flow needs to re-check the desired
    // flow stats, but that's currently done by looking for an empty
    // allocation_id ... Actually, this may not be that bad, for
    // either the allocation id has expired, in which case we can just
    // zero it and process things normally, or it hasn't, and the
    // meter info in the flow should be the same (hopefully), so we
    // can just check the SDN state in here!

    error.Init(EX_DATAERR, "conga_process_post_allocations(): "
               "POST allocation renewal (%s) not supported yet",
               allocation_id.c_str());

#if 0
    // Make sure the flow already exist.
#if DEBUG_MUTEX_LOCK
    warnx("conga_process_post_allocations: requesting flow list lock.");
#endif
    pthread_mutex_lock(flow_list_mtx);
    list<FlowInfo>::iterator flow_itr = flows->begin();
    while (flow_itr != flows->end()) {
      string key = flow_itr->allocation_id_;
      // Allocation ID is *not* case-sensitive!  (Is it even alpha-numeric?)
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      if (!key.compare(new_flow.allocation_id_)) {
        exit(0); // XXX What are we doing here? Again?
        error.Init(EX_DATAERR, "conga_process_post_allocations(): "
                   "Allocation ID: %s already exists in flow table", 
                   new_flow.allocation_id_.c_str());
        return "";
      }

      flow_itr++;
    }
#if DEBUG_MUTEX_LOCK
    warnx("conga_process_post_allocations: releasing flow list lock.");
#endif
    pthread_mutex_unlock(flow_list_mtx);
#endif  // #if 0

    return "";
  }  // if (allocation_id.size() > 0) {

  // This is an instantiation request, sanity check we have the
  // necessary attributes.

  if (!details.HasMember(kDetailSrcIP) || !details[kDetailSrcIP].IsString() ||
      !details.HasMember(kDetailDstIP) || !details[kDetailDstIP].IsString() ||
      !details.HasMember(kDetailRate) || !details[kDetailRate].IsInt()) {
    error.Init(EX_DATAERR, "conga_process_post_allocations(): "
               "%s, %s or %s is invalid: %s", 
               kDetailSrcIP, kDetailDstIP, kDetailRate,
               msg_body.c_str());
    return "";
  }

  // Build the *desired* flow.  We'll go back to our main event-loop
  // and send out our stats/flow request to learn the meter-id based
  // on the lack of allocation_id in our flow list.  And once we know
  // the meter id for this src->dst, then we can check its throughput.

  // Build our new flow.
  FlowInfo new_flow;
  new_flow.api_key_ = details[kDetailAPIKey].GetString();
  new_flow.src_ip_ = details[kDetailSrcIP].GetString();
  new_flow.dst_ip_ = details[kDetailDstIP].GetString();
  new_flow.rate_ = details[kDetailRate].GetInt();
  new_flow.peer_ = peer->handle();

  // See what else we have ...
  if (details.HasMember(kDetailSrcPort) && details[kDetailSrcPort].IsInt())
    new_flow.src_port_ = details[kDetailSrcPort].GetInt();
  if (details.HasMember(kDetailDstPort) && details[kDetailDstPort].IsInt())
    new_flow.dst_port_ = details[kDetailDstPort].GetInt();
  if (details.HasMember(kDetailDuration) && details[kDetailDuration].IsInt())
    new_flow.duration_ = details[kDetailDuration].GetInt();

  // TODO(aka): HACK if duration is not set in allocation request!
  new_flow.duration_ =
      (new_flow.duration_ > 0) ? new_flow.duration_ : info.duration_;

#if DEBUG_MUTEX_LOCK
  warnx("conga_process_post_allocations: requesting flow list lock.");
#endif
  pthread_mutex_lock(flow_list_mtx);
  flows->push_back(new_flow);

  // Note, we don't bother saving state now, as this flow has not yet
  // been allocated.

#if DEBUG_MUTEX_LOCK
  warnx("conga_process_post_allocations: releasing flow list lock.");
#endif
  pthread_mutex_unlock(flow_list_mtx);

  logger.Log(LOG_INFO, "Received new POST allocations "
             "(%s:%s, %s:%s, %s:%s, %s:%d) from %s (%d).",
             kDetailAPIKey, new_flow.api_key_.c_str(),
             kDetailSrcIP, new_flow.src_ip_.c_str(),
             kDetailDstIP, new_flow.dst_ip_.c_str(),
             kDetailRate, new_flow.rate_,
             peer->hostname().c_str(), new_flow.peer_);
  
  return "";  // head back to main event-loop
}

// Routine to delete a flow.
string conga_process_delete_allocations(const ConfInfo& info, 
                                        const HTTPFraming& http_hdr,
                                        const string& msg_body,
                                        const File& msg_data,
                                        list<AuthInfo>* authenticators, 
                                        pthread_mutex_t* authenticators_mtx,
                                        list<FlowInfo>* flows,
                                        pthread_mutex_t* flow_list_mtx,
                                        list<TCPSession>::iterator peer) {
  URL url = http_hdr.uri();

  // Get the allocation_id the user specified in the RESTful request.
  size_t last_slash = url.path().find_last_of("/");
  string allocation_id = url.path().substr(last_slash + 1);
  if (allocation_id.size() == 0) {
    error.Init(EX_DATAERR, "conga_process_delete_allocations(): "
               "No Allocation-ID found");
    return "";
  }

  // Parse JSON message-body.
  rapidjson::Document details;
  if (details.Parse(msg_body.c_str()).HasParseError()) {
    error.Init(EX_DATAERR, "conga_process_delete_allocations(): "
               "Failed to parse JSON: %s", msg_body.c_str());
    return "";
  }

  // Make sure our API key is valid.
  if (!details.HasMember(kDetailAPIKey) || !details[kDetailAPIKey].IsString()) {
    error.Init(EX_DATAERR, "conga_process_delete_allocations(): "
               "%s is invalid: %s", kDetailAPIKey, msg_body.c_str());
    return "";
  } 

#if DEBUG_MUTEX_LOCK
  warnx("conga_process_delete_allocations: requesting api keys lock.");
#endif
  pthread_mutex_lock(authenticators_mtx);
  list<AuthInfo>::iterator api_key_itr = authenticators->begin();
  while (api_key_itr != authenticators->end()) {
    string key = api_key_itr->api_key_;
    // API key is *not* case-sensitive!
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (!key.compare(details[kDetailAPIKey].GetString()))
      break;

    api_key_itr++;
  }

  if (api_key_itr == authenticators->end()) {
    // We have no record of this key (anymore?).
    error.Init(EX_DATAERR, "conga_process_delete_allocations(): "
               "API Key: %s, not found", details[kDetailAPIKey].GetString());
#if DEBUG_MUTEX_LOCK
    warnx("conga_process_delete_allocations: releasing api keys lock.");
#endif
    pthread_mutex_unlock(authenticators_mtx);
    return "";
  }

  AuthInfo our_auth(*api_key_itr);  // save a copy so we can release our lock

#if DEBUG_MUTEX_LOCK
  warnx("conga_process_delete_allocations: releasing api keys lock.");
#endif
  pthread_mutex_unlock(authenticators_mtx);

  logger.Log(LOG_DEBUG, "conga_process_delete_allocations(): "
             "working on user: %s, key: %s, project: %s, resource: %s "
             "and possible allocation id: %s.",
             our_auth.user_id_.c_str(), our_auth.api_key_.c_str(),
             our_auth.project_id_.c_str(), our_auth.resource_id_.c_str(),
             allocation_id.c_str());

  // Find the flow that matches our allocation_id ...
#if DEBUG_MUTEX_LOCK
  warnx("conga_process_delete_allocations: requesting flow list lock.");
#endif
  pthread_mutex_lock(flow_list_mtx);
  list<FlowInfo>::iterator flow_itr = flows->begin();
  while (flow_itr != flows->end()) {
    string key = flow_itr->allocation_id_;
    // Allocation ID is *not* case-sensitive!  (Is it even alpha-numeric?)
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (!key.compare(allocation_id))
      break;

    flow_itr++;
  }

  string ret_msg(kHTTPMsgBodyMaxSize, '\0');
  int status = 0;

  // Act depending on whether or not we found a flow matching our id ...
  if (flow_itr == flows->end()) {
#if DEBUG_MUTEX_LOCK
    warnx("conga_process_delete_allocations: releasing flow list lock.");
#endif
    pthread_mutex_unlock(flow_list_mtx);

    // Note, this is not necessarily an ERROR, so respond normally,
    // but with the error field filled.

    string err_msg = "Allocation ID not found in Flow table";

    // Build response.
    snprintf((char*)ret_msg.c_str(), kHTTPMsgBodyMaxSize - 1,
             "{ \"status\":%d, \"results\": [ { "
             "\"%s\":\"%s\""
             "} ] \"errors\":\"%s\" }", 
             status,
             kDetailAllocationID, allocation_id.c_str(),
             err_msg.c_str());
  } else {
    // Found our flow, so remove it from our list.
    FlowInfo our_flow(*flow_itr);  // save a copy before blowing it away
    flows->erase(flow_itr);

#if DEBUG_MUTEX_LOCK
    warnx("conga_process_delete_allocations: releasing flow list lock.");
#endif
    pthread_mutex_unlock(flow_list_mtx);

    // Build response.
    snprintf((char*)ret_msg.c_str(), kHTTPMsgBodyMaxSize - 1,
             "{ \"status\":%d, \"results\": [ { "
             "\"%s\":\"%s\""
             "} ] \"errors\": [ ] }", 
             status,
             kDetailAllocationID, our_flow.allocation_id_.c_str());
  }

  logger.Log(LOG_NOTICE, "Processed DELETE allocations (%s:%s, %s:%s) from %s.",
             kDetailAllocationID, allocation_id.c_str(),
             kDetailAPIKey, our_auth.api_key_.c_str(),
             peer->hostname().c_str());

  // Head back to conga_process_incoming_msg() to send RESPONSE out.
  return ret_msg;
}

// Routine to either return information about a specific flow, or to
// return the allocation ids for all flows associated with an API key
// (user/grant/resource).
string conga_process_get_allocations(const ConfInfo& info,
                                     const HTTPFraming& http_hdr,
                                     const string& msg_body,
                                     const File& msg_data,
                                     list<AuthInfo>* authenticators, 
                                     pthread_mutex_t* authenticators_mtx,
                                     list<FlowInfo>* flows,
                                     pthread_mutex_t* flow_list_mtx,
                                     list<TCPSession>::iterator peer) {
  URL url = http_hdr.uri();

  // Get the allocation_id if the user specified one in the RESTful request.

#if 0  // Deprecated code: allocation_id use to be in the query as opposed to the path.
  list<struct url_query_info> queries = url.query();
  list<struct url_query_info>::iterator itr = queries.begin();
  while (itr != queries.end()) {
    string key = itr->key;
    // API key is *not* case-sensitive!
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (!key.compare(kDetailAllocationID)) {
      request_info->allocation_id_ = itr->value;
    } else {
      logger.Log(LOG_WARNING, "conga_process_get_allocations(): Received unknown query: %s=%s.",
                 key.c_str(), itr->value.c_str());
    }

    itr++;
  }
#endif

  size_t last_slash = url.path().find_last_of("/");
  string allocation_id = url.path().substr(last_slash + 1);

  // Parse JSON message-body.
  rapidjson::Document details;
  if (details.Parse(msg_body.c_str()).HasParseError()) {
    error.Init(EX_DATAERR, "conga_process_get_allocations(): "
               "Failed to parse JSON: %s", msg_body.c_str());
    return "";
  }

  // Make sure our API key is valid.
  if (!details.HasMember(kDetailAPIKey) || !details[kDetailAPIKey].IsString()) {
    error.Init(EX_DATAERR, "conga_process_get_allocations(): TODO(aka) "
               "%s, %s, %s, %s or %s is invalid: %s", 
               kDetailAPIKey, kDetailProjectID, 
               kDetailSrcIP, kDetailDstIP, kDetailRate, msg_body.c_str());
    return "";
  } 

#if DEBUG_MUTEX_LOCK
  warnx("conga_process_get_allocations: requesting api keys lock.");
#endif
  pthread_mutex_lock(authenticators_mtx);
  list<AuthInfo>::iterator api_key_itr = authenticators->begin();
  while (api_key_itr != authenticators->end()) {
    string key = api_key_itr->api_key_;
    // API key is *not* case-sensitive!
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (!key.compare(details[kDetailAPIKey].GetString()))
      break;

    api_key_itr++;
  }

  if (api_key_itr == authenticators->end()) {
    // We have no record of this key (anymore?).
    error.Init(EX_DATAERR, "conga_process_get_allocations(): "
               "API Key: %s, not found", details[kDetailAPIKey].GetString());
#if DEBUG_MUTEX_LOCK
    warnx("conga_process_get_allocations: releasing api keys lock.");
#endif
    pthread_mutex_unlock(authenticators_mtx);
    return "";
  }

  AuthInfo our_auth(*api_key_itr);  // save a copy so we can release our lock

#if DEBUG_MUTEX_LOCK
  warnx("conga_process_get_allocations: releasing api keys lock.");
#endif
  pthread_mutex_unlock(authenticators_mtx);

  logger.Log(LOG_DEBUG, "conga_process_get_allocations(): "
             "working on user: %s, project: %s, resource: %s "
             "and possible allocation id: %s.",
             our_auth.user_id_.c_str(), our_auth.project_id_.c_str(),
             our_auth.resource_id_.c_str(), allocation_id.c_str());

  string ret_msg(kHTTPMsgBodyMaxSize, '\0');

  // See if we want one or all ...
  if (allocation_id.size() > 0) {
    // Find the flow that matches our allocation_id ...
#if DEBUG_MUTEX_LOCK
    warnx("conga_process_get_allocations: requesting flow list lock.");
#endif
    pthread_mutex_lock(flow_list_mtx);
    list<FlowInfo>::iterator flow_itr = flows->begin();
    while (flow_itr != flows->end()) {
      string key = flow_itr->allocation_id_;
      // Allocation ID is *not* case-sensitive!  (Is it even alpha-numeric?)
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      if (!key.compare(allocation_id))
        break;

      flow_itr++;
    }

    if (flow_itr == flows->end()) {
      error.Init(EX_DATAERR, "conga_process_get_allocations(): "
                 "Allocation ID: %s, not found in flow table",
                 allocation_id.c_str());
#if DEBUG_MUTEX_LOCK
      warnx("conga_process_get_allocations: releasing flow list lock.");
#endif
      pthread_mutex_unlock(flow_list_mtx);
      return "";
    }

    FlowInfo our_flow(*flow_itr);  // save a copy, so we can release the lock

#if DEBUG_MUTEX_LOCK
    warnx("conga_process_get_allocations: releasing flow list lock.");
#endif
    pthread_mutex_unlock(flow_list_mtx);

    logger.Log(LOG_EMERG, "conga_process_get_allocations(): XXX TODO(aka) Add code to get status!");

    string state = "running";
    int status = 0;
    
    // Build response.
    snprintf((char*)ret_msg.c_str(), kHTTPMsgBodyMaxSize - 1,
             "{ \"status\":%d, \"results\": [ { "
             "\"%s\":\"%s\", "
             "\"%s\":\"%s\", "
             "\"%s\":%d, "
             "\"%s\":\"%s\", \"%s\":\"%s\", "
             "\"%s\":\"%s\", \"%s\":%d, "
             "\"%s\":\"%s\", \"%s\":%d, "
             "\"%s\":%d"
             "} ] }", 
             status,
             kDetailAllocationID, our_flow.allocation_id_.c_str(),
             kDetailState, state.c_str(), 
             kDetailRate, our_flow.rate_,
             kDetailUserID, our_auth.user_id_.c_str(),
             kDetailProjectID, our_auth.project_id_.c_str(),
             kDetailSrcIP, our_flow.src_ip_.c_str(),
             kDetailSrcPort, our_flow.src_port_,
             kDetailDstIP, our_flow.dst_ip_.c_str(),
             kDetailDstPort, our_flow.dst_port_,
             kDetailRate, our_flow.rate_);

    logger.Log(LOG_NOTICE, "Processed GET allocations (%s:%s, %s:%s) from %s.",
               kDetailAllocationID, our_flow.allocation_id_.c_str(),
               kDetailAPIKey, our_flow.api_key_.c_str(),
               peer->hostname().c_str());
  } else {
    // User wants to list all flows that they are associated with her key.

    int status = 0;  // TODO(aka) how do we update status if something fails below?

    snprintf((char*)ret_msg.c_str(), kHTTPMsgBodyMaxSize - 1,
             "{\"status\":%d, \"results\": [", status);

    // Loop over all flows that use our API key ...
#if DEBUG_MUTEX_LOCK
    warnx("conga_process_get_allocations: requesting flow list lock.");
#endif
    bool existing_element = false;
    pthread_mutex_lock(flow_list_mtx);
    list<FlowInfo>::iterator flow_itr = flows->begin();
    while (flow_itr != flows->end()) {
      string key = flow_itr->api_key_;
      // API key is *not* case-sensitive!
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      if (!key.compare(our_auth.api_key_)) {

        logger.Log(LOG_EMERG, "conga_process_get_allocations(): XXX TODO(aka) Add code to get status!");
        string state = "running";

        // Add flow info to response message.
        if (existing_element)
          snprintf((char*)ret_msg.c_str() + strlen(ret_msg.c_str()),
                   (kHTTPMsgBodyMaxSize - 1) - strlen(ret_msg.c_str()),
                   ", ");  // add JSON element separater

        snprintf((char*)ret_msg.c_str() + strlen(ret_msg.c_str()),
                 (kHTTPMsgBodyMaxSize - 1) - strlen(ret_msg.c_str()),
                 "{\"%s\":\"%s\", "
                 "\"%s\":\"%s\", "
                 "\"%s\":%d, "
                 "\"%s\":\"%s\", \"%s\":\"%s\", "
                 "\"%s\":\"%s\", \"%s\":%d, "
                 "\"%s\":\"%s\", \"%s\":%d, "
                 "\"%s\":%d}",
                 kDetailAllocationID, flow_itr->allocation_id_.c_str(),
                 kDetailState, state.c_str(), 
                 kDetailRate, flow_itr->rate_,
                 kDetailUserID, our_auth.user_id_.c_str(),
                 kDetailProjectID, our_auth.project_id_.c_str(),
                 kDetailSrcIP, flow_itr->src_ip_.c_str(),
                 kDetailSrcPort, flow_itr->src_port_,
                 kDetailDstIP, flow_itr->dst_ip_.c_str(),
                 kDetailDstPort, flow_itr->dst_port_,
                 kDetailRate, flow_itr->rate_);
        existing_element = true;
      }

      flow_itr++;
    }  // while (flow_itr != flows->end()) {

    snprintf((char*)ret_msg.c_str() + strlen(ret_msg.c_str()),
             (kHTTPMsgBodyMaxSize - 1) - strlen(ret_msg.c_str()), "]}");

#if DEBUG_MUTEX_LOCK
    warnx("conga_process_get_allocations: releasing flow list lock.");
#endif
    pthread_mutex_unlock(flow_list_mtx);

    logger.Log(LOG_NOTICE, "Processed GET allocations (%s:%s, %s:%s) from %s.",
               kDetailAPIKey, our_auth.api_key_.c_str(),
               kDetailProjectID, our_auth.project_id_.c_str(),
               peer->hostname().c_str());
  }

  // Head back to conga_process_incoming_msg() to send RESPONSE out.
  return ret_msg;
}

// Routine to assign (or request) a meter be assigned to a flow.
//
// Note, this routine blocks on the waiting HTTP response.
size_t conga_request_stats_flowentry_modify(const ConfInfo& info,
                                            const string& dpid,
                                            const SwitchInfo& controller, 
                                            const MeterInfo& meter,
                                            const time_t now,
                                            size_t allocation_id_cnt,
                                            SSLContext* ssl_context,
                                            list<FlowInfo>::iterator flow_itr) {
  // Setup a client connection to the Ryu controller.
  TCPSession tmp_session(MsgHdr::TYPE_HTTP);
  tmp_session.Init();  // set aside buffer space
  tmp_session.SSLConn::Init(kRyuControllerName, AF_INET, 
                            IPCOMM_DNS_RETRY_CNT);  // init IPComm base class
  tmp_session.set_port(kRyuControllerPort);
  tmp_session.set_blocking();
  tmp_session.Socket(PF_INET, SOCK_STREAM, 0, NULL);
  if (error.Event()) {
    error.AppendMsg("conga_request_stats_flowentry_modify():");
    return allocation_id_cnt;
  }

  // Build a (HTTP) framing header and load the framing header into
  // our TCPSession's MsgHdr list.  Sample request:
  //
  // 0000: POST /stats/flowentry/modify HTTP/1.1
  // 0027: Host: tango.psc.edu:8080
  // 0041: User-Agent: curl/7.43.0
  // 005a: Accept: */*
  // 0067: Content-Length: 384
  // 007c: Content-Type: application/x-www-form-urlencoded
  // 00ad: 
  // => Send data, 384 bytes (0x180)
  // 0000: {"dpid": 1229782937975278821, "table_id": 30, "idle_timeout": 0,
  // 0040:  "hard_timeout": 0, "priority": 100, "flags": 0, "match":{"eth_t
  // 0080: ype": 2048, "vlan_vid": 4010,  "nw_dst":"10.10.4.20", "priority"
  // 00c0: :100 }, "actions":[{ "type": "METER", "meter_id": 210}, { "type"
  // 0100: : "OUTPUT", "port": 1 }, { "type": "SET_FIELD",  "field": "vlan_
  // 0140: "vid", "value": 8106 }, {"type": "SET_QUEUE", "queue_id": 0 } ] }
  //
  // curl --trace-ascii ./curl.trace -X POST -d '{"dpid": 1229782937975278821, "table_id": 30, "idle_timeout": 0, "hard_timeout": 0, "priority": 100, "flags": 0, "match":{"eth_type": 2048, "vlan_vid": 4010,  "nw_dst":"10.10.4.20", "priority":100 }, "actions":[{ "type": "METER", "meter_id": 210}, { "type": "OUTPUT", "port": 1 }, { "type": "SET_FIELD",  "field": "vlan_vid", "value": 8106 }, {"type": "SET_QUEUE", "queue_id": 0 } ] }' http://tango.psc.edu:8080/stats/flowentry/modify

  char path_buf[kURLMaxSize];
  snprintf(path_buf, kURLMaxSize - 1, "%s/%s/%s",
           kRyuQueryStats, kRyuQueryFlowentry, kRyuQueryModify);
  URL query_url;
  query_url.Init("http", kRyuControllerName, kRyuControllerPort,
                 path_buf, strlen(path_buf), NULL, 0, NULL);
  HTTPFraming query_http_hdr;
  query_http_hdr.InitRequest(HTTPFraming::POST, query_url);

  // Build message-body.
  char tmp_body[kHTTPMsgBodyMaxSize];

  // Note, eth_type may need to be variable, same with vlan_vid
  snprintf(tmp_body, kHTTPMsgBodyMaxSize - 1,
           "{\"dpid\": %s, \"table_id\": 30, \"idle_timeout\": 0, "
           "\"hard_timeout\": 0, \"priority\": 100, \"flags\": 0, "
           "\"match\":{\"eth_type\": %d, \"vlan_vid\": %d, "
           "\"nw_dst\": \"%s\", \"priority\": 100}, "
           "\"actions\":[{\"type\": \"METER\", \"meter_id\": %d}, "
           "{\"type\": \"OUTPUT\", \"port\": 1}, "
           "{\"type\": \"SET_FIELD\", \"field\": \"vlan_vid\", "
           "\"value\": 8106}, "
           "{\"type\": \"SET_QUEUE\", \"queue_id\": 0}]}",
           dpid.c_str(), controller.dl_type_, controller.lan_vlan_,
           flow_itr->dst_ip_.c_str(), meter.meter_);

  // Add HTTP message-headers (for host, Accept and content type & length).
  struct rfc822_msg_hdr mime_msg_hdr;
  mime_msg_hdr.field_name = MIME_HOST;
  char tmp_value[kURLMaxSize];
  snprintf(tmp_value, kURLMaxSize - 1, "%s:%hu", 
           kRyuControllerName, kRyuControllerPort);
  mime_msg_hdr.field_value = tmp_value;
  query_http_hdr.AppendMsgHdr(mime_msg_hdr);

#if 0  // Add Accept: */* header
  mime_msg_hdr.field_name = MIME_ACCEPT;
  snprintf(tmp_value, kURLMaxSize - 1, "*/*");
  mime_msg_hdr.field_value = tmp_value;
  query_http_hdr.AppendMsgHdr(mime_msg_hdr);
#endif

  mime_msg_hdr.field_name = MIME_CONTENT_TYPE;
  mime_msg_hdr.field_value = MIME_APP_X_WWW_FORM_URLENCODED;
  query_http_hdr.AppendMsgHdr(mime_msg_hdr);

  mime_msg_hdr.field_name = MIME_CONTENT_LENGTH;
  char tmp_buf[64];
  snprintf(tmp_buf, 64, "%ld", (long)strlen(tmp_body));
  mime_msg_hdr.field_value = tmp_buf;
  query_http_hdr.AppendMsgHdr(mime_msg_hdr);

  logger.Log(LOG_NOTICE, "conga_request_stats_flowentry_modify(): Generated HTTP message:\n%s%s", query_http_hdr.print_hdr(0, true).c_str(), tmp_body);  // XXX change to DEBUG

  // Setup opaque MsgHdr for TCPSession, and add HTTP header to it.
  MsgHdr tmp_msg_hdr(MsgHdr::TYPE_HTTP);
  tmp_msg_hdr.Init(++msg_id_hash, query_http_hdr);
  tmp_session.AddMsgBuf(query_http_hdr.print_hdr(0, true).c_str(),
                        query_http_hdr.hdr_len(true),
                        tmp_body, query_http_hdr.msg_len(), tmp_msg_hdr);
  if (error.Event()) {
    logger.Log(LOG_ERR, "conga_request_stats_flowentry_modify(): "
               "failed to build msg: %s", error.print().c_str());
    return allocation_id_cnt;
  }

  // HACK: Normally, we would add our REQUEST message to our outgoing
  // TCPSession list (to_peers), and then go back to wait for
  // transmission in the event-loop, processing the results in
  // conga_process_response().  However, doing that would require us
  // to be able to associate the FlowInfo between the two SSL lists
  // (which might be done via the MsgHdr msg_id!).  So, for now, we're
  // just going to sequentially turn around and ask the controller for
  // a response in here, then set the appropriate flag in our
  // FlowInfo.

  logger.Log(LOG_INFO, "Sending REQUEST: \'%s\n%s\' to %s.", 
             query_http_hdr.print_start_line(false).c_str(), 
             query_http_hdr.print_msg_hdrs().c_str(), 
             tmp_session.SSLConn::print().c_str());

  // Okay, try and connect, then send out our request.
  tmp_session.Connect();
  tmp_session.Write();
  if (error.Event()) {
    error.AppendMsg("conga_request_stats_flowentry_modify(): ");
    return allocation_id_cnt;
  }

  // If we made it here, hang around to get our response ...
  bool eof = false;
  ssize_t bytes_read = 0;
  while (!eof) {
    bytes_read = tmp_session.Read(&eof);
    if (error.Event()) {
      error.AppendMsg("conga_request_stats_flowentry_modify(): ");
      return allocation_id_cnt;
    }
    if (bytes_read > 0) {
      if (!tmp_session.IsIncomingMsgInitialized())
        tmp_session.InitIncomingMsg();
      if (error.Event()) {
        error.AppendMsg("conga_request_stats_flowentry_modify(): ");
        return allocation_id_cnt;
      }

      if (tmp_session.IsIncomingMsgComplete())
        break;
    }
  }
  tmp_session.Close();

  logger.Log(LOG_DEBUG, "conga_request_stats_flowentry_modify(): Read %ld byte(s) from %s, rbuf_len: %ld, eof: %d.", bytes_read, tmp_session.hostname().c_str(), tmp_session.rbuf_len(), eof);

  // Process the response.
  if (!tmp_session.IsIncomingMsgInitialized()) {
    error.Init(EX_DATAERR, "conga_request_stats_flowentry_modify(): "
               "Not INITIALIZED: Failed to parse response from %s", 
               tmp_session.hostname().c_str());
    return allocation_id_cnt;
  }

  const MsgHdr msg_hdr = tmp_session.rhdr();
  if (msg_hdr.http_hdr().status_code() != 200) {
    error.Init(EX_DATAERR, "conga_request_stats_flowentry_modify(): "
               "Communication failure with %s: %s",
               tmp_session.hostname().c_str(),
               tmp_session.rhdr().print().c_str());
    return allocation_id_cnt;
  }

  // TODO(aka) Do we need to check for any specific message-headers or body?

  // Set our FlowInfo to show that's now associated with our chosen meter ...
  flow_itr->meter_ = meter.meter_;

  // ... and generate a unique allocation id.
  char allocation_id[64];  // assuming no more than 64 digits (64 bits
                           // ~= 20 digits)
  snprintf(allocation_id, 64, "%lu", (unsigned long)++allocation_id_cnt);
  flow_itr->allocation_id_ = allocation_id;
  flow_itr->expiration_ = flow_itr->duration_;
  
  // Head back to main's event-loop to send RESPONSE out.
  return allocation_id_cnt;
}

// Routine to encapsulate (frame) the REPONSE ERROR as a standard HTTP
// message.
//
// This routine can set an ErrorHandler event.
void conga_gen_http_error_response(const ConfInfo& info, 
                                   const HTTPFraming& http_hdr, 
                                   list<TCPSession>::iterator peer) {
  // Build ERROR message.
  string msg(1024, '\0');  // '\0' so strlen() works
  snprintf((char*)msg.c_str() + strlen(msg.c_str()),
           1024 - strlen(msg.c_str()), 
           "Unable to satisfy REQUEST \"%s\": %s",
           http_hdr.print_start_line(false).c_str(), 
           error.print().c_str());
  error.clear();

  // Setup HTTP RESPONSE message header.
  HTTPFraming ack_hdr;
  ack_hdr.InitResponse(500, HTTPFraming::CLOSE);

  // Add HTTP content-type and content-length message-headers.
  struct rfc822_msg_hdr mime_msg_hdr;
  mime_msg_hdr.field_name = MIME_CONTENT_TYPE;
  mime_msg_hdr.field_value = MIME_TEXT_PLAIN;    // XXX need to set this correctly
  struct rfc822_parameter param;
  param.key = MIME_CHARSET;
  param.value = MIME_ISO_8859_1;
  mime_msg_hdr.parameters.push_back(param);
  ack_hdr.AppendMsgHdr(mime_msg_hdr);
  if (error.Event()) {
    error.AppendMsg("conga_gen_http_error_response()");
    return;
  }

  param.key.clear();  // so we don't hose next msg-hdr
  param.value.clear();

  mime_msg_hdr.field_name = MIME_CONTENT_LENGTH;
  char tmp_buf[64];
  snprintf(tmp_buf, 64, "%ld", (long)strlen(msg.c_str())); 
  mime_msg_hdr.field_value = tmp_buf;
  ack_hdr.AppendMsgHdr(mime_msg_hdr);
  if (error.Event()) {
    error.AppendMsg("conga_gen_http_error_response()");
    return;
  }

  //logger.Log(LOG_INFO, "conga_gen_http_error_response(): Generated HTTP headers:\n%s", http_hdr.print_hdr(0).c_str());

  // Setup opaque MsgHdr for TCPSession, and add HTTP header to it.
  MsgHdr ack_msg_hdr(MsgHdr::TYPE_HTTP);
  ack_msg_hdr.Init(++msg_id_hash, ack_hdr);  // HTTP has no id
  if (error.Event()) {
    error.AppendMsg("conga_gen_http_error_response()");
    return;
  }

  // And add the message to our TCPSession queue for transmission.
  peer->AddMsgBuf(ack_hdr.print_hdr(0, false).c_str(), ack_hdr.hdr_len(false), 
                  msg.c_str(), strlen(msg.c_str()), ack_msg_hdr);
  if (error.Event()) {
    error.AppendMsg("conga_gen_http_error_response()");
    return;  // AddMsgFile() throws events before updating peer
  }

  //logger.Log(LOG_INFO, "conga_gen_http_error_response(): %s is waiting transmission to %s, contents: %s", ack_hdr.print().c_str(), peer->print().c_str(), msg.c_str());

  logger.Log(LOG_ERROR, "Returning ERROR \"%s\" to %s.", 
             msg.c_str(), peer->print().c_str());
}

// Routine to encapsulate (frame) the REPONSE as a standard HTTP message.
void conga_gen_http_response(const ConfInfo& info, const HTTPFraming& http_hdr,
                             const string msg, list<TCPSession>::iterator peer) {
  // Setup HTTP RESPONSE message header.
  HTTPFraming ack_hdr;
  ack_hdr.InitResponse(200, HTTPFraming::CLOSE);

  // Add HTTP content-type and content-length message-headers.
  struct rfc822_msg_hdr mime_msg_hdr;
  mime_msg_hdr.field_name = MIME_CONTENT_TYPE;
  mime_msg_hdr.field_value = MIME_TEXT_PLAIN;
  ack_hdr.AppendMsgHdr(mime_msg_hdr);
  if (error.Event()) {
    error.AppendMsg("conga_gen_http_response()");
    return;
  }

  mime_msg_hdr.field_name = MIME_CONTENT_LENGTH;
  char tmp_buf[64];
  snprintf(tmp_buf, 64, "%ld", (long)msg.size());
  mime_msg_hdr.field_value = tmp_buf;
  ack_hdr.AppendMsgHdr(mime_msg_hdr);
  if (error.Event()) {
    error.AppendMsg("conga_gen_http_response()");
    return;
  }

  // Setup opaque MsgHdr for TCPSession, and add HTTP header to it.
  MsgHdr ack_msg_hdr(MsgHdr::TYPE_HTTP);
  ack_msg_hdr.Init(++msg_id_hash, ack_hdr);  // HTTP has no id
  if (error.Event()) {
    error.AppendMsg("conga_gen_http_response()");
    return;
  }

  // And add the message to our TCPSession queue for transmission.
  peer->AddMsgBuf(ack_hdr.print_hdr(0, false).c_str(), ack_hdr.hdr_len(false), 
                  msg.c_str(), ack_hdr.msg_len(), ack_msg_hdr);
  if (error.Event()) {
    error.AppendMsg("conga_gen_http_response()");
    return;  // AddMsgFile() throws events before updating peer
  }

  logger.Log(LOG_DEBUG, "conga_gen_http_response(): processed request %s; "
             "%s is waiting transmission to %s, contents: %s", 
             http_hdr.print_start_line(false).c_str(), 
             ack_hdr.print().c_str(), peer->print().c_str(), 
             msg.c_str());
}


#if 0  // Deprecated!

// Process the text/plain "message-body" in our message.
//
// TODO(aka) This procedure is deprecated in favor of
// conga_process_text_xml_request().
void conga_process_text_plain_request(const ConfInfo& info, 
                                     const HTTPFraming& http_hdr,
                                     const string& msg_body,
                                     const File& msg_data,
                                     RequestInfo* request_info) {
  URL url = http_hdr.uri();
  logger.Log(LOG_NOTICE, "Received HTTP REQUEST (%s), using function: %s.",
             http_hdr.print_start_line(false).c_str(), url.path().c_str());

  // Head back to conga_process_incoming_msg() to send RESPONSE out.
}

// Routine to *parse* and process the text/xml "message-body" in our
// message.
//
// TOOD(aka) We really only need the HTTPFraming header in here, not
// the entire MsgHdr ...
void conga_process_text_xml_request(const ConfInfo& info, 
                                   const HTTPFraming& http_hdr,
                                   const string& msg_body, const File& msg_data,
                                   RequestInfo* request_info) {
  error.Init(EX_DATAERR, "conga_process_text_plain_request(): "
             "No support for \'%s\' \'Content-Type\' yet: %s",
             MIME_TEXT_PLAIN, http_hdr.print_hdr(0, false).c_str());
  // Deal with ERROR & NACK in conga_process_incoming_msg().

#if 0
  if (msg_body.size() == 0) {
    error.Init(EX_DATAERR, "conga_process_text_xml_request(): msg body empty");
    return;  // deal with ERROR & NACK in conga_process_incoming_msg()
  }

  // Parse the XML message-body.
  conga_parse_xml(info, msg_body, request_info);
  if (error.Event()) {
    error.AppendMsg("conga_process_text_xml_request()");
    return;  // deal with ERROR & NACK in conga_process_incoming_msg()
  }

  // Perform the request.
  conga_process_request(info, request_info);
  if (error.Event()) {
    error.AppendMsg("conga_process_text_xml_request()");
    return;  // deal with ERROR & NACK in conga_process_incoming_msg()
  }
#endif

  // Head back to conga_process_incoming_msg() to send RESPONSE out.
}

// Routine to parse the XML message-body of a REQUEST.
void conga_parse_xml(const ConfInfo& info, const string& xml_msg, 
                    RequestInfo* request_info) {
#if 0
  // Initialize Xerces-c XML parser.
  try {
    XMLPlatformUtils::Initialize();
  } catch (const XMLException& toCatch) {
    char* err_msg = XMLString::transcode(toCatch.getMessage());
    error.Init(EX_UNAVAILABLE, "conga_parse_xml: Xerces-c init failed: %s", 
               err_msg);
    XMLString::release(&err_msg);
    return;  // deal with ERROR & NACK in conga_process_incoming_msg()
  }

  // Do actual work with Xerces-c protected within a block ...
  {
    /*
    // Sample DOM parser in Xerces-c.
    XercesDOMParser* parser = new XercesDOMParser();
    parser->setDoNamespaces(true);    // optional
    parser->setDoSchema(true);
    parser->setValidationScheme(XercesDOMParser::Val_Always);

    // For Schema w/no namespace
    parser.setExternalNoNamespaceSchemaLocation("conga-schema.xsd");
    // For Schema with namespace?
    parser.setExternalSchemaLocation(
    "http://my.com personal.xsd http://my2.com test2.xsd");

    ErrorHandler* errHandler = (ErrorHandler*) new HandlerBase();
    parser->setErrorHandler(errHandler);

    char* xmlFile = "x1.xml";

    try {
    parser->parse(xmlFile);
    }
    */

    /*
    // Sample Load/Save DOM parser in Xerces-c:
    XMLCh tempStr[100];
    XMLString::transcode("LS", tempStr, 99);
    DOMImplementation* impl = DOMImplementationRegistry::getDOMImplementation(tempStr);
    DOMLSParser* parser = ((DOMImplementationLS*)impl)->createLSParser(DOMImplementationLS::MODE_SYNCHRONOUS, 0);

    // optionally you can set some features on this builder
    if (parser->getDomConfig()->canSetParameter(XMLUni::fgDOMValidate, true))
    parser->getDomConfig()->setParameter(XMLUni::fgDOMValidate, true);
    if (parser->getDomConfig()->canSetParameter(XMLUni::fgDOMNamespaces, true))
    parser->getDomConfig()->setParameter(XMLUni::fgDOMNamespaces, true);
    if (parser->getDomConfig()->canSetParameter(XMLUni::fgDOMDatatypeNormalization, true))
    parser->getDomConfig()->setParameter(XMLUni::fgDOMDatatypeNormalization, true);

    // optionally you can implement your DOMErrorHandler (e.g. MyDOMErrorHandler)
    // and set it to the builder
    MyDOMErrorHandler* errHandler = new myDOMErrorHandler();
    parser->getDomConfig()->setParameter(XMLUni::fgDOMErrorHandler, errHandler);

    char* xmlFile = "x1.xml";
    DOMDocument *doc = 0;

    try {
    doc = parser->parseURI(xmlFile);
    } catch (const XMLException& toCatch) {
    char* message = XMLString::transcode(toCatch.getMessage());
    cout << "Exception message is: \n"
    << message << "\n";
    XMLString::release(&message);
    return -1;
    } catch (const DOMException& toCatch) {
    char* message = XMLString::transcode(toCatch.msg);
    cout << "Exception message is: \n"
    << message << "\n";
    XMLString::release(&message);
    return -1;
    } catch (...) {
    cout << "Unexpected Exception \n" ;
    return -1;
    }

    parser->release();
    delete errHandler;
    */

    // Setup Xerces-c XMLString conversion scratch space.
    size_t tmp_str_len = XML_MAX_ELEMENT_SIZE;  // TOOD(aka) take from Wrapper
    XMLCh tmp_str[tmp_str_len + 1];

    // Get a Load/Save *implementation* from the factory, then a
    // *parser* instance from the the implementation.

    XMLString::transcode("LS", tmp_str, tmp_str_len);
    DOMImplementation* impl = 
        DOMImplementationRegistry::getDOMImplementation(tmp_str);
    DOMLSParser* parser = 
        ((DOMImplementationLS*)impl)->createLSParser(
            DOMImplementationLS::MODE_SYNCHRONOUS, 0);

    // Specify some features on this parser.
    DOMConfiguration* config = parser->getDomConfig();
    if (config->canSetParameter(XMLUni::fgDOMNamespaces, true))
      config->setParameter(XMLUni::fgDOMNamespaces, true);
    if (config->canSetParameter(XMLUni::fgDOMDatatypeNormalization, true))
      config->setParameter(XMLUni::fgDOMDatatypeNormalization, true);
    if (config->canSetParameter(XMLUni::fgXercesHandleMultipleImports, true))
      config->setParameter(XMLUni::fgXercesHandleMultipleImports, true);
    if (config->canSetParameter(XMLUni::fgXercesSchema, true))
      config->setParameter(XMLUni::fgXercesSchema, true);
    if (config->canSetParameter(XMLUni::fgDOMValidate, true))
      config->setParameter(XMLUni::fgDOMValidate, true);
    //if (config->setParameter(XMLUni::fgXercesSchemaFullChecking, true))
    //config->setParameter(XMLUni::fgXercesSchemaFullChecking, true);

#if DEBUG_XML
    // For Debugging:
    logger.Log(LOG_NOTICE, "conga_parse_xml(): xml_msg: %s.", xml_msg.c_str());
#endif

    // Setup a Load/Save input and the input source (our memory
    // buffer) for the parser.

    DOMLSInput* input = ((DOMImplementationLS*)impl)->createLSInput();
    XMLByte* xml_msg_raw = (XMLByte*)xml_msg.c_str();  // XML ptr to char*
    MemBufInputSource* input_source = 
        new MemBufInputSource(xml_msg_raw,
                              xml_msg.size(),           // XMLSizeT
                              "conga_xml_parse",         // Fake SystemID
                              false                     // adoptBuffer
                              );
    input->setByteStream(input_source);  // associate mem buf with input
    input->setEncoding(XMLUni::fgUTF8EncodingString);

    // TOOD(aka) Not sure if we need to set the System ID ...
    //XMLString::transcode("foobar", tmp_str, tmp_str_len);
    //intput->setSystemId(tmp_str);

    // Parse the XML.
    DOMDocument* doc = NULL;
    try {
      // For PROFILING:
      struct timeval start_xerces_time;
      gettimeofday(&start_xerces_time, NULL);

      doc = parser->parse(input);
      if (doc == NULL) {
        error.Init(EX_DATAERR, "conga_parse_xml: Xerces-c parse() failed");
        return;  // deal with ERROR & NACK in conga_process_incoming_msg()
      }

      // For PROFILING:
      struct timeval end_xerces_time;
      gettimeofday(&end_xerces_time, NULL);
      request_info->time_xerces_parse = 
          (end_xerces_time.tv_sec + 
           (int)(end_xerces_time.tv_usec/1000000.0)) - 
          (start_xerces_time.tv_sec + 
           (int)(start_xerces_time.tv_usec/1000000.0));

      // For PROFILING:
      struct timeval start_internal_time;
      gettimeofday(&start_internal_time, NULL);

      // TODO(aka) UNTAINT XML Nodes!

      // Grab (root) elements from the DOM docuement ...
      DOMElement* root = doc->getDocumentElement();
      if (root == NULL) {
        error.Init(EX_DATAERR, 
                   "conga_parse_xml: Xerces-c getDocumentElement() failed");
        return;  // deal with ERROR & NACK in conga_process_incoming_msg()
      }
      char* root_name = XMLString::transcode(root->getNodeName());

      // ... and grab the children of root.
      DOMNodeList* children = root->getChildNodes();
      const XMLSize_t num_children = children->getLength();

#if DEBUG_XML
      // For Debugging:
      {
        string tmp_str(1024, '\0');  // '\0' so strlen() works
        for (XMLSize_t i = 0; i < num_children; i++) {
          DOMNode* child = children->item(i);
          if (child != NULL) {
            char* child_name = XMLString::transcode(child->getNodeName());
            snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
                     1024 - strlen(tmp_str.c_str()), "%s (%d)", 
                     child_name, child->getNodeType());
            XMLString::release(&child_name);
            if (i + 1 < num_children)
              snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
                       1024 - strlen(tmp_str.c_str()), ", ");
          }
        }
        logger.Log(LOG_NOTICE, "DEBUG: XML doc has %d children: %s.", 
                   num_children, tmp_str.c_str());
      }
#endif

      // Loop over all children processing them ...
      for (XMLSize_t i = 0; i < num_children; i++) {
        DOMNode* child = children->item(i);
        if (child == NULL || child->getNodeType() != DOMNode::ELEMENT_NODE) {
          logger.Log(LOG_DEBUG, "conga_parse_xml(): TOOD(aka) "
                     "node either NULL or not an ELEMENT_NODE.");
          continue;
        }

        // Get child element's name and set aside sapce for it's
        // value, if necessary.

        char* child_name = XMLString::transcode(child->getNodeName());
        char* child_value = NULL;

        /*  TODO(aka) I see no advantage in treating Node as an Element ...
        // Cast node as element.
        //DOMElement* element = (DOMElement*)child;
        DOMElement* element = dynamic_cast< xercesc::DOMElement* >(child);
        */

        // XXX TODO(aka) All of these strncasecmp() statements need to
        // be preceded with a size check to verify that both
        // child_name and the element name are the same length!
        // Moreover, all the LOG_ERROR getFirstChild() == NULLs need
        // to be made WARNS or less.

        if (!strncasecmp("conga", child_name, strlen("conga"))) {
          logger.Log(LOG_WARNING, "conga_parse_xml(): TOOD(aka) "
                     "Found a conga element using only 4 charaters");
        } else if (!strncasecmp("output-format", child_name, 
                                strlen("output-format"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_WARNING, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->output_format_ = child_value;
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): output-format: %s.", child_value);
        } else if (!strncasecmp("bundle-format", child_name, 
                                strlen("bundle-format"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_WARNING, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->bundle_format_ = child_value;

          // Lowercase the bundle-format, as it may be passed to ffmpeg.
          std::transform(request_info->bundle_format_.begin(), 
                         request_info->bundle_format_.end(), 
                         request_info->bundle_format_.begin(), ::tolower);
          //logger.Log(LOG_INFO, "conga_parse_xml(): bundle-format: %s.", child_value);
        } else if (!strncasecmp("num-gradients", child_name, 
                                strlen("num-gradients"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->num_gradients_ = atoi(child_value);
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): num-gradients: %s.", child_value);
        } else if (!strncasecmp("max-resolution", child_name, 
                                strlen("max-resolution"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->max_resolution_ = atof(child_value);
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): max-resolution: %s.", child_value);
        } else if (!strncasecmp("start-color", child_name, 
                                strlen("start-color"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->start_color_ = child_value;
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): start-color: %s.", child_value);
        } else if (!strncasecmp("end-color", child_name, 
                                strlen("end-color"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->end_color_ = child_value;
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): end-color: %s.", child_value);
        } else if (!strncasecmp("start-radius", child_name, 
                                strlen("start-radius"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->start_radius_ = atof(child_value);
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): start-radius: %s.", child_value);
        } else if (!strncasecmp("end-radius", child_name, 
                                strlen("end-radius"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->end_radius_ = atof(child_value);
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): end-radius: %s.", child_value);
        } else if (!strncasecmp("project-image", child_name, 
                                strlen("project-image"))) {
          // The presence of the element implies a value of *true*.
          request_info->project_image_ = true;
        } else if (!strncasecmp("fallout-radius", child_name, 
                                strlen("fallout-radius"))) {
          /*  TOOD(aka) Deprecated!
              DOMNode* value = child->getFirstChild();
              if (value == NULL) {
              logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
              "%s->getFirstChild(falout-radius) returned NULL.", 
              child_name);
              XMLString::release(&child_name);
              continue;
              }

              child_value = XMLString::transcode(value->getNodeValue());
              request_info->fallout_radius_ = strtod(child_value, (char**)NULL);
          */
          //logger.Log(LOG_INFO, "conga_parse_xml(): fallout-radius: %s.", child_value);
        } else if (!strncasecmp("font-type", child_name, 
                                strlen("font-type"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->font_type_ = child_value;
          //logger.Log(LOG_INFO, "conga_parse_xml(): setting font-type to: %s.", request_info->font_type_.c_str());
        } else if (!strncasecmp("font-size", child_name, 
                                strlen("font-size"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->font_size_ = atof(child_value);
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): font-size: %s.", child_value);
        } else if (!strncasecmp("legend-font-size", child_name, 
                                strlen("legend-font-size"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->legend_font_size_ = atof(child_value);
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): font-size: %s.", child_value);
        } else if (!strncasecmp("background-color", child_name, 
                                strlen("background-color"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->background_color_ = child_value;
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): background-color: %s.", child_value);
        } else if (!strncasecmp("fill-color", child_name, 
                                strlen("fill-color"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->fill_color_ = child_value;
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): fill-color: %s.", child_else);
        } else if (!strncasecmp("stroke-width", child_name, 
                                strlen("stroke-width"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->stroke_width_ = atof(child_value);
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): stroke-width: %s.", child_value);
        } else if (!strncasecmp("title", child_name, 
                                strlen("title"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->title_ = child_value;
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): title: %s.", child_value);
        } else if (!strncasecmp("legend-text", child_name, 
                                strlen("legend-text"))) {
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          child_value = XMLString::transcode(value->getNodeValue());
          request_info->legend_ = child_value;
          //logger.Log(LOG_DEBUG, "conga_parse_xml(): legend-text: %s.", child_value);
        } else if (!strncasecmp("style-range-list", child_name, 
                                strlen("style-range-list"))) {
          if (child->getNodeType() != DOMNode::ELEMENT_NODE) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s->getFirstChild() returned NULL.", child_name);
            XMLString::release(&child_name);
            continue;
          }

          // Grab the attributes style-id and legend-support, by
          // casting Node as an Element.  Note, spaces required inside
          // of <> in cast.

          int style_id = 0;
          DOMElement* child_ele = dynamic_cast< xercesc::DOMElement* >(child);
          XMLString::transcode("style-id", tmp_str, tmp_str_len);
          const XMLCh* xmlch_attribute = child_ele->getAttribute(tmp_str);
          char* style_id_ptr = XMLString::transcode(xmlch_attribute);
          style_id = strtol(style_id_ptr, (char**)NULL, 10);
          XMLString::release(&style_id_ptr);
          if (style_id == 0) {
            error.Init(EX_DATAERR, 
                       "conga_parse_xml: Xerces-c getAttribute(%s) failed", 
                       child_name);
            return;  // deal with ERROR & NACK in conga_process_incoming_msg()
          }

          int legend_support = 0;
          XMLString::transcode("legend-support", tmp_str, tmp_str_len);
          xmlch_attribute = child_ele->getAttribute(tmp_str);
          char* legend_support_ptr = XMLString::transcode(xmlch_attribute);
          legend_support = strtol(legend_support_ptr, (char**)NULL, 10);
          XMLString::release(&legend_support_ptr);

          logger.Log(LOG_DEBUG, "conga_parse_xml(): legend support for style id %d is %d, TODO(aka) Need to add to Style!", style_id, legend_support);

          // Grab the children of this element.
          DOMNodeList* sub_children = child->getChildNodes();
          const XMLSize_t num_sub_children = sub_children->getLength();

#if DEBUG_XML
          // For Debugging:
          logger.Log(LOG_NOTICE, "conga_parse_xml(): "
                     "%s has %d sub children in doc.", 
                     child_name, num_sub_children);
#endif

          // Loop over all children, grabbing sets that constitute a
          // struct style_range

          vector<struct style_range> gradients;
          struct style_range style_entry;
          XMLSize_t idx = 0;
          while (idx < num_sub_children) {
            DOMNode* sub_child = sub_children->item(idx++);
            if (sub_child == NULL) {
              logger.Log(LOG_ERROR, "conga_parse_xml(): "
                         "NULL sub child found in %s[%d]", 
                         child_name, (int)idx);
              continue;
            }

            // Get child element's name.
            char* sub_child_name = 
                XMLString::transcode(sub_child->getNodeName());
            char* sub_child_value = NULL;
            
#if DEBUG_XML
            // For Debugging:
            logger.Log(LOG_NOTICE, "conga_parse_xml(): %s[%d]: %s (%d), "
                       "num children: %ld.",
                       child_name, idx - 1, 
                       sub_child_name, sub_child->getNodeType(), 
                       sub_child->getChildNodes()->getLength());
#endif

            if (!strncasecmp("color", sub_child_name, strlen("color"))) {
              if ((idx - 1) > 0) {
                
                // Other than the first time, if we see a color
                // element, then we must be starting a new struct.

                gradients.push_back(style_entry);
              }

              style_entry.color.clear();
              style_entry.radius = 0;
              style_entry.lower_bound = 0;
              style_entry.upper_bound = 0;

              // TODO(aka) Do I need to release value?
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              style_entry.color.assign(sub_child_value);
            } else if (!strncasecmp("radius", sub_child_name, 
                                    strlen("radius"))) {
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              style_entry.radius = strtod(sub_child_value, (char**)NULL);
            } else if (!strncasecmp("lower-bound", sub_child_name, 
                                    strlen("lower-bound"))) {
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              style_entry.lower_bound = strtod(sub_child_value, (char**)NULL);
            } else if (!strncasecmp("upper-bound", sub_child_name, 
                                    strlen("upper-bound"))) {
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              style_entry.upper_bound = strtod(sub_child_value, (char**)NULL);
            } else {
              logger.Log(LOG_ERROR, "conga_parse_xml(): "
                         "TOOD(aka) "
                         "Unknown element tag in style-range-list: %s.", 
                         sub_child_name);
            }  // else if (!strncasecmp("color", sub_child_name, strlen("color"))) {

            // Clean up.
            if (sub_child_name != NULL)
              XMLString::release(&sub_child_name);
            if (sub_child_value != NULL)
              XMLString::release(&sub_child_value);
          } // while (idx < (num_sub_children - 1)) {

          //logger.Log(LOG_DEBUG, "conga_parse_xml(): Adding last HASC (%s) to Wrapper, and Wrapper to wrappers outside of while().\n", hasc.code().c_str());

          logger.Log(LOG_INFO, "conga_parse_xml(): "
                     "Adding style range with style_id: %d.", style_id);

          // Add last style range entry, and then the vector to the map!
          gradients.push_back(style_entry);
          request_info->styles_.insert(pair<int, vector<struct style_range> >(style_id, gradients));
          request_info->legend_supports_.insert(pair<int, int>(style_id, legend_support));
        } else if (!strncasecmp("fips-code-list", child_name, 
                                strlen("fips-code-list"))) {
          // Grab the children of this element.
          DOMNodeList* sub_children = child->getChildNodes();
          const XMLSize_t num_sub_children = sub_children->getLength();

          //logger.Log(LOG_INFO, "conga_parse_xml(): %s has %d sub children in doc.", child_name, num_sub_children);

          // Loop over all children, grabbing sets that constitute a
          // FIPS Wrapper ...

          Wrapper wrapper;
          FIPS fips;
          XMLSize_t idx = 0;
          while (idx < num_sub_children) {
            DOMNode* sub_child = sub_children->item(idx++);
            if (sub_child == NULL || 
                sub_child->getNodeType() != DOMNode::ELEMENT_NODE) {
              logger.Log(LOG_WARNING, "conga_parse_xml(): "
                         "TOOD(aka) sub child node either NULL "
                         "or not an ELEMENT_NODE.");
              continue;
            }

            // Get child element's name.
            char* sub_child_name = 
                XMLString::transcode(sub_child->getNodeName());
            char* sub_child_value = NULL;
            
            //logger.Log(LOG_DEBUG, "conga_parse_xml(): Working on idx %d, sub child %s, num children = %ld.", idx - 1, sub_child_name, sub_child->getChildNodes()->getLength());

            if (!strncasecmp("fips-code", sub_child_name, 
                             strlen("fips-code"))) {
              if ((idx - 1) > 0) {
                
                // Other than the first time, if we see a fips-code,
                // then we must be starting a new FIPS Wrapper entry.

                wrapper.set_fips(fips);

                //logger.Log(LOG_DEBUG, "conga_parse_xml(): Adding Wrapper (%s) to wrappers.\n", wrapper.fips()->code().c_str());

                request_info->wrappers_.push_back(wrapper);
              }

              wrapper.clear();
              fips.clear();

              // TOOD(aka) Need attribute processing!

              // TODO(aka) Do I need to release value?
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());

              // For now, let's keep this as a string.

              // TOOD(aka) Actually, the schema has it as a decimal,
              // so we need to change someplace!

              fips.set_code(sub_child_value);
            } else if (!strncasecmp("value", sub_child_name, 
                                    strlen("value"))) {
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              wrapper.set_value(strtod(sub_child_value, (char**)NULL));
            } else if (!strncasecmp("time-seq", sub_child_name, 
                                    strlen("time-seq"))) {
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              wrapper.set_time_seq(strtol(sub_child_value, (char**)NULL, 10));
            } else {
              logger.Log(LOG_ERROR, "conga_parse_xml(): "
                         "TOOD(aka) "
                         "Unknown element tag in fips-code-list: %s.", 
                         sub_child_name);
            } // else if (!strncasecmp("fips-code", sub_child_name, strlen("fips-code"))) {
            // Clean up.
            if (sub_child_name != NULL)
              XMLString::release(&sub_child_name);
            if (sub_child_value != NULL)
              XMLString::release(&sub_child_value);
          } // while (idx < (num_sub_children - 1)) {

          logger.Log(LOG_DEBUG, "conga_parse_xml(): Adding last (?) FIPS (%s) to Wrapper, and Wrapper to wrappers outside of while().", fips.code().c_str());

          wrapper.set_fips(fips);
          request_info->wrappers_.push_back(wrapper);
        } else if (!strncasecmp("hasc-code-list", child_name, 
                                strlen("hasc-code-list"))) {
          // Grab the children of this element.
          DOMNodeList* sub_children = child->getChildNodes();
          const XMLSize_t num_sub_children = sub_children->getLength();

          //logger.Log(LOG_INFO, "conga_parse_xml(): %s has %d sub children in doc.", child_name, num_sub_children);

          // Loop over all children, grabbing sets that constitute a
          // FIPS Wrapper ...

          Wrapper wrapper;
          HASC hasc;
          XMLSize_t idx = 0;
          while (idx < num_sub_children) {
            DOMNode* sub_child = sub_children->item(idx++);
            if (sub_child == NULL || 
                sub_child->getNodeType() != DOMNode::ELEMENT_NODE) {
              logger.Log(LOG_WARNING, "conga_parse_xml(): "
                         "TOOD(aka) sub child node either NULL "
                         "or not an ELEMENT_NODE.");
              continue;
            }

            // Get child element's name.
            char* sub_child_name = 
                XMLString::transcode(sub_child->getNodeName());
            char* sub_child_value = NULL;
            
            //logger.Log(LOG_DEBUG, "conga_parse_xml(): Working on idx %d, sub child %s, num children = %ld.", idx - 1, sub_child_name, sub_child->getChildNodes()->getLength());

            if (!strncasecmp("hasc-code", sub_child_name, 
                             strlen("hasc-code"))) {
              if ((idx - 1) > 0) {
                
                // Other than the first time, if we see a hasc-code,
                // then we must be starting a new HASC Wrapper entry.

                wrapper.set_hasc(hasc);

                //logger.Log(LOG_DEBUG, "conga_parse_xml(): Adding Wrapper (%s) to wrappers.\n", wrapper.hasc()->code().c_str());

                request_info->wrappers_.push_back(wrapper);
              }

              wrapper.clear();
              hasc.clear();

              // TOOD(aka) Need attribute processing!

              // TODO(aka) Do I need to release value?
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              hasc.set_code(sub_child_value);
            } else if (!strncasecmp("value", sub_child_name, 
                                    strlen("value"))) {
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              wrapper.set_value(strtod(sub_child_value, (char**)NULL));
            } else if (!strncasecmp("time-seq", sub_child_name, 
                                    strlen("time-seq"))) {
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              wrapper.set_time_seq(strtol(sub_child_value, (char**)NULL, 10));
            } else {
              logger.Log(LOG_ERROR, "conga_parse_xml(): "
                         "TOOD(aka) "
                         "Unknown element tag in hasc-code-list: %s.", 
                         sub_child_name);
            } // else if (!strncasecmp("hasc-code", sub_child_name, strlen("hasc-code"))) {
            // Clean up.
            if (sub_child_name != NULL)
              XMLString::release(&sub_child_name);
            if (sub_child_value != NULL)
              XMLString::release(&sub_child_value);
          } // while (idx < (num_sub_children - 1)) {

          //logger.Log(LOG_DEBUG, "conga_parse_xml(): Adding last HASC (%s) to Wrapper, and Wrapper to wrappers outside of while().\n", hasc.code().c_str());

          wrapper.set_hasc(hasc);
          request_info->wrappers_.push_back(wrapper);

        } else if (!strncasecmp("lonlat-list", child_name, 
                                strlen("lonlat-list"))) {
          // Grab the children of this element.
          DOMNodeList* sub_children = child->getChildNodes();
          const XMLSize_t num_sub_children = sub_children->getLength();

          //logger.Log(LOG_INFO, "conga_parse_xml(): %s has %d sub children in doc.", child_name, num_sub_children);

          // Loop over all children, grabbing sets that constitute a
          // FIPS Wrapper ...

          Wrapper wrapper;
          LonLat lonlat;
          XMLSize_t idx = 0;
          while (idx < num_sub_children) {
            DOMNode* sub_child = sub_children->item(idx++);
            if (sub_child == NULL || 
                sub_child->getNodeType() != DOMNode::ELEMENT_NODE) {
              logger.Log(LOG_WARNING, "conga_parse_xml(): "
                         "TOOD(aka) sub child node either NULL "
                         "or not an ELEMENT_NODE.");
              continue;
            }

            // Get child element's name.
            char* sub_child_name = 
                XMLString::transcode(sub_child->getNodeName());
            char* sub_child_value = NULL;
            
            //logger.Log(LOG_DEBUG, "conga_parse_xml(): Working on idx %d, sub child %s, num children = %ld.", idx - 1, sub_child_name, sub_child->getChildNodes()->getLength());

            if (!strncasecmp("longitude", sub_child_name, 
                             strlen("longitude"))) {
              if ((idx - 1) > 0) {
                
                // Other than the first time, if we see a longitude,
                // then we must be starting a new LonLat Wrapper entry.

                wrapper.set_lonlat(lonlat);

                //logger.Log(LOG_DEBUG, "conga_parse_xml(): Adding Wrapper (%s) to wrappers.\n", wrapper.lonlat()->code().c_str());

                request_info->wrappers_.push_back(wrapper);
              }

              wrapper.clear();
              lonlat.clear();

              // TOOD(aka) Need attribute processing!

              // TODO(aka) Do I need to release value?
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              lonlat.set_longitude(strtod(sub_child_value, (char**)NULL));
            } else if (!strncasecmp("latitude", sub_child_name, 
                                    strlen("latitude"))) {
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              lonlat.set_latitude(strtod(sub_child_value, (char**)NULL));
            } else if (!strncasecmp("value", sub_child_name, 
                                    strlen("value"))) {
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              wrapper.set_value(strtod(sub_child_value, (char**)NULL));
            } else if (!strncasecmp("time-seq", sub_child_name, 
                                    strlen("time-seq"))) {
              DOMNode* value = sub_child->getFirstChild();
              if (value == NULL) {
                logger.Log(LOG_ERROR, "conga_parse_xml(): "
                           "TODO(aka) %s->getFirstChild() returned NULL.", 
                           sub_child_name);
                XMLString::release(&sub_child_name);
                continue;
              }

              sub_child_value = XMLString::transcode(value->getNodeValue());
              wrapper.set_time_seq(strtol(sub_child_value, (char**)NULL, 10));
            } else {
              logger.Log(LOG_ERROR, "conga_parse_xml(): "
                         "TOOD(aka) "
                         "Unknown element tag in lonlat-list: %s.", 
                         sub_child_name);
            } // else if (!strncasecmp("hasc-code", sub_child_name, strlen("hasc-code"))) {
            // Clean up.
            if (sub_child_name != NULL)
              XMLString::release(&sub_child_name);
            if (sub_child_value != NULL)
              XMLString::release(&sub_child_value);
          } // while (idx < (num_sub_children - 1)) {

          //logger.Log(LOG_DEBUG, "conga_parse_xml(): Adding last HASC (%s) to Wrapper, and Wrapper to wrappers outside of while().\n", hasc.code().c_str());

          wrapper.set_lonlat(lonlat);
          request_info->wrappers_.push_back(wrapper);

        } else if (!strncasecmp("wrapper-raw", child_name, 
                                strlen("wrapper-raw"))) {
          // Sanity check we have the right number of (sub)children.
          DOMNodeList* sub_children = child->getChildNodes();
          const XMLSize_t num_sub_children = sub_children->getLength();

          //logger.Log(LOG_INFO, "conga_parse_xml(): %s has %d sub child(ren) in doc.", child_name, num_sub_children);

          if (num_sub_children != 1) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): TODO(aka) "
                       "%s has %d children", child_name, num_sub_children);
            XMLString::release(&child_name);
            continue;
          }

          // Grab the first child of this element (should be the value).

          // TODO(aka) Do I need to release value?
          DOMNode* value = child->getFirstChild();
          if (value == NULL) {
            logger.Log(LOG_ERROR, "conga_parse_xml(): "
                       "TODO(aka) %s->getFirstChild() returned NULL.", 
                       child_name);
            XMLString::release(&child_name);
            continue;
          }
          child_value = XMLString::transcode(value->getNodeValue());

          //logger.Log(LOG_DEBUG, "conga_parse_xml(): working with child value (len %ld): %s.", strlen(child_value), child_value);

          // Parse the child value for all the Wrappers it contains.
          int cnt = 
              wrapper_list_init_from_buf(child_value, strlen(child_value), 
                                     &request_info->wrappers_);

          logger.Log(LOG_DEBUG, "conga_parse_xml(): "
                     "Added %d Wrappers in raw form to list.", cnt);
        } else {
          error.Init(EX_DATAERR, "conga_parse_xml(): unknown element tag: %s", 
                     child_name);
          return;
        }  // else if (XMLString::equals(element->getTagName(), tmp_str)) {

        // Clean up.
        if (child_name != NULL)
          XMLString::release(&child_name);
        if (child_value != NULL)
          XMLString::release(&child_value);
      }  // for (XMLSize_t i = 0; i < num_children; i++) {

      // Additional clean up.
      if (root_name != NULL)
        XMLString::release(&root_name);

      // For PROFILING:
      struct timeval end_internal_time;
      gettimeofday(&end_internal_time, NULL);
      request_info->time_xml_processing = 
          (end_internal_time.tv_sec + 
           (int)(end_internal_time.tv_usec/1000000.0)) - 
          (start_internal_time.tv_sec + 
           (int)(start_internal_time.tv_usec/1000000.0));
      // XXX logger.Log(LOG_NOTICE, "conga_parse_xml(): PROFILING XML message parsed internally in %ds.", time_internal_diff);

#if DEBUG_WRAPPERS
      // For Debugging:
      {
        size_t tmp_str_len = 1024 * 10;
        string tmp_str(tmp_str_len, '\0');  // '\0' so strlen() works
        int index = 0;
        list<Wrapper>::iterator itr = request_info->wrappers_.begin();
        while (itr != request_info->wrappers_.end()) {
          snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
                   tmp_str_len - strlen(tmp_str.c_str()), "[%d] %s",
                   index, itr->print().c_str());
          index++;
          itr++;
          if (itr != request_info->wrappers_.end())
            snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
                     tmp_str_len - strlen(tmp_str.c_str()), ", ");
        }

        // TOOD(aka) Will logger.Log() handle an arbitrarily large char*?
        logger.Log(LOG_NOTICE, "DEBUG: Wrappers after XML parse (%ld): %s.", 
                   request_info->wrappers_.size(), tmp_str.c_str());
      }
#endif

#if DEBUG_STYLES
      // For Debugging:
      {
        string tmp_str(1024, '\0');  // '\0' so strlen() works
        map<int, vector<struct style_range> >::const_iterator styles_map_itr =
            request_info->styles_.begin();
        while (styles_map_itr != request_info->styles_.end()) {
          int legend_support = 
              request_info->legend_supports_.find(styles_map_itr->first)->second;
          snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
                   1024 - strlen(tmp_str.c_str()), "[%d:%d]", 
                   styles_map_itr->first, legend_support);
          for (int i = 0; i < (int)styles_map_itr->second.size(); i++) {
            snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
                     1024 - strlen(tmp_str.c_str()), " %s", 
                     styles_map_itr->second[i].color.c_str());
            if ((i + 1) < (int)styles_map_itr->second.size())
              snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
                       1024 - strlen(tmp_str.c_str()), ", ");
          }
          styles_map_itr++;
          if (styles_map_itr != request_info->styles_.end())
            snprintf((char*)tmp_str.c_str() + strlen(tmp_str.c_str()),
                     1024 - strlen(tmp_str.c_str()), ", ");
        }
        logger.Log(LOG_NOTICE, "DEBUG: Styles after XML parse (%ld): %s", 
                   request_info->styles_.size(), tmp_str.c_str());
      }
#endif

    } catch (const OutOfMemoryException& toCatch) {
      error.Init(EX_DATAERR, "conga_parse_xml: caught OutOfMemoryException");
      return;  // deal with ERROR & NACK in conga_process_incoming_msg()
    } catch (const DOMException& toCatch) {
      char* err_msg = XMLString::transcode(toCatch.getMessage());
      error.Init(EX_DATAERR, 
                 "conga_parse_xml(): caught DOMException code (%hd), msg: %s",
                 toCatch.code, err_msg);
      XMLString::release(&err_msg);
      return;  // deal with ERROR & NACK in conga_process_incoming_msg()
    } catch (const XMLException& toCatch) {
      char* err_msg = XMLString::transcode(toCatch.getMessage());
      error.Init(EX_DATAERR, "conga_parse_xml(): caught XMLException msg: %s",
                 err_msg);
      XMLString::release(&err_msg);
      return;  // deal with ERROR & NACK in conga_process_incoming_msg()
    } catch (...) {
      error.Init(EX_DATAERR, "conga_parse_xml: Unexpected Exception thrown");
      return;  // deal with ERROR & NACK in conga_process_incoming_msg()
    }

    //input_source.resetMemBufInputSource(xml_msg.c_str(), (XMLSize_t)msg_hdr.msg_len());
    parser->release();
    input->release();
    //delete errHandler;
  }  // block protector for Xerces-c

  XMLPlatformUtils::Terminate();

  logger.Log(LOG_DEBUG, "conga_parse_xml(): working with %ld Wrappers, "
             "format: %s, %s, %s.",
             request_info->wrappers_.size(), request_info->output_format_.c_str(), 
             request_info->start_color_.c_str(), request_info->end_color_.c_str());
#endif
}

// Routine to process a request.
void conga_process_request(const ConfInfo& info, RequestInfo* request_info) {

#if 0
  // Setup the database (only if we have HASC or FIPS codes).
  MySQLSession mysql;
  list<Wrapper>::iterator wrapper = request_info->wrappers_.begin();
  while (wrapper != request_info->wrappers_.end()) {
    // As long as we have *one* Wrapper that is not a LonLat ...
    if (!wrapper->IsLonLat() && !wrapper->IsLonLatLabel() && 
        !wrapper->IsLonLatPoly() && !wrapper->IsLonLatPath()) {
      mysql.Init();
      mysql.Connect(info.database_, info.database_user_, info.database_db_);
      if (error.Event()) {
        error.AppendMsg("conga_process_request()");
        return;
      }

      break;  // only need to initialize the database once
    }

    wrapper++;
  }
#endif

}

// Routine to encapsulate (frame) the RESPONSE as a WSDL-service
// RESPONSE message.
void conga_gen_wsdl_response(const ConfInfo& info, const RequestInfo& request_info, 
                            const HTTPFraming& http_hdr, 
                            list<TCPSession>::iterator peer) {
  error.Init(EX_SOFTWARE, "conga_gen_wsdl_response(): not supported yet");
  return;  // deal with ERROR & NACK in conga_process_incoming_msg()

  /*
  // TODO(aka) WSDL response
  POST /insuranceClaims HTTP/1.1
  Host: www.risky-stuff.com
  Content-Type: Multipart/Related; boundary=MIME_boundary; type=text/xml;
  start="<claim061400a.xml@claiming-it.com>"
  Content-Length: XXXX
  SOAPAction: http://schemas.risky-stuff.com/Auto-Claim
  Content-Description: This is the optional message description.

  --MIME_boundary
  Content-Type: text/xml; charset=UTF-8
  Content-Transfer-Encoding: 8bit
  Content-ID: <claim061400a.xml@claiming-it.com>

  <?xml version='1.0' ?>
  <SOAP-ENV:Envelope
  xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/">
  <SOAP-ENV:Body>
  <claim:insurance_claim_auto id="insurance_claim_document_id"
  xmlns:claim="http://schemas.risky-stuff.com/Auto-Claim">
  <theSignedForm href="cid:claim061400a.tiff@claiming-it.com"/>
  <theCrashPhoto href="cid:claim061400a.jpeg@claiming-it.com"/>
  <!-- ... more claim details go here... -->
  </claim:insurance_claim_auto>
  </SOAP-ENV:Body>
  </SOAP-ENV:Envelope>

  --MIME_boundary
  Content-Type: image/tiff
  Content-Transfer-Encoding: base64
  Content-ID: <claim061400a.tiff@claiming-it.com>

  ...Base64 encoded TIFF image...
  --MIME_boundary
  Content-Type: image/jpeg
  Content-Transfer-Encoding: binary
  Content-ID: <claim061400a.jpeg@claiming-it.com>

  ...Raw JPEG image..
  --MIME_boundary-- 
  */

  // Setup HTTP RESPONSE message header.
  HTTPFraming ack_hdr;
  ack_hdr.InitResponse(200, HTTPFraming::CLOSE);

  // Add HTTP content-type and content-length message-headers.
  struct rfc822_msg_hdr mime_msg_hdr;
  mime_msg_hdr.field_name = MIME_CONTENT_TYPE;
  mime_msg_hdr.field_value = MIME_TEXT_PLAIN;  // XXX need to set this correctly!
  ack_hdr.AppendMsgHdr(mime_msg_hdr);

  mime_msg_hdr.field_name = MIME_CONTENT_LENGTH;
  char tmp_buf[64];
  snprintf(tmp_buf, 64, "%ld", (long)msg.size());
  mime_msg_hdr.field_value = tmp_buf;
  ack_hdr.AppendMsgHdr(mime_msg_hdr);

  /*
    struct rfc822_parameter param;
    param.key = ;
    param.value = ;
    mime_msg_hdr.parameters.push_back(param);
  */

  // Setup opaque MsgHdr for TCPSession, and add HTTP header to it.
  MsgHdr ack_msg_hdr(MsgHdr::TYPE_HTTP);
  ack_msg_hdr.Init(++msg_id_hash, ack_hdr);  // HTTP has no id

  // And add the message to our TCPSession queue for transmission.
  peer->AddMsgBuf(ack_hdr.print_hdr(0, false).c_str(), ack_hdr.hdr_len(false), 
                  request_info.results_.c_str(), ack_hdr.msg_len(), ack_msg_hdr);

  logger.Log(LOG_DEBUG, "conga_gen_wsdl_response(): %s is waiting transmission to %s, contents: %s", ack_hdr.print().c_str(), peer->print().c_str(), request_info.results_.c_str());

  logger.Log(LOG_INFO, "Processed WSDL REQUEST (%s) from %s.", 
             http_hdr.print_start_line(false).c_str(), 
             peer->print().c_str());
}
#endif  // Deprecated!


