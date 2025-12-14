/* loopyMaxHeap - High-performance max-heap implementation
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 * Licensed under Apache 2.0
 *
 * Implementation notes:
 * - Array-based binary heap for cache locality
 * - Position map for O(log n) arbitrary element removal
 * - Grows by doubling to amortize allocation cost
 * - All indices are 1-based internally for simpler parent/child math
 */

#include "loopyPlatform.h"

#include "../deps/datakit/src/datakit.h"
#include "loopyMaxHeap.h"

#include <string.h>

/* ====================================================================
 * Internal Helpers - Inlined for Performance
 * ==================================================================== */

/* Parent/child index calculations (1-based indexing) */
#define PARENT(i) ((i) >> 1)
#define LEFT(i) ((i) << 1)
#define RIGHT(i) (((i) << 1) | 1)

/* Swap two elements and update position map */
static inline void heapSwap(loopyMaxHeap *heap, size_t i, size_t j) {
    loopyHeapValue vi = heap->data[i];
    loopyHeapValue vj = heap->data[j];

    /* Swap in heap array */
    heap->data[i] = vj;
    heap->data[j] = vi;

    /* Update position map - find entries and update their indices */
    /* For fd tracking where values are small, we use value as direct index */
    if (vi >= 0 && (size_t)vi < heap->posMapSize) {
        heap->posMap[vi].heapIndex = j;
    }
    if (vj >= 0 && (size_t)vj < heap->posMapSize) {
        heap->posMap[vj].heapIndex = i;
    }
}

/* Bubble up element at index i to restore heap property */
static inline void heapBubbleUp(loopyMaxHeap *heap, size_t i) {
    while (i > 1) {
        size_t p = PARENT(i);
        if (heap->data[p] >= heap->data[i]) {
            break;
        }
        heapSwap(heap, i, p);
        i = p;
    }
}

/* Bubble down element at index i to restore heap property */
static inline void heapBubbleDown(loopyMaxHeap *heap, size_t i) {
    while (true) {
        size_t largest = i;
        size_t left = LEFT(i);
        size_t right = RIGHT(i);

        if (left <= heap->size && heap->data[left] > heap->data[largest]) {
            largest = left;
        }
        if (right <= heap->size && heap->data[right] > heap->data[largest]) {
            largest = right;
        }

        if (largest == i) {
            break;
        }

        heapSwap(heap, i, largest);
        i = largest;
    }
}

/* Ensure position map can hold value as index */
static bool ensurePosMapCapacity(loopyMaxHeap *heap, loopyHeapValue value) {
    if (value < 0) {
        return true; /* Negative values not tracked in position map */
    }

    size_t needed = (size_t)value + 1;
    if (needed <= heap->posMapSize) {
        return true;
    }

    /* Grow position map - use at least 2x or needed, whichever is larger */
    size_t newSize = heap->posMapSize ? heap->posMapSize * 2 : 16;
    while (newSize < needed) {
        newSize *= 2;
    }

    loopyHeapPosEntry *newMap =
        zrealloc(heap->posMap, newSize * sizeof(*newMap));
    if (!newMap) {
        return false;
    }

    /* Zero out new entries */
    memset(newMap + heap->posMapSize, 0,
           (newSize - heap->posMapSize) * sizeof(*newMap));

    heap->posMap = newMap;
    heap->posMapSize = newSize;
    return true;
}

