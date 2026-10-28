#ifndef SCAN_H
#define SCAN_H

#include "catalog.h"
#include "page_manager.h"

#include <stdint.h>

// physical location of one record, unique within its table
typedef struct {
  uint32_t page_id;
  uint16_t slot_id;
} RecordId;

typedef struct {
  const uint8_t *data;
  uint32_t len;
  RecordId id; // page_id + slot_id, unique to this record
} RawRecord;

typedef struct {
  RawRecord *records;
  uint32_t count;
  TableMetadata meta; // schema of the scanned table (from catalog.dat)
} ScanBatch;

// loads every record of data/<db_name>/<table_name>.dat into memory
// returns 0 on success, -1 on unknown db/table or corrupt page
// an existing but empty table gives count == 0, records == NULL
int scan_table(const char *db_name, const char *table_name, ScanBatch *out);

// reads one page of records; returns 0 for a batch, 1 at end of file, -1 on error
int scan_table_page(const char *db_name, const char *table_name, PageId page_id,
                    ScanBatch *out);

// releases everything owned by the batch and zeroes it
void scan_batch_free(ScanBatch *batch);

#endif
