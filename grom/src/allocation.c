#include "allocation.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>

ssize_t getrandom(void* buf, size_t size, unsigned int flags);

static bool get_random_word(void *context, uint64_t *value)
{
    (void)context;
    size_t bytes_read = 0;
    while (bytes_read < sizeof *value) {
        ssize_t result = getrandom((char *)value + bytes_read,
                                   sizeof *value - bytes_read, 0);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        bytes_read += (size_t)result;
    }
    return true;
}

/* Reject the incomplete residue group so [0, bound) is uniform. */
static bool random_below(void *context,
                         uint64_t bound, uint64_t *result)
{
    uint64_t value;
    uint64_t threshold = (UINT64_C(0) - bound) % bound;
    do {
        if (!get_random_word(context, &value))
            return false;
    } while (value < threshold);

    *result = value % bound;
    return true;
}

/*
 * Coordinates and lengths are pool-relative 4 KiB blocks. The caller
 * supplies a validated pool identity and its sorted, disjoint preservation
 * ranges. Omitted slots may be overwritten.
 *
 * The caller owns the input ranges and a separate plan buffer with room
 * for extent_limit entries. On success the plan lists backing extents in
 * logical block order. The caller is responsible for erasing it after use.
 * On failure plan_count is zero and any entries written are erased.
 */
AStatus plan_allocation(uint64_t pool_blocks, uint64_t requested_blocks,
                              uint64_t max_extent_blocks,
                              const Extent *protected_ranges, size_t protected_count,
                              Extent *plan, size_t extent_limit, size_t *plan_count,
                              void *random_context)
{
    if (!plan_count)
        return INVALID;
    *plan_count = 0;
    if (!pool_blocks || !requested_blocks || !max_extent_blocks || 
        (protected_count && !protected_ranges) || (extent_limit && !plan))
        return INVALID;

    uint64_t previous_end = 0;
    uint64_t eligible_blocks = pool_blocks;
    for (size_t i = 0; i < protected_count; ++i) {
        Extent range = protected_ranges[i];
        if (!range.length || range.start < previous_end || range.start > pool_blocks ||
            range.length > pool_blocks - range.start)
            return INVALID;
        previous_end = range.start + range.length;
        eligible_blocks -= range.length;
    }
    if (requested_blocks > eligible_blocks)
        return NO_SPACE;

    uint64_t minimum_extents = 1 + (requested_blocks - 1) / max_extent_blocks;
    if (minimum_extents > extent_limit)
        return PLAN_LIMIT;

    /* The complement starts with at most protected_count + 1 gaps.
     * Placing an extent can split one gap into two. */
    size_t max_gap_entries = SIZE_MAX / sizeof(Extent);
    if (extent_limit >= max_gap_entries || protected_count >= max_gap_entries - extent_limit)
        return NO_MEMORY;
    size_t gap_capacity = protected_count + 1 + extent_limit;
    Extent *gaps = calloc(gap_capacity, sizeof *gaps);
    if (!gaps)
        return NO_MEMORY;

    size_t gap_count = 0;
    previous_end = 0;
    for (size_t i = 0; i < protected_count; ++i) {
        Extent range = protected_ranges[i];
        if (previous_end < range.start)
            gaps[gap_count++] = (Extent){previous_end, range.start - previous_end};
        previous_end = range.start + range.length;
    }
    if (previous_end < pool_blocks)
        gaps[gap_count++] = (Extent){previous_end, pool_blocks - previous_end};

    AStatus status = OK;
    size_t extents_written = 0;
    uint64_t blocks_remaining = requested_blocks;
    while (blocks_remaining > 0) {
        if (extents_written == extent_limit) {
            status = PLAN_LIMIT;
            break;
        }

        uint64_t length_limit = max_extent_blocks < blocks_remaining
                              ? max_extent_blocks : blocks_remaining;
        uint64_t anchor_offset;
        uint64_t grow_left;
        uint64_t length_draw;
        if (!random_below(random_context, eligible_blocks, &anchor_offset) ||
            !random_below(random_context, 2, &grow_left) ||
            !random_below(random_context, length_limit, &length_draw)) {
            status = NO_ENTROPY;
            break;
        }

        /* Find the sampled block among the gaps. Each gap gets one
         * possible anchor per block, regardless of its position. */
        size_t gap_index = 0;
        while (anchor_offset >= gaps[gap_index].length) {
            anchor_offset -= gaps[gap_index].length;
            ++gap_index;
        }
        Extent gap = gaps[gap_index];

        uint64_t room = grow_left ? anchor_offset + 1 : gap.length - anchor_offset;
        uint64_t length = length_draw + 1;
        if (length > room)
            length = room;
        uint64_t leading_blocks = grow_left ? anchor_offset - (length - 1) : anchor_offset;
        uint64_t trailing_blocks = gap.length - leading_blocks - length;
        Extent placement = {gap.start + leading_blocks, length};
        plan[extents_written++] = placement;

        /* Remove the gap by swapping in the last entry. Keep either
         * remainder as a new gap. The gap list can be unordered. */
        --gap_count;
        gaps[gap_index] = gaps[gap_count];
        if (leading_blocks)
            gaps[gap_count++] = (Extent){gap.start, leading_blocks};
        if (trailing_blocks)
            gaps[gap_count++] = (Extent){placement.start + placement.length, trailing_blocks};

        eligible_blocks -= length;
        blocks_remaining -= length;
    }

    explicit_bzero(gaps, gap_capacity * sizeof *gaps);
    free(gaps);
    if (status == OK)
        *plan_count = extents_written;
    else if (extents_written)
        explicit_bzero(plan, extents_written * sizeof *plan);
    return status;
}

