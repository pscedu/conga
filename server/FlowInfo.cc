/* $Id: FlowInfo.cc,v 1.3 2014/02/24 18:06:00 akadams Exp $ */

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"

#include "ErrorHandler.h"
#include "File.h"            // for PATH_MAX
#include "Logger.h"
#include "FlowInfo.h"

#define SCRATCH_BUF_SIZE (1024 * 4)

//static const char kFlowInfoDelimiter = ':';

static const char* kNameFlows = "flows";
static const char* kNameAllocationID = "allocation-id";
static const char* kNameMeter = "meter";
static const char* kNameAPIKey = "key";
static const char* kNameRate = "rate";
static const char* kNameDuration = "duration";
static const char* kNameExpiration = "expires";
static const char* kNameSrcIP = "src-ip";
static const char* kNameDstIP = "dst-ip";

// Non-class specific utility functions.
size_t flow_info_list_save_state(const string filename,
                                 const list<FlowInfo>& flows) {
  // No need for a mutex, as this only gets called from within a mutex
  // when the state changes.  Moreover, we're not concerned with
  // polled_ or peer_, as they only have relevance during initialization.

  // Create JSON buffer.
  char tmp_buf[PATH_MAX];  // JSON buffer
  tmp_buf[0] = '\0';
  snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
           "{\"%s\":[", kNameFlows);

  list<FlowInfo>::const_iterator itr = flows.begin();
  while (itr != flows.end()) {
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "{\"%s\":\"%s\", ", kNameAllocationID, itr->allocation_id_.c_str());
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":\"%s\", ", kNameAPIKey, itr->api_key_.c_str());
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":%d, ", kNameMeter, itr->meter_);
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":%d, ", kNameRate, itr->rate_);
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":%d, ", kNameDuration, itr->duration_);
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":%d, ", kNameExpiration, (int)itr->expiration_);
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":\"%s\", ", kNameSrcIP, itr->src_ip_.c_str());
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":\"%s\"}", kNameDstIP, itr->dst_ip_.c_str());
    itr++;
    if (itr != flows.end())
      snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf), ", ");
  }

  snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf), "]}");

  // Write buffer out to disk.
  File state_file;
  state_file.Init(filename.c_str(), NULL);
  state_file.Fopen(NULL, "w+");
  FILE* fp = state_file.fp();
  size_t n = fwrite(tmp_buf, strlen(tmp_buf), 1, fp);
  state_file.Fclose();
  if (error.Event()) {
    error.AppendMsg("flow_info_list_load_state(): ");
    return 0;
  }

  if (n == 0) {
      error.Init(EX_SOFTWARE, "flow_info_list_save_state(): "
                 "JSON %db long, but fwrite() wrote 0 objects",
                 (int)strlen(tmp_buf));
      return 0;
  }

  return strlen(tmp_buf);
}

size_t flow_info_list_load_state(const string filename, 
                                 list<FlowInfo>* flows) {
  // Again, no need for a mutex, as this will only be called on
  // startup (maybe HUP?).  Similarly, no need for polled_ or peer_
  // (see above).

  // Note, we return the *max* allocation_id, so we can use it to set
  // our static global in main().

  File state_file;
  state_file.Init(filename.c_str(), NULL);

  // See if file exists.
  if (!state_file.Exists(NULL))
    return 0;  // nothing to do

  char tmp_buf[PATH_MAX];  // JSON buffer
  tmp_buf[0] = '\0';

  // Read buffer in from disk.
  state_file.Fopen(NULL, "r");
  FILE* fp = state_file.fp();
  char* ret = fgets(tmp_buf, PATH_MAX, fp);
  state_file.Fclose();
  if (error.Event()) {
    error.AppendMsg("flow_info_list_load_state(): ");
    return 0;
  }

  if (ret == NULL) {
      error.Init(EX_SOFTWARE, "flow_info_list_load_state(): "
                 "fgets(%s) failed: %s", 
                 state_file.path(NULL).c_str(), strerror(errno));
      return 0;
  }

  // Parse our JSON.
  printf("XXX parsing: %s.\n", tmp_buf);
  // Parse JSON message-body.
  rapidjson::Document doc;
  if (doc.Parse(tmp_buf).HasParseError()) {
    error.Init(EX_DATAERR, "flow_info_list_load_state(): "
               "Failed to parse JSON in %s: %s",
               state_file.path(NULL).c_str(), tmp_buf);
    return 0;
  }

  // Make sure we have a valid JSON object.
  if (!doc.IsObject() || !doc.HasMember(kNameFlows) ||
      !doc[kNameFlows].IsArray()) {
    error.Init(EX_DATAERR, "flow_info_list_load_state(): "
               "Invalid JSON object in %s: %s",
               state_file.path(NULL).c_str(), tmp_buf);
    return 0;
  }

  // Loop through array building FlowInfo objects ...
  size_t max_id = 0;
  const rapidjson::Value& list = doc[kNameFlows];
  FlowInfo tmp_flow;

  // RAPIDJSON: Uses SizeType instead of size_t.
  for (rapidjson::SizeType i = 0; i < list.Size(); ++i) {
    if (!list[i].HasMember(kNameAllocationID) || 
        !list[i].HasMember(kNameAPIKey) || 
        !list[i].HasMember(kNameMeter) ||
        !list[i].HasMember(kNameRate) ||
        !list[i].HasMember(kNameDuration) ||
        !list[i].HasMember(kNameExpiration) ||
        !list[i].HasMember(kNameSrcIP) ||
        !list[i].HasMember(kNameDstIP)) {
      warnx("flow_info_list_load_state(): TODO(aka) Invalid JSON flows element!");
      continue;
    }

    tmp_flow.allocation_id_ = list[i][kNameAllocationID].GetString();
    tmp_flow.api_key_ = list[i][kNameAPIKey].GetString();
    tmp_flow.meter_ = list[i][kNameMeter].GetInt();
    tmp_flow.rate_ = list[i][kNameRate].GetInt();
    tmp_flow.duration_ = list[i][kNameDuration].GetInt();
    tmp_flow.expiration_ = (time_t)list[i][kNameExpiration].GetInt();
    tmp_flow.src_ip_ = list[i][kNameSrcIP].GetString();
    tmp_flow.dst_ip_ = list[i][kNameSrcIP].GetString();

    // Make sure this flow hasn't expired.
    int now = (int)time(NULL);
    if (tmp_flow.expiration_ >= now) {
      warnx("flow_info_list_load_state(): "
            "Not loading %s (%d) due to expiration (%d, now: %d).",
            tmp_flow.api_key_.c_str(), tmp_flow.meter_,
            (int)tmp_flow.expiration_, (int)now);
      continue;
    }

    flows->push_back(tmp_flow);

    size_t new_id = (size_t)strtol(tmp_flow.allocation_id_.c_str(), NULL, 10);
    if (new_id > max_id)
      max_id = new_id;

    tmp_flow.clear();
  }

  return max_id;
}

// Accessors & mutators.
void FlowInfo::clear(void) {
  allocation_id_.clear();
  meter_ = -1;

  api_key_.clear();

  rate_ = 0;
  duration_ = 0;
  expiration_ = 0;

  src_ip_.clear();
  dst_ip_.clear();

  polled_ = 0;
  peer_ = 0;
}
