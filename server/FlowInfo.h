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
  string allocation_id_;      // unique token for this flow (TODO(aka) should be call flow_id!)

  string api_key_;            // API-Key authorizing this flow (maps to AuthInfo)

  int bandwidth_;
  //string expires_in_;
  int duration_;

  string src_ip_;
  in_port_t src_port_;
  string dst_ip_;
  in_port_t dst_port_;

  // TODO(aka) Not sure if we need the two below elements ...
  string peer_;              // who requested this flow
  uint16_t msg_hdr_id_;      // link to message (socket) in either to_peers or from_peers

  // Constructor & destructor.
  FlowInfo(void) {
    msg_hdr_id_ = 0;
  }

  virtual ~FlowInfo(void) { };

  // Copy constructor.
  FlowInfo(const FlowInfo& src)
  : allocation_id_(src.allocation_id_), api_key_(src.api_key_),
    src_ip_(src.src_ip_), dst_ip_(src.dst_ip_), peer_(src.peer_) {
    bandwidth_ = src.bandwidth_;
  //expires_in_.clear();
    duration_ = src.duration_;
  }

  // Accessors & mutators.
  void clear(void);

 protected:

 private:
  // Dummy declarations for assignment & equality operator.
  void operator =(const FlowInfo& src);
  int operator ==(const FlowInfo& other) const;
};


#endif  /* #ifndef FLOWINFO_H_ */
