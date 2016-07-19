/* $Id: SwitchInfo.cc,v 1.1 2012/02/03 13:17:10 akadams Exp $ */

// Copyright © 2010, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#include "SwitchInfo.h"

// Non-class specific defines & data structures.

// Non-class specific utility functions.

// SwitchInfo Class.

// Copy constructor, assignment and equality operator, needed for STL.

// Accessors.

// Mutators.
void SwitchInfo::clear(void) {
  name_.clear();
  domain_.clear();
  host_.clear();
  dl_type_ = -1;
  wan_vlan_ = -1;
  lan_vlan_ = -1;
  end_hosts_.clear();
}

// SwitchInfo manipulation.
void SwitchInfo::Init(const char* name, const char* domain, const char* host,
                      const int dl_type, const int wan_vlan,
                      const int lan_vlan, const list<string>& end_hosts) {
  name_ = name;
  domain_ = domain;
  host_ = host;
  dl_type_ = dl_type;
  wan_vlan_ = wan_vlan;
  lan_vlan_ = lan_vlan;
  end_hosts_ = end_hosts;
}

// Boolean checks.
