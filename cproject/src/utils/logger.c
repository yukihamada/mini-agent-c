#include <stdio.h>
#include <time.h>
#include "logger.h"

void init_logger() {
    printf("ログシステム初期化\n");
}

void log_info(const char* message) {
    time_t now = time(NULL);
    printf("[%s] INFO: %s\n", ctime(&now), message);
}

void log_error(const char* message) {
    time_t now = time(NULL);
    printf("[%s] ERROR: %s\n", ctime(&now), message);
}
