#include "catalog.h"
#include "scan.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_utils.h"

ColumnMetadata cols[] = {
    {"name", TYPE_STRING, 0, 20},
    {"age", TYPE_INT32, 20, 4},
    {"salary", TYPE_INT32, 24, 4},
};

typedef struct {
  char name[20];
  int age;
  int salary;
} user_data;

user_data users[] = {
    {"Rahul", 16, 300},
    {"Rohan", 26, 9300},
    {"Raj", 36, 4000},
};
int main(void) {
  char root[] = "/tmp/mydb-test-storage-XXXXXX";
  TEST_ASSERT(mkdtemp(root) != NULL);
  TEST_ASSERT(setenv("MYDB_DATA_DIR", root, 1) == 0);

  Database *db = open_db("TEST_DB2");

  TEST_ASSERT(db != NULL);

  TEST_ASSERT(new_table(db, "users", cols, sizeof(cols) / sizeof(cols[0])) !=
              NULL);

  for (size_t i = 0; i < sizeof(users) / sizeof(users[0]); i++) {
    TEST_ASSERT(db_insert_record(db, "users", &users[i], sizeof(users[i])) ==
                0);
  }

  ScanBatch batch = {0};
  TEST_ASSERT(scan_table("TEST_DB2", "users", &batch) == 0);
  TEST_ASSERT(batch.count == sizeof(users) / sizeof(users[0]));
  for (uint32_t i = 0; i < batch.count; i++) {
    TEST_ASSERT(batch.records[i].len == sizeof(user_data));
    TEST_ASSERT(memcmp(batch.records[i].data, &users[i], sizeof(user_data)) ==
                0);
  }
  scan_batch_free(&batch);
  close_db(db);

  // check if the file exists
  char table_path[256];
  char tables_path[256];
  char catalog_path[256];
  char db_path[256];

  snprintf(table_path, sizeof(table_path), "%s/TEST_DB2/tables/users.dat",
           root);
  snprintf(tables_path, sizeof(tables_path), "%s/TEST_DB2/tables", root);
  snprintf(catalog_path, sizeof(catalog_path), "%s/TEST_DB2/catalog.dat", root);
  snprintf(db_path, sizeof(db_path), "%s/TEST_DB2", root);

  TEST_ASSERT(remove(table_path) == 0);
  TEST_ASSERT(rmdir(tables_path) == 0);
  TEST_ASSERT(remove(catalog_path) == 0);
  TEST_ASSERT(rmdir(db_path) == 0);
  TEST_ASSERT(rmdir(root) == 0);
  TEST_OK("test_storage");
  return 0;
}
