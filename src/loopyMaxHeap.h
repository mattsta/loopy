/* loopyMaxHeap - High-performance max-heap with pooled memory allocation
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 * Licensed under Apache 2.0
 *
 * Features:
 * - O(1) find-max, O(log n) insert/delete
 * - Pooled memory allocation (no per-operation malloc)
 * - Cache-friendly array-based storage
 * - Self-managing capacity growth
 * - Suitable for tracking file descriptors, priorities, etc.
 */

#pragma once

#include "loopyPlatform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Heap element type - using int32_t for fd tracking, but generalizable */
typedef int32_t loopyHeapValue;

/* Position map entry for fast lookups */
typedef struct loopyHeapPosEntry {
    loopyHeapValue value;
    size_t heapIndex; /* 1-based index into heap array, 0 = unused */
} loopyHeapPosEntry;

/* Max-heap structure - exposed for embedding in other structures */
typedef struct loopyMaxHeap {
    loopyHeapValue *data;      /* Heap array (1-indexed, data[0] unused) */
    loopyHeapPosEntry *posMap; /* Position map for O(1) index lookup */
    size_t size;               /* Current number of elements */
    size_t capacity;           /* Current capacity (data array size - 1) */
    size_t posMapSize;         /* Position map array size */
    bool allocated;            /* True if struct was heap-allocated */
} loopyMaxHeap;

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * @brief Creates a new dynamically allocated max-heap structure.
 *
 * Allocates memory for a new loopyMaxHeap structure on the heap, along with
 * the internal data array and position map. The heap will automatically grow
 * as needed when elements are inserted. Use loopyMaxHeapFree() to deallocate.
 *
 * @param initialCapacity Initial capacity for the heap data array. If 0,
 *                        defaults to 16 elements. The heap will double in
 *                        size when capacity is exceeded.
 *
 * @return A pointer to the newly allocated loopyMaxHeap, or NULL if memory
 *         allocation fails for either the heap structure or internal arrays.
 *
 * @note The returned heap is marked as heap-allocated internally, so
 *       loopyMaxHeapFree() will deallocate the structure itself. Do not use
 *       loopyMaxHeapDeinit() on heaps created with loopyMaxHeapNew().
 *
 * @see loopyMaxHeapInit() for creating a heap using caller-provided memory
 * @see loopyMaxHeapFree() for deallocation
 * @see loopyMaxHeapInsert() to add elements
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(100);
 * if (!heap) {
 *     // Handle allocation failure
 *     return;
 * }
 *
 * loopyMaxHeapInsert(heap, 42);
 * loopyMaxHeapInsert(heap, 17);
 *
 * loopyHeapValue max = loopyMaxHeapPeek(heap); // Returns 42
 *
 * loopyMaxHeapFree(heap); // Deallocates heap and all internal data
 * @endcode
 */
loopyMaxHeap *loopyMaxHeapNew(size_t initialCapacity);

/**
 * @brief Initializes a max-heap using caller-provided memory for the structure.
 *
 * Initializes a loopyMaxHeap structure that has been provided by the caller
 * (e.g., embedded in another structure or stack-allocated). This function
 * allocates the internal data array and position map. Use loopyMaxHeapDeinit()
 * to clean up. This allows heap usage without an extra allocation for the
 * structure itself.
 *
 * @param heap Pointer to an uninitialized loopyMaxHeap structure. Must not
 *             be NULL. Can be stack-allocated or embedded in another struct.
 *
 * @param initialCapacity Initial capacity for the heap data array. If 0,
 *                        defaults to 16 elements. The heap will double in
 *                        size as needed.
 *
 * @return true if initialization succeeds, false if memory allocation fails
 *         for the data array or position map.
 *
 * @note The caller is responsible for providing the loopyMaxHeap structure
 *       itself; this function only allocates the internal arrays.
 *
 * @note Use loopyMaxHeapDeinit() to clean up, NOT loopyMaxHeapFree().
 *
 * @see loopyMaxHeapNew() for heap-allocated variant
 * @see loopyMaxHeapDeinit() for cleanup
 * @see loopyMaxHeapInited() to check if heap is initialized
 *
 * @code
 * // Stack-allocated heap
 * loopyMaxHeap heap;
 * if (!loopyMaxHeapInit(&heap, 50)) {
 *     // Handle allocation failure
 *     return;
 * }
 *
 * loopyMaxHeapInsert(&heap, 10);
 * loopyMaxHeapInsert(&heap, 20);
 *
 * loopyMaxHeapDeinit(&heap); // Clean up internal arrays
 * @endcode
 */
