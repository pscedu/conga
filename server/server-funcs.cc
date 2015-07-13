/* $Id: server-funcs.cc,v 1.13 2014/05/21 15:19:42 akadams Exp $ */

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <err.h>
#include <inttypes.h>          // for strtol()
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>            // for getopt

#include "ErrorHandler.h"
#include "Logger.h"
#include "defines.h"            // for ASCII color strings &
                                // delimiters TOOD(aka) Is this still
                                // needed?
#include "server-funcs.h"

// CONGAd

// Static defaults.
static const char* conf_file_default = "conga.conf";
//static const ssize_t kDefaultBufSize = TCPSESSION_DEFAULT_BUFSIZE;


// Main utility functions.

// Routine to print out "usage" information.
void usage(void) {
  fprintf(stderr, "Usage: conga [-46htVvq] [-c config_file] "
          "[-d database_server]\n"
          "\t[-L log_device[[:log_level],...]\n"
          "\t[-p network port] [-u database_user]\n");
}

// Routine to parse command line options and load values into the
// global ConfInfo struct.
int parse_command_line(int argc, char* argv[], ConfInfo* info) {
  extern char* optarg;
  const char* getopt_flags = "46A:a:B:b:c:D:d:F:f:HhI:i:K:k:L:l:M:m:N:n:O:o:p:qS:s:TtU:u:Vv?";

  // Loop on argv options.
  int ch;
  while ((ch = getopt(argc, argv, getopt_flags)) != -1) {
    switch(ch) {
      case '4' :
        info->v4_enabled_ = true;
        break;

      case '6' :
        info->v6_enabled_ = true;
        break;

      case 'A' :
        // Fall-through.

      case 'a' :
        warn("parse_command_line(): option a not supported.");
        break;

      case 'B' :
        // Fall-through.

      case 'b' :
        warn("parse_command_line(): option b not supported.");
        break;

      case 'c' :  // Configuration file
        if (!optarg)
          errx(EX_CONFIG, "parse_command_line(): NULL config file.");  // die horribly

        info->conf_file_ = optarg;
        break;

      case 'D' :
        // Fall-through.

      case 'd' :  // Database IP address
        if (!optarg)
          errx(EX_CONFIG, "parse_command_line(): NULL database.");  // die horribly
        info->database_ = optarg;
        break;

      case 'E' :  // Errors are fatal
        logger.set_errors_fatal();
        break;

      case 'F' :
        // Fall-through.

      case 'f' :
        warn("parse_command_line(): option f not supported.");
        break;

      case 'H' :
        // Fall-through.

      case 'h' :  // Help
        usage();
        exit(1);
        break;

      case 'I' :  
        // Fall-through

      case 'i' : 
        warn("parse_command_line(): option i not supported.");
        break;

      case 'K' :
        // Fall-through

      case 'k' :
        warn("parse_command_line(): option k not supported.");
        break;

      case 'L' :  // Log location or mechanism
#if 0
        // TODO(aka) Before going into the background, mark that logging was set.
        if (!info->logging_set_)
          info->logging_set_ = 1;  // user explicitly *set* a logging type/level
#endif

        if (!strncasecmp("stderr", optarg, strlen("stderr")))
          info->log_to_stderr_ = 1;  // stderr explicitly set by user

        logger.set_mechanism_priority(optarg);
        break;

      case 'M' :  // Mode of operation
        // Fall through

      case 'm' :
        warn("parse_command_line(): option m not supported.");
        break;

      case 'N' :
        //fall through

      case 'n' :
        warn("parse_command_line(): option n not supported.");
        break;

      case 'O' :
        // Fall through.

      case 'o' :
        warn("parse_command_line(): option o not supported.");
        break;

      case 'P' :
        // Fall-through.

      case 'p' :  // Port number for my_url
        info->port_ = (in_port_t)atoi(optarg);
        break;

      case 'q' :  // Quite logging by one level
        logger.DecrementMechanismPriority();
        break;

      case 'S' :
        // Fall-through

      case 's' :
        warn("parse_command_line(): option s not supported.");
        break;
      
      case 'T' :
        // Fall-through

      case 't' :  // multi-Thread server
        info->multi_threaded_ = true;
        break;

      case 'U' :
        // Fall-through

      case 'u' :  // database User
        if (!optarg)
          errx(EX_CONFIG, "parse_command_line(): "
               "NULL database User.");  // die horribly

        info->database_user_ = optarg;
        break;
      
      case 'V' :  // Version
        fprintf(stdout, "%s\n", SERVER_VERSION);
        exit(0);
        break;

      case 'v' :  // set loggging to one level higher or Verbose
        logger.IncrementMechanismPriority();
        break;

      case '?' :
        // Fall-through!

      default :
        fprintf(stderr, "ERROR: unknown option: %c.\n", ch);
        usage();
        exit(1);
    }  // switch(ch)
  }  // while (ch = getopt() !- -1)

  // Modify argc & argv based on what we processed with getopt(3).
  argc -= optind;
  argv += optind;

  // TODO(aka) Test for additional command line arguments.

  if (argc)
    logger.Log(LOG_DEBUGGING, "parse_command_line(): post getopts(), argc is %d.", argc);

  return optind;
}

