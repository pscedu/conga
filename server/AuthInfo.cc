/* $Id: AuthInfo.cc,v 1.1 2012/02/03 13:17:10 akadams Exp $ */

// Copyright © 2010, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#include <err.h>
#include <string.h>
#include <time.h>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"

#include "ErrorHandler.h"
#include "AuthInfo.h"

//static const char kAuthInfoDelimiter = ':';

static const char* kNameAuthenticators = "authenticators";
static const char* kNameAPIKey = "key";
static const char* kNameUserID = "user";
static const char* kNameProjectID = "project";
static const char* kNameResourceID = "resource";
static const char* kNameStartTime = "start";
static const char* kNameEndTime = "end";

// Non-class specific utility functions.
size_t auth_info_list_save_state(const string filename,
                                 const list<AuthInfo>& authenticators) {
  // No need for a mutex, as this only gets called from within a mutex
  // when the state changes.

  // Create JSON buffer.
  char tmp_buf[PATH_MAX];  // JSON buffer
  tmp_buf[0] = '\0';
  snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
           "{\"%s\":[", kNameAuthenticators);

  list<AuthInfo>::const_iterator itr = authenticators.begin();
  while (itr != authenticators.end()) {
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "{\"%s\":\"%s\", ", kNameAPIKey, itr->api_key_.c_str());
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":\"%s\", ", kNameUserID, itr->user_id_.c_str());
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":\"%s\", ", kNameProjectID, itr->project_id_.c_str());
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":\"%s\", ", kNameResourceID, itr->resource_id_.c_str());
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":%d, ", kNameStartTime, itr->start_time_);
    snprintf(tmp_buf + strlen(tmp_buf), PATH_MAX - strlen(tmp_buf),
             "\"%s\":%d}", kNameEndTime, itr->end_time_);
    itr++;
    if (itr != authenticators.end())
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
    error.AppendMsg("auth_info_list_load_state(): ");
    return 0;
  }

  if (n == 0) {
      error.Init(EX_SOFTWARE, "auth_info_list_save_state(): "
                 "JSON %db long, but fwrite() wrote 0 objects",
                 (int)strlen(tmp_buf));
      return 0;
  }

  return strlen(tmp_buf);
}

size_t auth_info_list_load_state(const string filename, 
                                 list<AuthInfo>* authenticators) {
  // Again, no need for a mutex, as this will only be called on
  // startup (maybe HUP?).

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
    error.AppendMsg("auth_info_list_load_state(): ");
    return 0;
  }

  if (ret == NULL) {
      error.Init(EX_SOFTWARE, "auth_info_list_load_state(): "
                 "fgets(%s) failed: %s", 
                 state_file.path(NULL).c_str(), strerror(errno));
      return 0;
  }

  // Parse our JSON.
  printf("XXX parsing: %s.\n", tmp_buf);
  // Parse JSON message-body.
  rapidjson::Document doc;
  if (doc.Parse(tmp_buf).HasParseError()) {
    error.Init(EX_DATAERR, "auth_info_list_load_state(): "
               "Failed to parse JSON in %s: %s",
               state_file.path(NULL).c_str(), tmp_buf);
    return 0;
  }

  // Make sure we have a valid JSON object.
  if (!doc.IsObject() || !doc.HasMember(kNameAuthenticators) ||
      !doc[kNameAuthenticators].IsArray()) {
    error.Init(EX_DATAERR, "auth_info_list_load_state(): "
               "Invalid JSON object in %s: %s",
               state_file.path(NULL).c_str(), tmp_buf);
    return 0;
  }

  // Loop through array building AuthInfo objects ...
  const rapidjson::Value& list = doc[kNameAuthenticators];
  AuthInfo tmp_auth;

  // RAPIDJSON: Uses SizeType instead of size_t.
  for (rapidjson::SizeType i = 0; i < list.Size(); ++i) {
    if (!list[i].HasMember(kNameAPIKey) || 
        !list[i].HasMember(kNameUserID) ||
        !list[i].HasMember(kNameProjectID) ||
        !list[i].HasMember(kNameResourceID) ||
        !list[i].HasMember(kNameStartTime) ||
        !list[i].HasMember(kNameEndTime)) {
      warnx("auth_info_list_load_state(): TODO(aka) Invalid JSON authenticators element!");
      continue;
    }

    tmp_auth.api_key_ = list[i][kNameAPIKey].GetString();
    tmp_auth.user_id_ = list[i][kNameUserID].GetString();
    tmp_auth.project_id_ = list[i][kNameProjectID].GetString();
    tmp_auth.resource_id_ = list[i][kNameResourceID].GetString();
    tmp_auth.start_time_ = list[i][kNameStartTime].GetInt();
    tmp_auth.end_time_ = list[i][kNameEndTime].GetInt();

    // Make sure this authenticator hasn't expired.
    int now = (int)time(NULL);
    if (tmp_auth.end_time_ <= now) {
      warnx("auth_info_list_load_state(): "
            "Not loading %s (%s) due to end-time (%d) expiring (now: %d).",
            tmp_auth.api_key_.c_str(), tmp_auth.user_id_.c_str(),
            tmp_auth.end_time_, now);
      continue;
    }

    authenticators->push_back(tmp_auth);
    tmp_auth.clear();
  }

  return authenticators->size();
}


