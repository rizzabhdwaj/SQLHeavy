// storage.c — the Database API the clients (Rust FFI, tests) call.
// Keeps the in-memory catalogue and routes inserts down to the page layer.

#include "storage.h"

#include "page_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// opens (creating if needed) data/<name>/ and loads its catalogue into memory
Database *open_db(const char *name) {
  Database *db = calloc(1, sizeof(Database));
  if (db == NULL) {
    return NULL;
  }

  snprintf(db->name, sizeof(db->name), "%s", name ? name : "");

  if (catalogue_create(db->name) != 0) {
    free(db);
    return NULL;
  }

  db->catalogue = catalogue_load(db->name);
  if (db->catalogue == NULL) {
    free(db);
    return NULL;
  }

  return db;
}

// frees the in-memory catalogue and the handle
void close_db(Database *db) {
  if (db == NULL) {
    return;
  }

  catalogue_free(db->catalogue);
  free(db);
}

// writes a new TableMetadata record to the catalogue file, then caches it
TableMetadata *new_table(Database *db, const char *name,
                         const ColumnMetadata *columns, uint16_t column_count) {
  if (db == NULL || db->catalogue == NULL || name == NULL) {
    return NULL;
  }
  if (db->catalogue->table_count >= MAX_TABLES || column_count > MAX_COLUMNS) {
    return NULL;
  }

  TableMetadata meta = {};
  snprintf(meta.name, sizeof(meta.name), "%s", name);
  meta.column_count = column_count;
  if (columns != NULL && column_count > 0) {
    memcpy(meta.columns, columns, column_count * sizeof(ColumnMetadata));
  }

  if (catalogue_put(db->name, &meta) != 0) {
    return NULL; // duplicate table name
  }

  TableMetadata *stored = &db->catalogue->tables[db->catalogue->table_count];
  *stored = meta;
  db->catalogue->table_count++;
  return stored;
}

// looks up a table in the in-memory catalogue (linear scan over <= MAX_TABLES)
TableMetadata *find_table(Database *db, const char *name) {
  if (db == NULL || db->catalogue == NULL || name == NULL) {
    return NULL;
  }

  for (uint16_t i = 0; i < db->catalogue->table_count; i++) {
    if (strcmp(db->catalogue->tables[i].name, name) == 0) {
      return &db->catalogue->tables[i];
    }
  }
  return NULL;
}

// points page_manager at data/<db>/tables/<table>.dat, then appends the record
int db_insert_record(Database *db, const char *table_name, const void *data,
                     uint16_t length) {
  if (find_table(db, table_name) == NULL) {
    return -1;
  }

  set_target(db->name, table_name);
  return insert_record(data, length);
}
