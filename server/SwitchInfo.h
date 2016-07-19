// Copyright © 2010, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#ifndef SWITCHINFO_H_
#define SWITCHINFO_H_

#include <string>
#include <list>
using namespace std;


/** Class for managing SDN switches.
 *
 *
 *  RCSID: $Id: SwitchInfo.h,v 1.2 2012/05/22 16:33:27 akadams Exp $.
 *
 *  @author Andrew K. Adams <akadams@psc.edu>
 */
class SwitchInfo {
 public:
  // TODO(aka) For now, make all data members public!
  string name_;         // descriptor found on SDN switch
  string domain_;       // e.g., PSC
  string host_;         // name of host to query
  int dl_type_;         // no idea, but always x800 (2048)
  int wan_vlan_;
  int lan_vlan_;
  list<string> end_hosts_;

  /** Constructor.
   *
   */
  SwitchInfo(void) {
    dl_type_ = -1;
    wan_vlan_ = -1;
    lan_vlan_ = -1;
  }

  /** Constructor.
   *  @param i an integer argument.
   */
  //explicit SwitchInfo(const int i);

  /** Destructor.
   *
   */
  virtual ~SwitchInfo(void) {
    // For now, do nothing.
  }

  // Copy constructor, assignment and equality operator needed for STL.

  /** Copy constructor.
   *
   */
  SwitchInfo(const SwitchInfo& src) 
  : name_(src.name_), domain_(src.domain_), host_(src.host_), 
    end_hosts_(src.end_hosts_) {
    dl_type_ = src.dl_type_;
    wan_vlan_ = src.wan_vlan_;
    lan_vlan_ = src.lan_vlan_;
  }

  // Accessors.
  list<string> end_hosts(void) const { return end_hosts_; }

  // Mutators.
  void clear(void);

  // SwitchInfo manipulation.

  /** Routine to *pretty-print* an object (usually for debugging).
   *
   */ 
  //string print(void) const;

  /** Routine to initialize a SwitchInfo object.
   *
   *  Work beyond what is suitable for the class constructor needs to
   *  be performed.  This routine will set an ErrorHandler event if it
   *  encounters an unrecoverable error.
   *
   *  @see ErrorHandler
   */
  void Init(const char* name, const char* domain, const char* host,
            const int dl_type, const int wan_vlan, const int lan_vlan,
            const list<string>& end_hosts);

  /** Parse (or convert) a char* into a SwitchInfo object.
   *
   *  This routine takes a pointer to a character stream and attempts
   *  to parse the stream into a SwitchInfo object.  This routine will
   *  set an ErrorHandler event if it encounters an unrecoverable
   *  error.
   *
   *  @see ErrorHandler Class
   *  @param buf is a char* stream
   *  @param len is a size_t representing the size of buf
   *  @return a size_t showing how much data from buf we used
   */
  //size_t InitFromBuf(const char* buf, const size_t len);

  // Boolean checks.

  // Flags.

 protected:
  // Data members.

 private:
  // Dummy declarations for copy constructor and assignment & equality operator.

  /** Assignment operator.
   *
   */
  SwitchInfo& operator =(const SwitchInfo& src);

  /** Equality operator.
   */
  int operator ==(const SwitchInfo& other) const;
};


#endif  /* #ifndef SWITCHINFO_H_ */

