// page_manager.h — low-level page I/O and the slotted-page record layout.
// This is the only layer that opens database files directly; everything above
// works in Page / PageId / Slot terms.

#ifndef PAGE_MANAGER_H
#define PAGE_MANAGER_H

#include <stdint.h>

typedef uint32_t PageId;

// file layout
//   data/<db_name>/catalog.dat            -> catalogue records
//   data/<db_name>/tables/<table_name>.dat -> table records
// must be set (by storage.c / catalog.c) before any page operation
void set_target(const char *db_name, const char *table_name);
void set_catalogue_target(const char *db_name);

// creates the directories of the current target and an empty page file
// (returns 0 when the file already exists or was created)
int create_page_file(void);

typedef struct {
  PageId page_id;
  uint16_t num_slots;
  uint16_t free_space_start;
  uint16_t free_space_end;
} PageHeader;

typedef struct {
  uint32_t page_count;
  uint32_t page_size;
} FileHeader;

// total page size = data area + page header
#define PAGE_SIZE (2400 + sizeof(PageHeader))

typedef struct {
  PageHeader header;
  uint8_t data[PAGE_SIZE - sizeof(PageHeader)];
} Page;
typedef struct {
  uint16_t offset;
  uint16_t length;
} Slot;

// slot i lives at the tail of the page; it knows where record i is
static inline Slot get_slot(const Page *page, uint16_t i) {
  const Slot *s = (const Slot *)((const uint8_t *)page + PAGE_SIZE -
                                 (i + 1) * sizeof(Slot));
  return *s;
}

PageId allocate_page(void);
Page read_page(PageId page_id);
PageId fetch_latest(void);
// appends one record to the current target; returns 0 on success
int insert_record(const void *data, uint16_t length);

// 1 when data/<db>/<table>.dat exists for the current target
int target_exists(void);

#endif
