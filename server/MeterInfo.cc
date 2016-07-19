/* $Id: MeterInfo.cc,v 1.1 2012/02/03 13:17:10 akadams Exp $ */

// Copyright © 2010, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#include "MeterInfo.h"

// Non-class specific defines & data structures.

// Non-class specific utility functions.

// MeterInfo Class.

// Copy constructor, assignment and equality operator, needed for STL.

// Accessors.

// Mutators.
void MeterInfo::clear(void) {
  meter_ = -1;
  dpid_.clear();
  flow_.clear();
  rate_ = 0;
  flag_rate_.clear();
  time_ = 0;
  byte_band_count_ = 0;
  byte_in_count_ = 0;
  prev_time_ = 0;
  prev_byte_band_count_ = 0;
  prev_byte_in_count_ = 0;
}

// MeterInfo manipulation.

// Boolean checks.


// XXX Two spaces between classes within .cc file.
