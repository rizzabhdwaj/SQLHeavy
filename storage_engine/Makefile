
CC = gcc
CFLAGS = -Wall -Wextra -g -fPIC -Iinclude

SRC = $(wildcard src/*.c)
HDR = $(wildcard include/*.h)
LIB = libdatabase.so

TEST_BINS = test_storage test_insert_stress

.PHONY: all lib test clean

all: lib

lib: $(LIB)

$(LIB): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -shared -o $@ $(SRC)

test_storage: tests/test_storage.c $(HDR) $(LIB)
	$(CC) -Wall -Wextra -g -Iinclude \
		-o $@ $< -L. -ldatabase \
		-Wl,-rpath,'$$ORIGIN'

test_insert_stress: tests/test_insert_stress.c $(HDR) $(LIB)
	$(CC) -Wall -Wextra -g -Iinclude \
		-o $@ $< -L. -ldatabase \
		-Wl,-rpath,'$$ORIGIN'

test: $(TEST_BINS)
	@for test in $(TEST_BINS); do ./$$test || exit $$?; done

clean:
	rm -f $(LIB) $(TEST_BINS) a.out *.o src/*.o
