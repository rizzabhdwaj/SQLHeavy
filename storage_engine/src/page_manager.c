// page_manager.c — the only layer that touches database files directly.
// Owns the file header, page allocation and the slotted-page record layout.
// Everything above this file works in Page / PageId / Slot terms.

#include "page_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char g_db_name[64];
static char g_path[256];

// root of the on-disk database tree: $MYDB_DATA_DIR when set, else ../data
static const char *data_root(void) {
  const char *env = getenv("MYDB_DATA_DIR");
  return (env != NULL && env[0] != '\0') ? env : "../data";
}

// <root>/<db_name>/tables/<table_name>.dat
void set_target(const char *db_name, const char *table_name) {
  snprintf(g_db_name, sizeof(g_db_name), "%s", db_name ? db_name : "");
  snprintf(g_path, sizeof(g_path), "%s/%s/tables/%s.dat", data_root(),
           g_db_name, table_name ? table_name : "");
}

// <root>/<db_name>/catalog.dat
void set_catalogue_target(const char *db_name) {
  snprintf(g_db_name, sizeof(g_db_name), "%s", db_name ? db_name : "");
  snprintf(g_path, sizeof(g_path), "%s/%s/catalog.dat", data_root(), g_db_name);
}

// true when the current target file has already been created
int target_exists(void) { return access(g_path, F_OK) == 0; }

// <root>, <root>/<db_name>, <root>/<db_name>/tables
static int make_dirs(void) {
  char dirName[256];
  const char *root = data_root();

  if (mkdir(root, 0755) != 0 && errno != EEXIST) {
    perror("mkdir data root");
    return -1;
  }

  snprintf(dirName, sizeof(dirName), "%s/%s", root, g_db_name);
  if (mkdir(dirName, 0755) != 0 && errno != EEXIST) {
    perror("mkdir");
    return -1;
  }

  snprintf(dirName, sizeof(dirName), "%s/%s/tables", root, g_db_name);
  if (mkdir(dirName, 0755) != 0 && errno != EEXIST) {
    perror("mkdir");
    return -1;
  }

  return 0;
}

// creates the target file containing only the file header (no pages yet)
int create_page_file(void) {
  if (make_dirs() != 0) {
    return -1;
  }

  if (target_exists()) {
    return 0;
  }

  FILE *file = fopen(g_path, "w+b");
  if (file == NULL) {
    perror("fopen");
    return -1;
  }

  FileHeader file_head = {.page_count = 0, .page_size = PAGE_SIZE};
  int ok = fwrite(&file_head, sizeof(FileHeader), 1, file) == 1;
  fclose(file);

  if (!ok) {
    perror("fwrite");
    return -1;
  }
  return 0;
}

// appends a fresh zeroed page at the end of the file and returns its id
PageId allocate_page(void) {
  if (make_dirs() != 0) {
    return (PageId)-1;
  }

  FILE *file = fopen(g_path, "r+b");

  if (file == NULL) {
    file = fopen(g_path, "w+b");
  }

  if (file == NULL) {
    perror("fopen");
    return (PageId)-1;
  }

  Page page = {};
  FileHeader file_head = {};

  if (fread(&file_head, sizeof(FileHeader), 1, file) != 1 ||
      file_head.page_size != PAGE_SIZE) {
    // no file header yet (or stale layout): start from an empty file
    file_head.page_count = 0;
    file_head.page_size = PAGE_SIZE;
  }

  // the new page always goes at the end: id = current page_count
  PageId page_id = file_head.page_count;
  file_head.page_count = page_id + 1;

  page.header.page_id = page_id;
  page.header.num_slots = 0;
  page.header.free_space_start = sizeof(PageHeader);
  page.header.free_space_end = PAGE_SIZE;

  if (fseek(file, (long)page_id * (long)PAGE_SIZE + (long)sizeof(FileHeader),
            SEEK_SET) != 0) {
    perror("fseek");
    fclose(file);
    return (PageId)-1;
  }
  if (fwrite(&page, sizeof(Page), 1, file) != 1) {
    perror("fwrite");
    fclose(file);
    return (PageId)-1;
  }

  rewind(file);
  if (fwrite(&file_head, sizeof(FileHeader), 1, file) != 1) {
    perror("fwrite");
    fclose(file);
    return (PageId)-1;
  }

  fclose(file);
  return page_id;
}

