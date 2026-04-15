#include <stdio.h>
#include "src/http/server.h"
#include "src/db/database.h"
#include "src/utils/logger.h"

int main() {
    init_logger();
    init_database();
    
    printf("C言語 Webサーバー起動中...\n");
    start_server(8080);
    
    return 0;
}
