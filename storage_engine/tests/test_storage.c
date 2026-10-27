#include "catalog.h"
#include "scan.h"
#include "storage.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "print_data.c"

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
  Database *db = open_db("TEST_DB2");

  if (db == NULL) {
    printf("Failed to open database TEST_DB2\n");
    return 1;
  }

  if (new_table(db, "users", cols, sizeof(cols) / sizeof(cols[0])) != 0) {
    close_db(db);
    return 1;
  }

  for (size_t i = 0; i < sizeof(users) / sizeof(users[0]); i++) {
    if (db_insert_record(db, "users", &users[i], sizeof(users[i])) != 0) {
      close_db(db);
      return 1;
    }
  }

  ScanBatch batch = {0};

  if (scan_table("TEST_DB2", "users", &batch) != 0) {
    close_db(db);
    return 1;
  }

  print_batch(&batch);

  // Use your project's batch cleanup function here.
  scan_batch_free(&batch);
  close_db(db);
  return 0;
}
