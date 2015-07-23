/* $Id: RequestInfo.cc,v 1.3 2014/02/24 18:06:00 akadams Exp $ */

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#include <stdlib.h>
#include <string.h>

#include "ErrorHandler.h"
#include "Logger.h"
#include "RequestInfo.h"

#define SCRATCH_BUF_SIZE (1024 * 4)


// Accessors & mutators.
void RequestInfo::clear(void) {
  peer_.clear();
  wsdl_request_ = false;

  user_id_.clear();
  proejct_id_.clear();
  request_id_.clear();
  api_key_.clear();
  expires_in_.clear();
  services_.clear();
  src_ip_.clear();
  dst_ip_.clear();
  data_size_.clear();

  results_.clear();
}