bool loopyMaxHeapInit(loopyMaxHeap *heap, size_t initialCapacity);

/**
 * @brief Checks if a heap structure has been properly initialized.
 *
 * Determines whether a loopyMaxHeap has been initialized by checking if the
 * internal data array has been allocated. This is useful to verify a heap
 * is safe to use before calling operations on it.
 *
 * @param heap Pointer to a loopyMaxHeap structure, or NULL.
 *
 * @return true if the heap pointer is non-NULL and the data array has been
 *         allocated (i.e., heap->data != NULL), false otherwise.
 *
 * @note This function is safe to call with NULL pointers.
 *
 * @note A heap initialized with loopyMaxHeapInit() or loopyMaxHeapNew()
 *       will return true. A heap that has been deinitialized with
 *       loopyMaxHeapDeinit() or loopyMaxHeapFree() will return false.
 *
 * @see loopyMaxHeapInit() to initialize a heap
 * @see loopyMaxHeapNew() to create a heap
 *
 * @code
 * loopyMaxHeap heap;
 * if (!loopyMaxHeapInited(&heap)) {
 *     // Heap is not initialized, must call loopyMaxHeapInit()
 *     loopyMaxHeapInit(&heap, 10);
 * }
 * @endcode
 */
bool loopyMaxHeapInited(const loopyMaxHeap *heap);

/**
 * @brief Deallocates all resources for a heap created with loopyMaxHeapNew().
 *
 * Frees the internal data array, position map, and the heap structure itself.
 * Use this function only on heaps allocated with loopyMaxHeapNew(). Calling
 * this on a heap initialized with loopyMaxHeapInit() may cause a double-free.
 *
 * @param heap Pointer to a loopyMaxHeap created with loopyMaxHeapNew(), or
 * NULL. The function safely handles NULL pointers.
 *
 * @return void
 *
 * @note This function is safe to call with NULL pointers.
 *
 * @note Do NOT call this on heaps initialized with loopyMaxHeapInit().
 *       Use loopyMaxHeapDeinit() instead.
 *
 * @note After calling this function, the heap pointer should not be used
 *       again. Consider setting the pointer to NULL.
 *
 * @see loopyMaxHeapNew() for creating heaps that should be freed with this
 * @see loopyMaxHeapDeinit() for cleaning up heaps created with
 * loopyMaxHeapInit()
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(100);
 * if (heap) {
 *     loopyMaxHeapInsert(heap, 42);
 *     loopyMaxHeapFree(heap);
 *     heap = NULL; // Good practice to clear pointer
 * }
 * @endcode
 */
void loopyMaxHeapFree(loopyMaxHeap *heap);

/**
 * @brief Deinitializes a max-heap structure and frees its internal resources.
 *
 * Frees the internal data array and position map for a heap initialized with
 * loopyMaxHeapInit(). The loopyMaxHeap structure itself is not freed; it
 * remains valid (though uninitialized) after this call. Use this function
 * for heaps initialized with loopyMaxHeapInit(); use loopyMaxHeapFree()
 * for heaps created with loopyMaxHeapNew().
 *
 * @param heap Pointer to a loopyMaxHeap initialized with loopyMaxHeapInit(),
 *             or NULL. The function safely handles NULL pointers.
 *
 * @return void
 *
 * @note This function is safe to call with NULL pointers.
 *
 * @note After deinit, the heap structure remains accessible but uninitialized.
 *       loopyMaxHeapInited() will return false after this call.
 *
 * @note Do NOT call this on heaps created with loopyMaxHeapNew().
 *       Use loopyMaxHeapFree() instead.
 *
 * @see loopyMaxHeapInit() for initialization with caller-provided memory
 * @see loopyMaxHeapFree() for heaps created with loopyMaxHeapNew()
 * @see loopyMaxHeapInited() to check if heap is initialized
 *
 * @code
 * loopyMaxHeap heap;
 * loopyMaxHeapInit(&heap, 50);
 *
 * loopyMaxHeapInsert(&heap, 100);
 *
 * loopyMaxHeapDeinit(&heap); // Frees internal arrays
 * // heap structure still exists on stack, but is now uninitialized
 * @endcode
 */
void loopyMaxHeapDeinit(loopyMaxHeap *heap);

/* ====================================================================
 * Core Operations
 * ==================================================================== */

