#include <stdio.h>
#include <sqlite3.h>
#include "database.h"

static sqlite3* db;

void init_database() {
    sqlite3_open("app.db", &db);
    printf("データベース初期化完了\n");
}

void close_database() {
    sqlite3_close(db);
}
