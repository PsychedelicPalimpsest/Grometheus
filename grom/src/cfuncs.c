#include <unistd.h>

int strncmp( const char* lhs, const char* rhs, size_t count ) {
  while (count--) {
    if (*lhs > *rhs) return -1;
    if (*lhs < *rhs) return 1;
    lhs++; rhs++;
  }
  return 0;
}



