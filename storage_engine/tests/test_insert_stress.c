#include "catalog.h"
#include "scan.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_utils.h"

#define MAX_RECORDS 5000

typedef struct {
  int32_t id;
  char payload[32];
} TestRecord;

static void remove_test_database(const char *root) {
  char path[256];

  for (uint16_t table = 0; table < MAX_TABLES; table++) {
    snprintf(path, sizeof(path), "%s/stress_db/tables/table_%u.dat", root,
             (unsigned)table);
    TEST_ASSERT(remove(path) == 0);
  }

  snprintf(path, sizeof(path), "%s/stress_db/tables", root);
  TEST_ASSERT(rmdir(path) == 0);
  snprintf(path, sizeof(path), "%s/stress_db/catalog.dat", root);
  TEST_ASSERT(remove(path) == 0);
  snprintf(path, sizeof(path), "%s/stress_db", root);
  TEST_ASSERT(rmdir(path) == 0);
  TEST_ASSERT(rmdir(root) == 0);
}

int main(void) {
  char root[] = "/tmp/mydb-test-insert-XXXXXX";
  TEST_ASSERT(mkdtemp(root) != NULL);
  TEST_ASSERT(setenv("MYDB_DATA_DIR", root, 1) == 0);

  ColumnMetadata columns[] = {
      {"id", TYPE_INT32, 0, sizeof(int32_t)},
      {"payload", TYPE_STRING, sizeof(int32_t), 32},
  };

  Database *db = open_db("stress_db");
  TEST_ASSERT(db != NULL);

  for (uint16_t table = 0; table < MAX_TABLES; table++) {
    char table_name[64];
    snprintf(table_name, sizeof(table_name), "table_%u", (unsigned)table);
    TEST_ASSERT(new_table(db, table_name, columns, 2) != NULL);

    for (int32_t record = 0; record < MAX_RECORDS; record++) {
      TestRecord value = {.id = record};
      snprintf(value.payload, sizeof(value.payload), "table=%u record=%d",
               (unsigned)table, record);
      TEST_ASSERT(db_insert_record(db, table_name, &value, sizeof(value)) == 0);
    }
  }
  TEST_ASSERT(db->catalogue->table_count == MAX_TABLES);

  close_db(db);

  // Reopen before cleanup so this verifies data persisted to disk.
  db = open_db("stress_db");
  TEST_ASSERT(db != NULL);
  TEST_ASSERT(db->catalogue->table_count == MAX_TABLES);

  for (uint16_t table = 0; table < MAX_TABLES; table++) {
    char table_name[64];
    snprintf(table_name, sizeof(table_name), "table_%u", (unsigned)table);
    TEST_ASSERT(find_table(db, table_name) != NULL);

    ScanBatch batch = {0};
    TEST_ASSERT(scan_table("stress_db", table_name, &batch) == 0);
    TEST_ASSERT(batch.count == MAX_RECORDS);

    for (uint32_t record = 0; record < batch.count; record++) {
      TestRecord expected = {.id = (int32_t)record};
      snprintf(expected.payload, sizeof(expected.payload), "table=%u record=%u",
               (unsigned)table, (unsigned)record);
      TEST_ASSERT(batch.records[record].len == sizeof(expected));
      TEST_ASSERT(
          memcmp(batch.records[record].data, &expected, sizeof(expected)) == 0);
    }

    scan_batch_free(&batch);
  }

  close_db(db);

  // Only delete the test database after every table has been read and checked.
  remove_test_database(root);
  TEST_OK("test_insert_stress: insert, reopen, read, and cleanup");
  return 0;
}
