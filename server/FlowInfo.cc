/* $Id: FlowInfo.cc,v 1.3 2014/02/24 18:06:00 akadams Exp $ */

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#include <stdlib.h>
#include <string.h>

#include "ErrorHandler.h"
#include "Logger.h"
#include "FlowInfo.h"

#define SCRATCH_BUF_SIZE (1024 * 4)


// Accessors & mutators.
void FlowInfo::clear(void) {
  allocation_id_.clear();

  api_key_.clear();

  bandwidth_ = 0;
  //expires_in_.clear();
  duration_ = 0;

  src_ip_.clear();
  dst_ip_.clear();

  peer_.clear();
  msg_hdr_id_ = 0;
}