// Routine to parse our configuation file.
void parse_conf_file(ConfInfo* info) {
  // Load the default configuration filename *if* not set by user.
  if (strlen(info->conf_file_.c_str()) == 0)
    info->conf_file_ = conf_file_default;

  // See if the file exists.
  struct stat stat_info;
  if (stat(info->conf_file_.c_str(), &stat_info)) {
    logger.Log(LOG_VERBOSE, "parse_conf_file(): %s does not exist ...", 
               info->conf_file_.c_str());
    return;
  }

  if (stat_info.st_size == 0) {
    logger.Log(LOG_VERBOSE, "parse_conf_file(): %s is empty, not using ...", 
               info->conf_file_.c_str());
    return;
  }

  // Open file.
  FILE* fp = NULL;
  if ((fp = fopen(info->conf_file_.c_str(), "r")) == NULL) {
    logger.Log(LOG_VERBOSE, "Not using %s, fopen failed.", 
               info->conf_file_.c_str());
    return;
  }

  char buf[PATH_MAX];
  char* buf_ptr;
  char* key_ptr;
  char* val_ptr;
  char* delimit_ptr;

  // Parse each line as a "key = value" pair.
  while ((buf_ptr = fgets(buf, PATH_MAX, fp)) != NULL) {
    // Skip over preceeding whitespace.
    while (*buf_ptr == '\t' || *buf_ptr == ' ' || 
           *buf_ptr == '\n')
      buf_ptr++;

    if (*buf_ptr == '#' || *buf_ptr == '\0')
      continue;  // skip comments and empty lines

    key_ptr = buf_ptr;  // assign pointer to key

    if ((delimit_ptr = strchr(key_ptr, CONF_FILE_DELIMITER)) == NULL) {
      logger.Log(LOG_WARN, "Unable to parse %s at line: %s.", 
                 info->conf_file_.c_str(), key_ptr);
      fclose(fp);
      return;
    }

    char* key_end_ptr = delimit_ptr;
    *key_end_ptr = '\0';  // separate key from value
    delimit_ptr++;  // increment over delimiter (which is now '\0')

    // Remove trailing whitespace from key.
    key_end_ptr--;  // backup off of NULL terminator
    while (*key_end_ptr == '\t' || *key_end_ptr == ' ' || 
           *key_end_ptr == '\n' || *key_end_ptr == CONF_FILE_DELIMITER) 
      *key_end_ptr-- = '\0';

    // Remove preceeding white space *and* initial quotes from value.
    while (*delimit_ptr == '\t' || *delimit_ptr == ' ' || 
           *delimit_ptr == '\n' || *delimit_ptr == CONF_FILE_DELIMITER ||
           *delimit_ptr == '\'' || *delimit_ptr == '"')
      delimit_ptr++;

    val_ptr = delimit_ptr;  // assign pointer to value

    char* val_end_ptr = strchr(val_ptr, '\0');  // find end of value
    if (val_end_ptr == NULL) {
      logger.Log(LOG_ERR, "parse_conf_file(): val_end_ptr is NULL!");
      exit(1);  // die horribly before we start up
    }
    
    // Remove trailing whitespace *and* quotes from value.
    val_end_ptr--;
    while (*val_end_ptr == '\t' || *val_end_ptr == ' ' || 
           *val_end_ptr == '\n' || *val_end_ptr == '\'' || *val_end_ptr == '"') 
      *val_end_ptr-- = '\0';

    // Before (over-)writing any variables, first set them to defaults.
    //info->tar_path_ = TAR_CMD;

    // Switch (well, if-than-else) based on key.
#if 0
    if (!strncasecmp(KEY_FONT_PATH, key_ptr, 
                     strlen(KEY_FONT_PATH))) {
      info->font_path_ = val_ptr;
    } else if (!strncasecmp(KEY_FFMPEG_PATH, key_ptr,
                            strlen(KEY_FFMPEG_PATH))) {
      info->ffmpeg_path_ = val_ptr;
    } else if (!strncasecmp(KEY_TAR_PATH, key_ptr,
                            strlen(KEY_TAR_PATH))) {
      info->tar_path_ = val_ptr;
    } else {
      logger.Log(LOG_WARN, "parse_conf_file(): Unknown key: %s", key_ptr);
    }
#else
    logger.Log(LOG_WARN, "parse_conf_file(): Unknown key: %s", key_ptr);
#endif
  }
}
