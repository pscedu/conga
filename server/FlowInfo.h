/* $Id: FlowInfo.h,v 1.9 2014/02/24 18:06:00 akadams Exp $ */

// FlowInfo Class: library for information used during plots.

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

// FlowInfo Class: A class to hold all necessary meta-data (as an
// instance) needed to produce a plot from a request.
class FlowInfo {
 public:

  // TODO(aka) For now, they're all public ... we'll abstract them
  // later when we figure out what we need and don't need.

  // Flow details.
  string api_key_;            // API-Key associated with this flow

  string user_id_;
  string project_id_;         // matches grant # in auth database
  string resource_id_;

  string allocation_id_;      // XXX not currently used?

  int bandwidth_;
  int start_time_;
  int end_time_;
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

  // Accessors & mutators.
  void clear(void);

 protected:

 private:
  // Dummy declarations for copy constructor and assignment & equality operator.
  FlowInfo(const FlowInfo& src);
  void operator =(const FlowInfo& src);
  int operator ==(const FlowInfo& other) const;
};


#endif  /* #ifndef FLOWINFO_H_ */
