/* $Id: FlowInfo.h,v 1.9 2014/02/24 18:06:00 akadams Exp $ */

// FlowInfo Class: meta-data for setting up flows

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#ifndef FLOWINFO_H_
#define FLOWINFO_H_

#include <arpa/inet.h>

#include <vector>
#include <list>
#include <map>
#include <string>
using namespace std;


// Non-class specific defines & data structures.

// FlowInfo Class: A class to hold all necessary meta-data to setup
// flows.
class FlowInfo {
 public:

  // TODO(aka) For now, they're all public ... we'll abstract them
  // later when we figure out what we need and don't need.

  // Flow details.
  string allocation_id_;      // unique token for this flow (TODO(aka)
                              // should be called flow_id!), but until
                              // we hear back from the ryu controller,
                              // its value is ""
  int meter_;                 // MeterInfo this flow would be managed by

  string api_key_;            // API-Key authorizing this flow (maps to AuthInfo)

  int bandwidth_;
  int duration_;
  time_t expiration_;            // used internally by conga

  string src_ip_;
  in_port_t src_port_;
  string dst_ip_;
  in_port_t dst_port_;

  time_t polled_;            // flag to show when conga requested flow stats
  uint16_t peer_;            // who requested this flow
                             // (TCPSession::handle()), TODO(aka) I'm
                             // not sure if this is used, i.e.,
                             // certainly if conga restarts its value
                             // will be meaningless ...

  //uint16_t msg_hdr_id_;      // link to message (socket) in either to_peers or from_peers  TODO(aka) Not sure if we need this ...

  // Constructor & destructor.
  FlowInfo(void) {
    // allocation_id_ is a blank, initial string
    meter_ = -1;
    expiration_ = 0;
    polled_ = 0;
    peer_ = 0;
  }

  virtual ~FlowInfo(void) { };

  // Copy constructor.
  FlowInfo(const FlowInfo& src)
  : allocation_id_(src.allocation_id_), api_key_(src.api_key_),
    src_ip_(src.src_ip_), dst_ip_(src.dst_ip_) {
    meter_ = src.meter_;
    bandwidth_ = src.bandwidth_;
    duration_ = src.duration_;
    expiration_ = src.expiration_;
    polled_ = src.polled_;
    peer_ = src.peer_;
  }

  // Accessors & mutators.
  void clear(void);

  friend size_t flow_info_list_save_state(const string filename, 
                                          const list<FlowInfo>& flows);
  friend size_t flow_info_list_load_state(const string filename,
                                          list<FlowInfo>* flows);
 protected:

 private:
  // Dummy declarations for assignment & equality operator.
  void operator =(const FlowInfo& src);
  int operator ==(const FlowInfo& other) const;
};


#endif  /* #ifndef FLOWINFO_H_ */
