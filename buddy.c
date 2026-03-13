#include "buddy.h"
#define NULL ((void *)0)

#define MAXRANK 16
#define PAGE_SIZE (4 * 1024)  // 4KB

typedef struct FreeNode {
    struct FreeNode *next;
    struct FreeNode *prev;
} FreeNode;

// Global state
static void *base_addr = NULL;
static int total_pages = 0;
static FreeNode *free_lists[MAXRANK + 1]; // Free lists for each rank (1-16)
static unsigned char page_ranks[128 * 1024 / 4]; // Max pages we'll support

// Helper functions
static inline int get_page_index(void *p) {
    if (p < base_addr || p >= base_addr + total_pages * PAGE_SIZE) {
        return -1;
    }
    long offset = (char *)p - (char *)base_addr;
    if (offset % PAGE_SIZE != 0) {
        return -1;
    }
    return offset / PAGE_SIZE;
}

static inline void *get_page_addr(int page_idx) {
    return (char *)base_addr + page_idx * PAGE_SIZE;
}

static inline int get_buddy_index(int page_idx, int rank) {
    int block_size = 1 << (rank - 1);
    return page_idx ^ block_size;
}

static inline int is_valid_rank(int rank) {
    return rank >= 1 && rank <= MAXRANK;
}

// Add node to free list (doubly-linked for O(1) removal)
static inline void add_to_free_list(int rank, FreeNode *node) {
    node->next = free_lists[rank];
    node->prev = NULL;
    if (free_lists[rank] != NULL) {
        free_lists[rank]->prev = node;
    }
    free_lists[rank] = node;
}

// Remove node from free list (O(1) with doubly-linked list)
static inline void remove_from_free_list(int rank, FreeNode *node) {
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        free_lists[rank] = node->next;
    }
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
}

// Initialize memory pages
int init_page(void *p, int pgcount) {
    base_addr = p;
    total_pages = pgcount;

    // Initialize free lists
    for (int i = 0; i <= MAXRANK; i++) {
        free_lists[i] = NULL;
    }

    // Initialize page ranks metadata
    for (int i = 0; i < pgcount; i++) {
        page_ranks[i] = 0;
    }

    // Build initial free blocks
    int page_idx = 0;
    while (page_idx < total_pages) {
        int remaining = total_pages - page_idx;
        int rank = MAXRANK;

        while (rank > 0) {
            int block_size = 1 << (rank - 1);
            if (block_size <= remaining && (page_idx % block_size == 0)) {
                FreeNode *node = (FreeNode *)get_page_addr(page_idx);
                add_to_free_list(rank, node);
                page_ranks[page_idx] = rank;
                page_idx += block_size;
                break;
            }
            rank--;
        }

        if (rank == 0) {
            return -EINVAL;
        }
    }

    return OK;
}

// Allocate pages with specified rank
void *alloc_pages(int rank) {
    if (!is_valid_rank(rank)) {
        return ERR_PTR(-EINVAL);
    }

    // Find a free block of the requested rank or larger
    int current_rank = rank;
    while (current_rank <= MAXRANK && free_lists[current_rank] == NULL) {
        current_rank++;
    }

    if (current_rank > MAXRANK) {
        return ERR_PTR(-ENOSPC);
    }

    // Split blocks until we get the desired rank
    while (current_rank > rank) {
        FreeNode *block = free_lists[current_rank];
        remove_from_free_list(current_rank, block);

        int page_idx = get_page_index((void *)block);
        current_rank--;
        int block_size = 1 << (current_rank - 1);

        // Split into two halves
        FreeNode *first_half = block;
        FreeNode *second_half = (FreeNode *)get_page_addr(page_idx + block_size);

        add_to_free_list(current_rank, second_half);
        page_ranks[page_idx + block_size] = current_rank;

        add_to_free_list(current_rank, first_half);
        page_ranks[page_idx] = current_rank;
    }

    // Remove and return the block
    FreeNode *result = free_lists[rank];
    remove_from_free_list(rank, result);

    int page_idx = get_page_index((void *)result);
    page_ranks[page_idx] = rank | 0x80;  // Mark as allocated

    return (void *)result;
}

// Return pages to the buddy system
int return_pages(void *p) {
    if (p == NULL) {
        return -EINVAL;
    }

    int page_idx = get_page_index(p);
    if (page_idx < 0) {
        return -EINVAL;
    }

    int rank = page_ranks[page_idx] & 0x7F;
    if (rank == 0 || !(page_ranks[page_idx] & 0x80)) {
        return -EINVAL;
    }

    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }

    page_ranks[page_idx] = rank;

    // Try to merge with buddy
    while (rank < MAXRANK) {
        int block_size = 1 << (rank - 1);
        int buddy_idx = get_buddy_index(page_idx, rank);

        if (buddy_idx < 0 || buddy_idx >= total_pages) {
            break;
        }

        if (page_ranks[buddy_idx] != rank) {
            break;
        }

        // Check alignment
        int merged_idx = page_idx < buddy_idx ? page_idx : buddy_idx;
        if (merged_idx % (block_size * 2) != 0) {
            break;
        }

        // Remove buddy from free list (O(1) with doubly-linked list)
        FreeNode *buddy_node = (FreeNode *)get_page_addr(buddy_idx);
        remove_from_free_list(rank, buddy_node);

        // Merge
        page_idx = merged_idx;
        rank++;
        page_ranks[page_idx] = rank;
        if (buddy_idx != merged_idx) {
            page_ranks[buddy_idx] = 0;
        }
    }

    // Add merged block to free list
    FreeNode *node = (FreeNode *)get_page_addr(page_idx);
    add_to_free_list(rank, node);

    return OK;
}

// Query the rank of a page
int query_ranks(void *p) {
    int page_idx = get_page_index(p);
    if (page_idx < 0) {
        return -EINVAL;
    }

    int rank = page_ranks[page_idx] & 0x7F;
    if (rank == 0) {
        // This page might be part of a larger block
        for (int r = MAXRANK; r >= 1; r--) {
            int block_size = 1 << (r - 1);
            int block_start = (page_idx / block_size) * block_size;
            if (block_start != page_idx) {
                int check_rank = page_ranks[block_start] & 0x7F;
                if (check_rank == r) {
                    return r;
                }
            }
        }
        return -EINVAL;
    }

    return rank;
}

// Query count of free pages for a given rank
int query_page_counts(int rank) {
    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }

    int count = 0;
    FreeNode *current = free_lists[rank];
    while (current != NULL) {
        count++;
        current = current->next;
    }

    return count;
}