/**
 * @brief Inserts a value into the max-heap.
 *
 * Adds a new element to the heap while maintaining the max-heap property
 * (parent >= all children). The operation is O(log n) where n is the number
 * of elements. If the heap is at capacity, it automatically grows by doubling.
 * The position map is updated to track the element's location for O(1) lookups.
 *
 * @param heap Pointer to an initialized loopyMaxHeap. Must not be NULL and
 *             must be initialized with loopyMaxHeapInit() or loopyMaxHeapNew().
 *
 * @param value The element to insert. Values are int32_t. Negative values are
 *              stored but not tracked in the position map (negative values skip
 *              O(1) position lookup optimization).
 *
 * @return true if insertion succeeds, false if memory allocation fails when
 *         growing the heap or position map.
 *
 * @note Insertion triggers automatic heap growth: capacity doubles when size
 *       reaches capacity. The position map also grows to accommodate value
 *       indices if needed (for non-negative values).
 *
 * @note The heap does not prevent duplicate values. The same value can be
 *       inserted multiple times.
 *
 * @note Time complexity: O(log n) average case, where n is heap size.
 *
 * @see loopyMaxHeapPeek() to get the maximum without removal
 * @see loopyMaxHeapPop() to remove and return the maximum
 * @see loopyMaxHeapRemove() to remove a specific value
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(10);
 * if (!heap) return;
 *
 * // Insert several values
 * loopyMaxHeapInsert(heap, 42);
 * loopyMaxHeapInsert(heap, 17);
 * loopyMaxHeapInsert(heap, 100);
 *
 * loopyHeapValue max = loopyMaxHeapPeek(heap); // Returns 100
 *
 * loopyMaxHeapFree(heap);
 * @endcode
 */
bool loopyMaxHeapInsert(loopyMaxHeap *heap, loopyHeapValue value);

/**
 * @brief Gets the maximum value in the heap without removing it.
 *
 * Returns the root of the max-heap, which is the largest element. This is an
 * O(1) operation that does not modify the heap. If the heap is empty, returns
 * the sentinel value LOOPY_HEAP_EMPTY_VALUE (-1).
 *
 * @param heap Pointer to an initialized loopyMaxHeap, or NULL.
 *
 * @return The maximum value (root element) if heap is non-empty and
 * initialized, or LOOPY_HEAP_EMPTY_VALUE (-1) if heap is NULL, uninitialized,
 * or empty.
 *
 * @note This function does not modify the heap; the maximum element remains.
 *
 * @note Time complexity: O(1)
 *
 * @note To distinguish between a heap containing -1 and an empty heap, use
 *       loopyMaxHeapIsEmpty() to check for emptiness.
 *
 * @note If heap is NULL or uninitialized, returns LOOPY_HEAP_EMPTY_VALUE.
 *       The function is safe to call with NULL.
 *
 * @see loopyMaxHeapPop() to get and remove the maximum
 * @see loopyMaxHeapIsEmpty() to check if heap is empty
 * @see loopyMaxHeapSize() to get the number of elements
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(10);
 * loopyMaxHeapInsert(heap, 50);
 * loopyMaxHeapInsert(heap, 30);
 * loopyMaxHeapInsert(heap, 70);
 *
 * loopyHeapValue max = loopyMaxHeapPeek(heap); // Returns 70
 * loopyHeapValue max2 = loopyMaxHeapPeek(heap); // Still 70 (not removed)
 *
 * loopyMaxHeapFree(heap);
 * @endcode
 */
loopyHeapValue loopyMaxHeapPeek(const loopyMaxHeap *heap);

/**
 * @brief Removes and returns the maximum value from the heap.
 *
 * Removes the root element (maximum) from the max-heap and restores the
 * heap property. The last element is moved to the root and then "bubbled down"
 * to its correct position. This is O(log n) where n is heap size. The position
 * map is updated to reflect the removal and element relocation.
 *
 * @param heap Pointer to an initialized loopyMaxHeap. Must not be NULL and
 *             must be initialized with loopyMaxHeapInit() or loopyMaxHeapNew().
 *
 * @return The maximum value (root element) if heap is non-empty, or
 *         LOOPY_HEAP_EMPTY_VALUE (-1) if heap is empty. Note that -1 is a
 *         valid heap value, so check heap size before calling if you need to
 *         distinguish empty from containing -1.
 *
 * @note This function modifies the heap: it decrements size and reorders
 *       elements to maintain max-heap property.
 *
 * @note Time complexity: O(log n) where n is the heap size.
 *
 * @note After pop, the element is no longer in the heap. The position map
 *       entry for that element is cleared.
 *
 * @note To check if heap is empty before calling, use loopyMaxHeapIsEmpty().
 *
 * @see loopyMaxHeapPeek() to get maximum without removal
 * @see loopyMaxHeapInsert() to add elements
 * @see loopyMaxHeapRemove() to remove arbitrary elements
 * @see loopyMaxHeapIsEmpty() to check if heap is empty
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(10);
 * loopyMaxHeapInsert(heap, 50);
 * loopyMaxHeapInsert(heap, 30);
 * loopyMaxHeapInsert(heap, 70);
 *
 * loopyHeapValue max = loopyMaxHeapPop(heap); // Returns 70, size now 2
 * loopyHeapValue max2 = loopyMaxHeapPop(heap); // Returns 50, size now 1
 *
 * loopyMaxHeapFree(heap);
 * @endcode
 */
