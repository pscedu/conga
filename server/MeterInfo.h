// Copyright © 2010, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#ifndef METERINFO_H_
#define METERINFO_H_

#include <stdint.h>

#include <string>
using namespace std;


/** Class for storing the current state of each flow.
 *
 *
 *  RCSID: $Id: MeterInfo.h,v 1.2 2012/05/22 16:33:27 akadams Exp $.
 *
 *  @author Andrew K. Adams <akadams@psc.edu>
 */
class MeterInfo {
 public:
  // Data members.  TODO(aka) For now, make them public.  :-(
  int meter_;                 // meter id; this and dpid_ value make it unique
  string dpid_;               // switch this meter is configured on
  string flow_;               // unused; TODO(aka) not sure if we need this
  uint64_t rate_;             // max rate (threshold, I think)
  string flag_rate_;          // rate reported in rate (I think)
  time_t time_;               // moment stats were populated in Class
  uint64_t byte_band_count_;  // bytes seen that are over threshold (I think)
  uint64_t byte_in_count_;    // bytes seen
  time_t prev_time_;               // last time stats were populated in Class
  uint64_t prev_byte_band_count_;  // last count on bytes over threshold
  uint64_t prev_byte_in_count_;    // last count on bytes seen

  /** Constructor.
   *
   */
  MeterInfo(void) {
    meter_ = -1;
    rate_ = 0;
    time_ = 0;
    byte_band_count_ = 0;
    byte_in_count_ = 0;
    prev_time_ = 0;
    prev_byte_band_count_ = 0;
    prev_byte_in_count_ = 0;
  }

  /** Constructor.
   *  @param i an integer argument.
   */
  //explicit MeterInfo(const int i);

  /** Destructor.
   *
   */
  virtual ~MeterInfo(void) {
    // For now, do nothing.
  }

  // Copy constructor, assignment and equality operator needed for STL.

  /** Copy constructor.
   *
   */
  MeterInfo(const MeterInfo& src)
  : dpid_(src.dpid_), flow_(src.flow_), flag_rate_(src.flag_rate_) {
    meter_ = src.meter_;
    rate_ = src.rate_;
    time_ = src.time_;
    byte_band_count_ = src.byte_band_count_;
    byte_in_count_ = src.byte_in_count_;
    prev_time_ = src.prev_time_;
    prev_byte_band_count_ = src.prev_byte_band_count_;
    prev_byte_in_count_ = src.prev_byte_in_count_;
  }

  // Accessors.

  // Mutators.
  void clear(void);

  // MeterInfo manipulation.

  /** Routine to *pretty-print* an object (usually for debugging).
   *
   */ 
  //string print(void) const;

  /** Routine to copy (or clone) a MeterInfo object.
   *
   * As copy constructors are usually frowned apon (except when needed
   * for the STL), a Clone() method is provided.
   *
   *  @param src the source MeterInfo to build our object from.
   */
  //void Clone(const MeterInfo& src);

  /** Routine to initialize a MeterInfo object.
   *
   *  Work beyond what is suitable for the class constructor needs to
   *  be performed.  This routine will set an ErrorHandler event if it
   *  encounters an unrecoverable error.
   *
   *  @see ErrorHandler
   */
  //void Init(void);

  /** Parse (or convert) a char* into a MeterInfo object.
   *
   *  This routine takes a pointer to a character stream and attempts
   *  to parse the stream into a MeterInfo object.  This routine will
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
  MeterInfo& operator =(const MeterInfo& src);

  /** Equality operator.
   */
  int operator ==(const MeterInfo& other) const;
};


#endif  /* #ifndef METERINFO_H_ */

