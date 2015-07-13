/* $Id: server-funcs.h,v 1.4 2014/05/21 15:19:42 akadams Exp $ */

// Server helper functions.

// Copyright © 2009, Pittsburgh Supercomputing Center (PSC).  
// See the file 'COPYRIGHT.txt' for any restrictions.

#ifndef SERVER_FUNCS_H_
#define SERVER_FUNCS_H_

#include "ConfInfo.h"

// CONGAd.
#define SERVER_VERSION "0.0.1"
#define CONF_FILE_DELIMITER '='

// Networking definitions.

// Global defaults.

// Function declarations.
int parse_command_line(int argc, char* argv[], ConfInfo* info);
void parse_conf_file(ConfInfo* info);
void usage(void);

#endif  /* #ifndef SERVER_FUNCS_H_ */
