#include <sys/syscall.h>
#include <unistd.h>

int strncmp( const char* lhs, const char* rhs, size_t count ) {
  while (count--) {
    if (*lhs > *rhs) return -1;
    if (*lhs < *rhs) return 1;
    lhs++; rhs++;
  }
  return 0;
}


ssize_t getrandom(void *buf, size_t size, unsigned int flags) {
    return syscall(SYS_getrandom, buf, size, flags);
}
