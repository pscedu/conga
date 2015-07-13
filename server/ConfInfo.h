/* $Id: ConfInfo.h,v 1.2 2014/05/21 15:19:42 akadams Exp $ */

// ConfInfo Class: library for configuration information.

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#ifndef CONFINFO_H_
#define CONFINFO_H_

#include <arpa/inet.h>

#include <stdint.h>

#include <string>
using namespace std;


class ConfInfo {
 public:
  ConfInfo(void) {
    v4_enabled_ = false;
    v6_enabled_ = false;
    multi_threaded_ = false;
 }

  // Update lamport time.
  void set_lamport(uint16_t old_lamport) {
    // TODO(aka) Do we need a MUTEX here?
    lamport_ = (old_lamport > lamport_) ? old_lamport + 1 : lamport_ + 1; 
  }

  // TODO(aka) For now, all data members are public ... we'll abstract
  // them later when we figure out what we need and don't need.

  in_port_t port_;
  bool v4_enabled_;           // flags to show we will use v4
  bool v6_enabled_;           // flags to show we will use v6
  int log_to_stderr_;
  string census_data_path_;
  bool multi_threaded_;       // flag to enabled multi-threading

  // For client.
  string peer_;
  in_port_t peer_port_;

  string database_;           // MySQL database's IP address (holding Geo Data)
  in_port_t database_port_;   // database's port
  string database_user_;      // database's port
  string database_db_;        // Geo Data database name (within MySQL database)
  string tar_path_;           // path to tar executible
  string conf_file_;          // file holding configuration options

  //uint8_t id_;              // this nodes process number (or id) as an int

  // The following are for experimenting with BasicFraming msg-hdrs.
  int id_;
  uint16_t lamport_;
  int established_connections_;

 protected:

 private:
};


#endif  /* #ifndef CONFINFO_H_ */