/* Grow heap data array */
static bool heapGrow(loopyMaxHeap *heap) {
    size_t newCapacity = heap->capacity ? heap->capacity * 2 : 16;

    /* data array is 1-indexed, so allocate capacity + 1 */
    loopyHeapValue *newData =
        zrealloc(heap->data, (newCapacity + 1) * sizeof(*newData));
    if (!newData) {
        return false;
    }

    heap->data = newData;
    heap->capacity = newCapacity;
    return true;
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopyMaxHeap *loopyMaxHeapNew(size_t initialCapacity) {
    loopyMaxHeap *heap = zcalloc(1, sizeof(*heap));
    if (!heap) {
        return NULL;
    }

    heap->allocated = true;

    if (!loopyMaxHeapInit(heap, initialCapacity)) {
        zfree(heap);
        return NULL;
    }

    return heap;
}

bool loopyMaxHeapInit(loopyMaxHeap *heap, size_t initialCapacity) {
    if (!heap) {
        return false;
    }

    bool wasAllocated = heap->allocated;
    memset(heap, 0, sizeof(*heap));
    heap->allocated = wasAllocated;

    if (initialCapacity == 0) {
        initialCapacity = 16;
    }

    /* Allocate data array (1-indexed, so +1) */
    heap->data = zcalloc(initialCapacity + 1, sizeof(*heap->data));
    if (!heap->data) {
        return false;
    }

    /* Allocate initial position map */
    heap->posMap = zcalloc(initialCapacity, sizeof(*heap->posMap));
    if (!heap->posMap) {
        zfree(heap->data);
        heap->data = NULL;
        return false;
    }

    heap->capacity = initialCapacity;
    heap->posMapSize = initialCapacity;
    heap->size = 0;

    return true;
}

bool loopyMaxHeapInited(const loopyMaxHeap *heap) {
    return heap && heap->data != NULL;
}

void loopyMaxHeapDeinit(loopyMaxHeap *heap) {
    if (heap) {
        if (heap->data) {
            zfree(heap->data);
            heap->data = NULL;
        }
        if (heap->posMap) {
            zfree(heap->posMap);
            heap->posMap = NULL;
        }
        bool wasAllocated = heap->allocated;
        memset(heap, 0, sizeof(*heap));
        heap->allocated = wasAllocated;
    }
}

void loopyMaxHeapFree(loopyMaxHeap *heap) {
    if (heap) {
        loopyMaxHeapDeinit(heap);
        if (heap->allocated) {
            zfree(heap);
        }
    }
}

/* ====================================================================
 * Core Operations
 * ==================================================================== */

bool loopyMaxHeapInsert(loopyMaxHeap *heap, loopyHeapValue value) {
    if (!heap) {
        return false;
    }

    /* Ensure capacity */
    if (heap->size >= heap->capacity) {
        if (!heapGrow(heap)) {
            return false;
        }
    }

    /* Ensure position map can track this value */
    if (!ensurePosMapCapacity(heap, value)) {
        return false;
    }

    /* Insert at end (1-indexed) */
    heap->size++;
    size_t i = heap->size;
    heap->data[i] = value;

    /* Update position map */
    if (value >= 0 && (size_t)value < heap->posMapSize) {
        heap->posMap[value].value = value;
        heap->posMap[value].heapIndex = i;
    }

    /* Restore heap property */
    heapBubbleUp(heap, i);

    return true;
}

loopyHeapValue loopyMaxHeapPeek(const loopyMaxHeap *heap) {
    if (!heap || heap->size == 0) {
        return LOOPY_HEAP_EMPTY_VALUE;
    }
    return heap->data[1];
}

loopyHeapValue loopyMaxHeapPop(loopyMaxHeap *heap) {
    if (!heap || heap->size == 0) {
        return LOOPY_HEAP_EMPTY_VALUE;
    }

    loopyHeapValue max = heap->data[1];

    /* Clear position map entry for removed value */
    if (max >= 0 && (size_t)max < heap->posMapSize) {
        heap->posMap[max].heapIndex = 0;
    }

    /* Move last element to root */
    if (heap->size > 1) {
        loopyHeapValue last = heap->data[heap->size];
        heap->data[1] = last;

        /* Update position map for moved element */
        if (last >= 0 && (size_t)last < heap->posMapSize) {
            heap->posMap[last].heapIndex = 1;
        }
    }

    heap->size--;

    /* Restore heap property */
    if (heap->size > 0) {
        heapBubbleDown(heap, 1);
    }

    return max;
}

bool loopyMaxHeapRemove(loopyMaxHeap *heap, loopyHeapValue value) {
    if (!heap || heap->size == 0) {
        return false;
    }

    /* Find index of value using position map for O(1) lookup */
    size_t i = 0;
    if (value >= 0 && (size_t)value < heap->posMapSize) {
        i = heap->posMap[value].heapIndex;
    }

    /* If not in position map or invalid, fall back to linear search */
    if (i == 0 || i > heap->size || heap->data[i] != value) {
        /* Linear search fallback */
        for (size_t j = 1; j <= heap->size; j++) {
            if (heap->data[j] == value) {
                i = j;
                break;
            }
        }
        if (i == 0) {
            return false; /* Not found */
        }
    }

    /* Clear position map entry */
    if (value >= 0 && (size_t)value < heap->posMapSize) {
        heap->posMap[value].heapIndex = 0;
    }

    /* If removing last element, just decrement size */
    if (i == heap->size) {
        heap->size--;
        return true;
    }

    /* Replace with last element */
    loopyHeapValue last = heap->data[heap->size];
    heap->data[i] = last;
    heap->size--;

    /* Update position map for moved element */
    if (last >= 0 && (size_t)last < heap->posMapSize) {
        heap->posMap[last].heapIndex = i;
    }

    /* Restore heap property - may need to bubble up or down */
    if (i > 1 && heap->data[i] > heap->data[PARENT(i)]) {
        heapBubbleUp(heap, i);
    } else {
        heapBubbleDown(heap, i);
    }

    return true;
}

bool loopyMaxHeapContains(const loopyMaxHeap *heap, loopyHeapValue value) {
    if (!heap || heap->size == 0) {
        return false;
    }

    /* Check position map first for O(1) lookup */
    if (value >= 0 && (size_t)value < heap->posMapSize) {
        size_t idx = heap->posMap[value].heapIndex;
        if (idx > 0 && idx <= heap->size && heap->data[idx] == value) {
            return true;
        }
    }

    /* Fallback to linear search for values not in position map */
    for (size_t i = 1; i <= heap->size; i++) {
        if (heap->data[i] == value) {
            return true;
        }
    }

    return false;
}

/* ====================================================================
 * Utility
 * ==================================================================== */

bool loopyMaxHeapIsEmpty(const loopyMaxHeap *heap) {
    return !heap || heap->size == 0;
}

size_t loopyMaxHeapSize(const loopyMaxHeap *heap) {
    return heap ? heap->size : 0;
}

size_t loopyMaxHeapCapacity(const loopyMaxHeap *heap) {
    return heap ? heap->capacity : 0;
}

bool loopyMaxHeapReserve(loopyMaxHeap *heap, size_t capacity) {
    if (!heap) {
        return false;
    }

    if (capacity <= heap->capacity) {
        return true;
    }

    loopyHeapValue *newData =
        zrealloc(heap->data, (capacity + 1) * sizeof(*newData));
    if (!newData) {
        return false;
    }

    heap->data = newData;
    heap->capacity = capacity;
    return true;
}

void loopyMaxHeapClear(loopyMaxHeap *heap) {
    if (heap) {
        /* Clear position map entries */
        if (heap->posMap) {
            memset(heap->posMap, 0, heap->posMapSize * sizeof(*heap->posMap));
        }
        heap->size = 0;
    }
}