loopyHeapValue loopyMaxHeapPop(loopyMaxHeap *heap);

/**
 * @brief Removes a specific value from the heap.
 *
 * Locates and removes an arbitrary element from the heap while maintaining
 * the max-heap property. For non-negative values, uses O(1) position map
 * lookup; for negative values or position map misses, falls back to O(n) linear
 * search. The removal process replaces the removed element with the last
 * element and then restores heap property via bubble-up or bubble-down as
 * needed.
 *
 * @param heap Pointer to an initialized loopyMaxHeap. Must not be NULL and
 *             must be initialized with loopyMaxHeapInit() or loopyMaxHeapNew().
 *
 * @param value The value to remove. Searches for the first occurrence if
 *              duplicates exist.
 *
 * @return true if the value was found in the heap and successfully removed,
 *         false if the value was not found or heap is empty.
 *
 * @note Time complexity: O(log n) for non-negative values (position map hit),
 *       O(n) for negative values or position map misses (linear search
 * fallback). n is the heap size.
 *
 * @note If the heap contains duplicate values, only the first occurrence is
 *       removed (the one found by position map or linear search).
 *
 * @note The position map entry for the removed value is cleared. If the removed
 *       element is not the last in the heap, the last element is moved to the
 *       removed position and reheapified.
 *
 * @note Removing the last element is O(1) since no reheapification is needed.
 *
 * @see loopyMaxHeapPop() to remove the maximum element
 * @see loopyMaxHeapContains() to check if value exists
 * @see loopyMaxHeapInsert() to add elements
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(10);
 * loopyMaxHeapInsert(heap, 50);
 * loopyMaxHeapInsert(heap, 30);
 * loopyMaxHeapInsert(heap, 70);
 *
 * bool found = loopyMaxHeapRemove(heap, 30); // true, 30 is removed
 * found = loopyMaxHeapRemove(heap, 30); // false, 30 no longer in heap
 *
 * loopyHeapValue max = loopyMaxHeapPeek(heap); // Returns 70
 *
 * loopyMaxHeapFree(heap);
 * @endcode
 */
bool loopyMaxHeapRemove(loopyMaxHeap *heap, loopyHeapValue value);

/**
 * @brief Checks if a value exists in the heap.
 *
 * Determines whether a specific value is currently in the heap. For
 * non-negative values, attempts O(1) position map lookup first. If the position
 * map lookup succeeds, the function returns immediately. Otherwise, or for
 * negative values, falls back to O(n) linear scan of the entire heap.
 *
 * @param heap Pointer to an initialized loopyMaxHeap, or NULL.
 *
 * @param value The value to search for.
 *
 * @return true if the value is found in the heap, false if the value is not
 *         found, heap is empty, or heap is NULL/uninitialized.
 *
 * @note Time complexity: O(1) average case for non-negative values (position
 *       map hit), O(n) worst case for negative values or position map misses.
 *       n is the heap size.
 *
 * @note This function does not modify the heap.
 *
 * @note The function is safe to call with NULL heap pointers.
 *
 * @note If the heap contains duplicate values, this function returns true if
 *       at least one occurrence is found.
 *
 * @see loopyMaxHeapRemove() to remove elements
 * @see loopyMaxHeapSize() to get number of elements
 * @see loopyMaxHeapIsEmpty() to check if heap is empty
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(10);
 * loopyMaxHeapInsert(heap, 50);
 * loopyMaxHeapInsert(heap, 30);
 * loopyMaxHeapInsert(heap, 70);
 *
 * bool found = loopyMaxHeapContains(heap, 30); // true
 * found = loopyMaxHeapContains(heap, 999); // false
 *
 * loopyMaxHeapFree(heap);
 * @endcode
 */