// reads the page with the given id; a zeroed Page is returned if it is missing
Page read_page(PageId page_id) {
  FILE *file = fopen(g_path, "rb");

  Page page = {};
  if (file == NULL) {
    if (errno != ENOENT) {
      perror("open page file");
    }
    return page;
  }

  if (fseek(file, (long)page_id * (long)PAGE_SIZE + sizeof(FileHeader),
            SEEK_SET) != 0) {
    perror("fseek");
    fclose(file);
    return page;
  }

  if (fread(&page, sizeof(Page), 1, file) != 1) {
    perror("fread");
    fclose(file);
    return page;
  }

  fclose(file);
  return page;
}

// id of the newest page in the file, (PageId)-1 when the file has no pages
PageId fetch_latest(void) {
  FILE *file = fopen(g_path, "rb");
  if (file == NULL) {
    if (errno != ENOENT) {
      perror("open page file");
    }
    return (PageId)-1;
  }

  FileHeader header = {};
  if (fread(&header, sizeof(FileHeader), 1, file) != 1) {
    perror("fread");
    fclose(file);
    return (PageId)-1;
  }
  fclose(file);

  if (header.page_count == 0) {
    return (PageId)-1;
  }
  return header.page_count - 1;
}

// appends one record to the latest page of the current target, allocating a
// new page when there is not enough room for the record plus its slot entry
int insert_record(const void *data, uint16_t length) {
  if (data == NULL && length != 0) {
    return -1;
  }

  // A slot entry must fit alongside every record, even on an empty page.
  if ((size_t)length + sizeof(PageHeader) + sizeof(Slot) > PAGE_SIZE) {
    return -1;
  }

  PageId page_id = fetch_latest();
  Page page = {};

  if (page_id != (PageId)-1) {
    page = read_page(page_id);
  }

  // no page yet, unreadable page, or page full -> append a fresh page
  // (each insert needs room for the record AND its slot entry)
  if (page_id == (PageId)-1 || page.header.page_id != page_id ||
      (size_t)page.header.free_space_start + length + sizeof(Slot) >
          (size_t)page.header.free_space_end) {
    page_id = allocate_page();
    if (page_id == (PageId)-1) {
      fprintf(stderr, "failed to allocate page\n");
      return -1;
    }
    page = read_page(page_id);
  }

  if (page.header.page_id != page_id) {
    fprintf(stderr, "failed to read page %u\n", (unsigned)page_id);
    return -1;
  }

  // records grow up from the header, slots grow down from the page tail
  uint16_t rec_offset = page.header.free_space_start;
  memcpy((uint8_t *)&page + rec_offset, data, length);

  Slot *slot = (Slot *)((uint8_t *)&page + PAGE_SIZE -
                        (page.header.num_slots + 1) * sizeof(Slot));
  slot->offset = rec_offset;
  slot->length = length;

  page.header.free_space_start += length;
  page.header.num_slots += 1;
  page.header.free_space_end -= sizeof(Slot);

  FILE *file = fopen(g_path, "r+b");
  if (file == NULL) {
    perror("open page file");
    return -1;
  }

  if (fseek(file, (long)page_id * (long)PAGE_SIZE + (long)sizeof(FileHeader),
            SEEK_SET) != 0 ||
      fwrite(&page, sizeof(Page), 1, file) != 1) {
    perror("write page");
    fclose(file);
    return -1;
  }

  fclose(file);
  return 0;
}
