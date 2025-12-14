/* loopyPlatform.h - Platform-specific feature macros for loopy
 *
 * This header must be included FIRST in all loopy .c files to ensure
 * proper exposure of POSIX/BSD/GNU APIs across different platforms.
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

#ifndef LOOPY_PLATFORM_H
#define LOOPY_PLATFORM_H

/* Platform-specific feature test macros
 * These MUST be defined before any system headers are included */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* Enables all glibc features: GNU + POSIX + BSD */
#endif

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* Enables BSD extensions on macOS */
#endif

#if !defined(__linux__) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L /* POSIX.1-2001 for other platforms */
#endif

#endif /* LOOPY_PLATFORM_H */