bool loopyMaxHeapContains(const loopyMaxHeap *heap, loopyHeapValue value);

/* ====================================================================
 * Utility
 * ==================================================================== */

/**
 * @brief Checks if the heap is empty (contains no elements).
 *
 * Determines whether the heap has no elements (size == 0). This is a simple
 * O(1) query that checks the size field. Safe to call on NULL pointers.
 *
 * @param heap Pointer to a loopyMaxHeap, or NULL.
 *
 * @return true if the heap is NULL, uninitialized, or contains no elements
 *         (size == 0), false if the heap contains one or more elements.
 *
 * @note Time complexity: O(1)
 *
 * @note This function is safe to call with NULL pointers.
 *
 * @note Use this before calling loopyMaxHeapPeek() or loopyMaxHeapPop() if
 *       you need to distinguish between empty heaps and heaps containing the
 *       sentinel value LOOPY_HEAP_EMPTY_VALUE (-1).
 *
 * @see loopyMaxHeapSize() to get the exact number of elements
 * @see loopyMaxHeapPeek() to get the maximum without removal
 * @see loopyMaxHeapPop() to remove the maximum
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(10);
 *
 * if (loopyMaxHeapIsEmpty(heap)) {
 *     // Heap is empty
 * }
 *
 * loopyMaxHeapInsert(heap, 42);
 *
 * if (!loopyMaxHeapIsEmpty(heap)) {
 *     loopyHeapValue max = loopyMaxHeapPop(heap);
 * }
 *
 * loopyMaxHeapFree(heap);
 * @endcode
 */
bool loopyMaxHeapIsEmpty(const loopyMaxHeap *heap);

/**
 * @brief Gets the current number of elements in the heap.
 *
 * Returns the exact count of elements currently stored in the heap. This is
 * an O(1) query that directly returns the size field.
 *
 * @param heap Pointer to a loopyMaxHeap, or NULL.
 *
 * @return The number of elements in the heap. Returns 0 if heap is NULL or
 *         uninitialized.
 *
 * @note Time complexity: O(1)
 *
 * @note This function is safe to call with NULL pointers.
 *
 * @note The returned size is always <= capacity. See loopyMaxHeapCapacity()
 *       to get the allocated capacity.
 *
 * @see loopyMaxHeapCapacity() to get the allocated capacity
 * @see loopyMaxHeapIsEmpty() to check if heap has no elements
 * @see loopyMaxHeapReserve() to pre-allocate capacity
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(10);
 *
 * loopyMaxHeapInsert(heap, 50);
 * loopyMaxHeapInsert(heap, 30);
 * loopyMaxHeapInsert(heap, 70);
 *
 * size_t count = loopyMaxHeapSize(heap); // Returns 3
 * size_t cap = loopyMaxHeapCapacity(heap); // Returns at least 10
 *
 * loopyMaxHeapFree(heap);
 * @endcode
 */
size_t loopyMaxHeapSize(const loopyMaxHeap *heap);

/**
 * @brief Gets the current allocated capacity of the heap.
 *
 * Returns the maximum number of elements the heap can hold without growing.
 * The capacity represents the size of the underlying data array (minus the
 * reserved index 0). This is an O(1) query that directly returns the capacity
 * field.
 *
 * @param heap Pointer to a loopyMaxHeap, or NULL.
 *
 * @return The current capacity (number of elements that can be stored before
 *         growth is needed). Returns 0 if heap is NULL or uninitialized.
 *
 * @note Time complexity: O(1)
 *
 * @note This function is safe to call with NULL pointers.
 *
 * @note The capacity is always >= size. When size reaches capacity, the next
 *       insert triggers automatic doubling of capacity.
 *
 * @note Initial capacity defaults to 16 if 0 is passed to loopyMaxHeapNew()
 *       or loopyMaxHeapInit().
 *
 * @see loopyMaxHeapSize() to get the current number of elements
 * @see loopyMaxHeapReserve() to pre-allocate specific capacity
 * @see loopyMaxHeapInsert() which auto-grows when size == capacity
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(10);
 *
 * size_t cap = loopyMaxHeapCapacity(heap); // Returns 10
 * size_t size = loopyMaxHeapSize(heap); // Returns 0
 *
 * // Add many elements (will trigger growth)
 * for (int i = 0; i < 20; i++) {
 *     loopyMaxHeapInsert(heap, i);
 * }
 *
 * cap = loopyMaxHeapCapacity(heap); // Returns at least 20
 *
 * loopyMaxHeapFree(heap);
 * @endcode
 */
