#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include "../core/types.h"

int server_run(const ResolvedConfig *config, int (*should_stop)(void *user_data), void *user_data);

#endif /* SERVER_SERVER_H */
