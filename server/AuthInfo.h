/* $Id: AuthInfo.h,v 1.9 2014/02/24 18:06:00 akadams Exp $ */

// AuthInfo Class: library for information used during plots.

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#ifndef AUTHINFO_H_
#define AUTHINFO_H_

#include <string>
using namespace std;

#include "File.h"        // for PATH_MAX & load/save_state


// Non-class specific defines & data structures.

// AuthInfo Class: A class to hold all necessary meta-data (as an
// instance) needed to produce a plot from a request.
class AuthInfo {
 public:

  // TODO(aka) For now, they're all public ... we'll abstract them
  // later when we figure out what we need and don't need.

  // Auth details.
  string api_key_;            // unique token for this user/grant/resource
  string user_id_;
  string project_id_;         // matches grant # in auth database
  string resource_id_;
  int start_time_;            // when key created (epoch time)
  int end_time_;

  // Constructor & destructor.
  AuthInfo(void) { };

  virtual ~AuthInfo(void) { };

  // Copy constructor.
  AuthInfo(const AuthInfo& src)
  : api_key_(src.api_key_), user_id_(src.user_id_), 
    project_id_(src.project_id_), resource_id_(src.resource_id_) {
    start_time_ = src.start_time_;
    end_time_ = src.end_time_;
  }

  // Accessors & mutators.
  string print(void) const;

  void clear(void) {
    api_key_.clear();
    user_id_.clear();
    project_id_.clear();
    resource_id_.clear();
    start_time_ = 0;
    end_time_ = 0;
  }

  friend size_t auth_info_list_save_state(const string filename, 
                                          const list<AuthInfo>& authenticators);
  friend size_t auth_info_list_load_state(const string filename,
                                          list<AuthInfo>* authenticators);

 protected:

 private:
  // Dummy declarations for assignment & equality operator.
  void operator =(const AuthInfo& src);
  int operator ==(const AuthInfo& other) const;
};


#endif  /* #ifndef AUTHINFO_H_ */
