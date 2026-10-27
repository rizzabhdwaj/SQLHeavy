// scan.c — full table scan. Walks every page + slot of a table file and
// copies the raw record bytes into one contiguous buffer for the caller.

#include "scan.h"

#include "catalog.h"
#include "page_manager.h"

#include <stdlib.h>
#include <string.h>

// validates slot i and writes it to out (0 when the index/slot is out of range)
static int slot_of(const Page *page, uint16_t i, Slot *out) {
  if (i >= page->header.num_slots) {
    return 0;
  }

  Slot s = get_slot(page, i);
  if (s.offset < sizeof(PageHeader) ||
      (size_t)s.offset + s.length > page->header.free_space_start) {
    return 0;
  }

  *out = s;
  return 1;
}

// loads every record of <root>/<db>/tables/<table>.dat into memory
int scan_table(const char *db_name, const char *table_name, ScanBatch *out) {
  if (out == NULL) {
    return -1;
  }
  out->records = NULL;
  out->count = 0;
  memset(&out->meta, 0, sizeof out->meta);

  if (db_name == NULL || table_name == NULL) {
    return -1;
  }

  if (catalogue_get(db_name, table_name, &out->meta) != 0) {
    return -1;
  }

  set_target(db_name, table_name);
  if (!target_exists()) {
    return -1;
  }

  PageId latest = fetch_latest();
  if (latest == (PageId)-1) {
    return 0;
  }

  // pass 1: count records and total bytes so one block holds everything
  uint32_t count = 0;
  size_t total = 0;

  for (PageId id = 0; id <= latest; id++) {
    Page page = read_page(id);
    if (page.header.page_id != id) {
      return -1;
    }

    for (uint16_t i = 0; i < page.header.num_slots; i++) {
      Slot s;
      if (!slot_of(&page, i, &s)) {
        return -1;
      }
      count++;
      total += s.length;
    }
  }

  if (count == 0) {
    return 0;
  }

  uint8_t *block = malloc(sizeof(RawRecord) * (size_t)count + total);
  if (block == NULL) {
    return -1;
  }

  RawRecord *records = (RawRecord *)block;
  uint8_t *cursor = block + sizeof(RawRecord) * (size_t)count;
  uint32_t n = 0;

  // pass 2: copy the record bytes out of the pages
  for (PageId id = 0; id <= latest; id++) {
    Page page = read_page(id);
    if (page.header.page_id != id) {
      free(block);
      return -1;
    }

    for (uint16_t i = 0; i < page.header.num_slots; i++) {
      Slot s;
      if (!slot_of(&page, i, &s)) {
        free(block);
        return -1;
      }

      memcpy(cursor, (const uint8_t *)&page + s.offset, s.length);
      records[n].data = cursor;
      records[n].len = s.length;
      records[n].id.page_id = id;
      records[n].id.slot_id = i;
      cursor += s.length;
      n++;
    }
  }

  out->records = records;
  out->count = n;
  return 0;
}

// releases the buffer owned by the batch
void scan_batch_free(ScanBatch *batch) {
  if (batch == NULL) {
    return;
  }

  free(batch->records);
  batch->records = NULL;
  batch->count = 0;
}
