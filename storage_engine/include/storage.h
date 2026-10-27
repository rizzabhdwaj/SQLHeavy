#ifndef STORAGE
#define STORAGE

#include "catalog.h"

#include <stdint.h>

#define NAME_LEN 20

typedef struct {
  char name[NAME_LEN];
  Catalogue *catalogue; // every table of this database, loaded from catalog.dat
} Database;

// opens data/<name>/ (creating it if absent) and loads the catalogue
Database *open_db(const char *name);
void close_db(Database *db);

// writes the table record into catalog.dat, returns the stored record
TableMetadata *new_table(Database *db, const char *name,
                         const ColumnMetadata *columns,
                         uint16_t column_count);

// catalogue record of one table, NULL when the table is unknown
TableMetadata *find_table(Database *db, const char *name);

int db_insert_record(Database *db, const char *table_name, const void *data,
                     uint16_t length);

#endif
