#include "catalog.h"
#include "scan.h"
#include "storage.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_RECORDS 1000

typedef struct {
  int32_t id;
  char payload[32];
} TestRecord;

static void remove_test_database(const char *root) {
  char path[256];

  for (uint16_t table = 0; table < MAX_TABLES; table++) {
    snprintf(path, sizeof(path), "%s/stress_db/tables/table_%u.dat", root,
             (unsigned)table);
    assert(remove(path) == 0);
  }

  snprintf(path, sizeof(path), "%s/stress_db/tables", root);
  assert(rmdir(path) == 0);
  snprintf(path, sizeof(path), "%s/stress_db/catalog.dat", root);
  assert(remove(path) == 0);
  snprintf(path, sizeof(path), "%s/stress_db", root);
  assert(rmdir(path) == 0);
  assert(rmdir(root) == 0);
}

int main(void) {
  char root[] = "/tmp/mydb-test-insert-XXXXXX";
  assert(mkdtemp(root) != NULL);
  assert(setenv("MYDB_DATA_DIR", root, 1) == 0);

  ColumnMetadata columns[] = {
      {"id", TYPE_INT32, 0, sizeof(int32_t)},
      {"payload", TYPE_STRING, sizeof(int32_t), 32},
  };

  Database *db = open_db("stress_db");
  assert(db != NULL);

  for (uint16_t table = 0; table < MAX_TABLES; table++) {
    char table_name[64];
    snprintf(table_name, sizeof(table_name), "table_%u", (unsigned)table);
    assert(new_table(db, table_name, columns, 2) != NULL);

    for (int32_t record = 0; record < MAX_RECORDS; record++) {
      TestRecord value = {.id = record};
      snprintf(value.payload, sizeof(value.payload), "table=%u record=%d",
               (unsigned)table, record);
      assert(db_insert_record(db, table_name, &value, sizeof(value)) == 0);
    }
  }
  assert(db->catalogue->table_count == MAX_TABLES);

  close_db(db);
  remove_test_database(root);
  puts("test_insert_stress: ok");
  return 0;
}
