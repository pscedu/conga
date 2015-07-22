/* $Id: RequestInfo.h,v 1.9 2014/02/24 18:06:00 akadams Exp $ */

// RequestInfo Class: library for information used during plots.

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#ifndef REQUESTINFO_H_
#define REQUESTINFO_H_

#include <arpa/inet.h>

#include <vector>
#include <list>
#include <map>
#include <string>
using namespace std;


// Non-class specific defines & data structures.

// RequestInfo Class: A class to hold all necessary meta-data (as an
// instance) needed to produce a plot from a request.
class RequestInfo {
 public:

  // TODO(aka) For now, they're all public ... we'll abstract them
  // later when we figure out what we need and don't need.

  // Server data members (and possibly used on client, as well).
  string peer_;              // who requested this plot
  uint16_t msg_hdr_id_;         // link to message (socket) in either to_peers or from_peers
  bool wsdl_request_;        // flag to show that REQUEST expects WSDL service

  // Details.
  string api_key_;
  string allocation_id_;
  string user_id_;
  string project_id_;
  string resource_id_;
  int bandwidth_;
  int start_time_;
  int end_time_;
  //string expires_in_;
  string src_ip_;
  in_port_t src_port_;
  string dst_ip_;
  in_port_t dst_port_;
  int duration_;

  string results_;           // TODO(aka) for now, just a string to hold the results

  // Constructor & destructor.
  RequestInfo(void) {
    msg_hdr_id_ = 0;
    wsdl_request_ = false;
  }

  virtual ~RequestInfo(void) { };

  // Accessors & mutators.
  void clear(void);

 protected:

 private:
  // Dummy declarations for copy constructor and assignment & equality operator.
  RequestInfo(const RequestInfo& src);
  void operator =(const RequestInfo& src);
  int operator ==(const RequestInfo& other) const;
};


#endif  /* #ifndef REQUESTINFO_H_ */
