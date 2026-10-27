// catalog.c — "what tables exist". The catalogue is itself stored in the
// normal page/slot record format: one TableMetadata record per table inside
// <root>/<db>/catalog.dat. Only metadata lives here, never table rows.

#include "catalog.h"

#include "page_manager.h"

#include <stdlib.h>
#include <string.h>

// copies record i of the current target file into out, validating its slot
static int copy_meta(const Page *page, uint16_t i, TableMetadata *out) {
  Slot s = get_slot(page, i);

  if (s.length != sizeof(TableMetadata) || s.offset < sizeof(PageHeader) ||
      (size_t)s.offset + s.length > PAGE_SIZE) {
    return -1;
  }

  memcpy(out, (const uint8_t *)page + s.offset, sizeof(TableMetadata));
  return 0;
}

// creates <root>/<db>/catalog.dat (header only) if it does not exist yet
int catalogue_create(const char *db_name) {
  if (db_name == NULL) {
    return -1;
  }

  set_catalogue_target(db_name);
  return create_page_file();
}

// finds one table by name by scanning every page of catalog.dat
int catalogue_get(const char *db_name, const char *table_name,
                  TableMetadata *out) {
  if (db_name == NULL || table_name == NULL || out == NULL) {
    return -1;
  }

  set_catalogue_target(db_name);

  PageId latest = fetch_latest();
  if (latest == (PageId)-1) {
    return -1;
  }

  for (PageId id = 0; id <= latest; id++) {
    Page page = read_page(id);
    if (page.header.page_id != id) {
      return -1;
    }

    for (uint16_t i = 0; i < page.header.num_slots; i++) {
      TableMetadata meta;
      if (copy_meta(&page, i, &meta) != 0) {
        return -1;
      }
      if (strcmp(meta.name, table_name) == 0) {
        *out = meta;
        return 0;
      }
    }
  }

  return -1;
}

// appends one table's metadata; -1 when the name is already catalogued
int catalogue_put(const char *db_name, const TableMetadata *meta) {
  if (db_name == NULL || meta == NULL) {
    return -1;
  }

  TableMetadata existing;
  if (catalogue_get(db_name, meta->name, &existing) == 0) {
    return -1; // already catalogued
  }

  set_catalogue_target(db_name);
  return insert_record(meta, (uint16_t)sizeof(TableMetadata));
}

// reads every TableMetadata record of catalog.dat into a fresh Catalogue
Catalogue *catalogue_load(const char *db_name) {
  if (db_name == NULL) {
    return NULL;
  }

  Catalogue *cat = calloc(1, sizeof(Catalogue));
  if (cat == NULL) {
    return NULL;
  }

  set_catalogue_target(db_name);

  PageId latest = fetch_latest();
  if (latest == (PageId)-1) {
    return cat; // no records yet
  }

  for (PageId id = 0; id <= latest && cat->table_count < MAX_TABLES; id++) {
    Page page = read_page(id);
    if (page.header.page_id != id) {
      catalogue_free(cat);
      return NULL;
    }

    for (uint16_t i = 0;
         i < page.header.num_slots && cat->table_count < MAX_TABLES; i++) {
      if (copy_meta(&page, i, &cat->tables[cat->table_count]) != 0) {
        catalogue_free(cat);
        return NULL;
      }
      cat->table_count++;
    }
  }

  return cat;
}

// releases a Catalogue returned by catalogue_load
void catalogue_free(Catalogue *catalogue) { free(catalogue); }