size_t loopyMaxHeapCapacity(const loopyMaxHeap *heap);

/**
 * @brief Pre-allocates capacity for at least the specified number of elements.
 *
 * Grows the heap's data array to ensure it can hold at least the specified
 * number of elements without needing further allocation. This is useful for
 * avoiding multiple allocations when the final size is known in advance. If
 * the requested capacity is less than or equal to the current capacity, the
 * function succeeds immediately without allocation.
 *
 * @param heap Pointer to an initialized loopyMaxHeap. Must not be NULL and
 *             must be initialized with loopyMaxHeapInit() or loopyMaxHeapNew().
 *
 * @param capacity The minimum capacity to reserve. If this is <= current
 *                 capacity, the function succeeds without growing.
 *
 * @return true if the requested capacity is successfully reserved (or already
 *         available), false if memory allocation fails.
 *
 * @note Time complexity: O(1) if capacity <= current capacity, O(n) for
 *       allocation and memory copy if growing is needed, where n is the
 *       current capacity.
 *
 * @note This function only grows, never shrinks. If capacity < current
 *       capacity, the current capacity is preserved.
 *
 * @note The position map is not directly resized by this function; it grows
 *       on-demand when values require it.
 *
 * @note Pre-reserving capacity can improve performance when you know the
 *       approximate final size.
 *
 * @see loopyMaxHeapCapacity() to get current capacity
 * @see loopyMaxHeapSize() to get current number of elements
 * @see loopyMaxHeapInsert() which auto-grows as needed
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(10);
 *
 * // Pre-allocate for 1000 elements to avoid repeated growth
 * if (!loopyMaxHeapReserve(heap, 1000)) {
 *     // Handle allocation failure
 *     loopyMaxHeapFree(heap);
 *     return false;
 * }
 *
 * // Now insert up to 1000 elements without reallocation
 * for (int i = 0; i < 1000; i++) {
 *     loopyMaxHeapInsert(heap, i);
 * }
 *
 * loopyMaxHeapFree(heap);
 * @endcode
 */
bool loopyMaxHeapReserve(loopyMaxHeap *heap, size_t capacity);

/**
 * @brief Clears all elements from the heap without deallocating memory.
 *
 * Removes all elements from the heap by resetting the size to 0 and clearing
 * all position map entries. The underlying data array and position map remain
 * allocated and unchanged, so capacity is preserved. This is useful for
 * reusing a heap without the overhead of deallocation and reallocation.
 *
 * @param heap Pointer to an initialized loopyMaxHeap, or NULL. The function
 *             safely handles NULL pointers.
 *
 * @return void
 *
 * @note Time complexity: O(capacity of position map), which is typically
 *       proportional to the largest value inserted.
 *
 * @note This function is safe to call with NULL pointers.
 *
 * @note After clearing, loopyMaxHeapIsEmpty() returns true, and
 *       loopyMaxHeapSize() returns 0.
 *
 * @note The allocated capacity (from loopyMaxHeapCapacity()) is preserved
 *       after clear, allowing efficient reuse.
 *
 * @note This is more efficient than delete/recreate when you need to reuse
 *       a heap with similar size constraints.
 *
 * @see loopyMaxHeapIsEmpty() to check if heap is empty
 * @see loopyMaxHeapSize() to get current number of elements
 * @see loopyMaxHeapDeinit() to fully deallocate
 *
 * @code
 * loopyMaxHeap *heap = loopyMaxHeapNew(100);
 *
 * // First batch of operations
 * loopyMaxHeapInsert(heap, 50);
 * loopyMaxHeapInsert(heap, 30);
 * loopyMaxHeapInsert(heap, 70);
 *
 * size_t cap1 = loopyMaxHeapCapacity(heap); // e.g., 100
 *
 * // Clear for reuse
 * loopyMaxHeapClear(heap);
 *
 * size_t size = loopyMaxHeapSize(heap); // Now 0
 * size_t cap2 = loopyMaxHeapCapacity(heap); // Still 100 (preserved)
 *
 * // Reuse the heap with the same capacity
 * loopyMaxHeapInsert(heap, 10);
 * loopyMaxHeapInsert(heap, 20);
 *
 * loopyMaxHeapFree(heap);
 * @endcode
 */
void loopyMaxHeapClear(loopyMaxHeap *heap);

/* Sentinel value returned when heap is empty */
#define LOOPY_HEAP_EMPTY_VALUE (-1)
