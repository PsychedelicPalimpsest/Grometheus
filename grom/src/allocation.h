#include <stdbool.h>
#include <stdint.h>

enum { BLOCK_BYTES = 4096 };

typedef struct {
    uint64_t start;
    uint64_t length;
} Extent;

typedef enum {
    OK,
    INVALID,
    NO_SPACE,
    PLAN_LIMIT,
    NO_MEMORY,
    NO_ENTROPY
} AStatus;

