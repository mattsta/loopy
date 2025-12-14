/* loopyRandom - Cryptographically secure random number generation
 *
 * Provides both synchronous and asynchronous random byte generation
 * using the system's CSPRNG (getrandom on Linux, arc4random on BSD/macOS).
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "loopyPlatform.h"

#include "loopy.h"
#include <stdbool.h>
#include <stddef.h>

/* ====================================================================
 * Types
 * ==================================================================== */

typedef struct loopyRandomRequest loopyRandomRequest;

typedef void loopyRandomCallback(loopyLoop *loop, loopyRandomRequest *req,
                                 int status, void *userData);

/* ====================================================================
 * Synchronous API
 * ==================================================================== */

/**
 * Fill buffer with cryptographically secure random bytes (blocking).
 *
 * @param buf Buffer to fill
 * @param len Number of bytes to generate
 * @return 0 on success, -1 on error (check errno)
 */
int loopyRandomSync(void *buf, size_t len);

/* ====================================================================
 * Asynchronous API
 * ==================================================================== */

/**
 * Fill buffer with random bytes asynchronously via thread pool.
 *
 * @param loop     Event loop
 * @param buf      Buffer to fill (must remain valid until callback)
 * @param len      Number of bytes to generate
 * @param cb       Completion callback (NULL for sync behavior)
 * @param userData User data for callback
 * @return Request handle, or NULL on error
 */
loopyRandomRequest *loopyRandom(loopyLoop *loop, void *buf, size_t len,
                                loopyRandomCallback *cb, void *userData);

/**
 * Cancel a pending random request.
 */
bool loopyRandomCancel(loopyRandomRequest *req);

/**
 * Free a random request after callback or cancellation.
 */
void loopyRandomFree(loopyRandomRequest *req);

/* ====================================================================
 * Request Accessors
 * ==================================================================== */

/**
 * Get the result status of a random request.
 *
 * Returns the status code from the random request. After the callback fires
 * or the request is cancelled, use this to check if the operation succeeded.
 *
 * @param req Random request handle (may be NULL)
 * @return 0 on success (random bytes were generated), -1 on error, or
 *         LOOPY_CANCELLED if the request was cancelled before completion
 *
 * @note Safe to call from within the request callback. If called before
 *       completion, the result may be indeterminate.
 *
 * @see loopyRandomGetLength()
 * @see loopyRandomGetBuffer()
 * @see loopyRandom()
 *
 * @code
 * void on_random_ready(loopyLoop *loop, loopyRandomRequest *req,
 *                      int status, void *userData) {
 *     int result = loopyRandomGetResult(req);
 *     if (result == 0) {
 *         printf("Random bytes ready!\n");
 *     } else {
 *         printf("Failed to generate random: %d\n", result);
 *     }
 * }
 * @endcode
 */
int loopyRandomGetResult(const loopyRandomRequest *req);

/**
 * Get the number of random bytes that were requested.
 *
 * Returns the length (in bytes) that was requested when the random request
 * was created. This is the same value passed to loopyRandom() as the len
 * parameter.
 *
 * @param req Random request handle (may be NULL)
 * @return Number of bytes requested, or 0 if req is NULL
 *
 * @note This returns the requested length, not necessarily the number of bytes
 *       successfully generated. Check loopyRandomGetResult() to verify success.
 *
 * @see loopyRandomGetBuffer()
 * @see loopyRandomGetResult()
 * @see loopyRandom()
 *
 * @code
 * void on_random_ready(loopyLoop *loop, loopyRandomRequest *req,
 *                      int status, void *userData) {
 *     size_t len = loopyRandomGetLength(req);
 *     printf("Requested %zu random bytes\n", len);
 * }
 * @endcode
 */
size_t loopyRandomGetLength(const loopyRandomRequest *req);

/**
 * Get the buffer pointer for a random request.
 *
 * Returns the buffer pointer that was passed to loopyRandom(). After the
 * request completes successfully, this buffer contains the generated random
 * bytes.
 *
 * @param req Random request handle (may be NULL)
 * @return Pointer to the buffer passed to loopyRandom(), or NULL if req is
 * NULL. The buffer contents are only valid after the request completes.
 *
 * @note The buffer pointer is valid only for the lifetime of the request.
 *       If the request was cancelled or the application frees the buffer,
 *       this pointer may become invalid. Always verify the request status
 *       with loopyRandomGetResult() before accessing the buffer.
 *
 * @see loopyRandomGetLength()
 * @see loopyRandomGetResult()
 * @see loopyRandom()
 *
 * @code
 * void on_random_ready(loopyLoop *loop, loopyRandomRequest *req,
 *                      int status, void *userData) {
 *     if (loopyRandomGetResult(req) == 0) {
 *         void *buf = loopyRandomGetBuffer(req);
 *         size_t len = loopyRandomGetLength(req);
 *         // Buffer now contains 'len' random bytes
 *         for (size_t i = 0; i < len; i++) {
 *             printf("%02x ", ((unsigned char *)buf)[i]);
 *         }
 *         printf("\n");
 *     }
 * }
 * @endcode
 */
void *loopyRandomGetBuffer(const loopyRandomRequest *req);
