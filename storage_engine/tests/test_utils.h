#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <stdio.h>
#include <stdlib.h>

#define TEST_GREEN "\033[32m"
#define TEST_RED "\033[31m"
#define TEST_RESET "\033[0m"

#define TEST_ASSERT(condition)                                               \
  do {                                                                        \
    if (!(condition)) {                                                       \
      fprintf(stderr, TEST_RED "not ok" TEST_RESET                          \
                      " - %s:%d: %s\n",                                     \
              __FILE__, __LINE__, #condition);                                \
      exit(EXIT_FAILURE);                                                     \
    }                                                                         \
  } while (0)

#define TEST_OK(name) printf(TEST_GREEN "ok" TEST_RESET " - %s\n", name)

#endif
