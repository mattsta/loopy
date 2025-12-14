# Contributing to Loopy

Thank you for your interest in contributing to loopy! This document provides guidelines and instructions for contributing.

## Getting Started

### Prerequisites

- C99 or later compiler (gcc, clang, msvc)
- CMake 3.15+
- Git

### Building from Source

```bash
git clone https://github.com/mattsta/loopy.git
cd loopy
mkdir build && cd build
cmake ..
make -j8
```

### Running Tests

```bash
# Run all tests
ctest -j8 --verbose

# Run specific test category
ctest -R "unit" --verbose
ctest -R "example" --verbose

# Run with stress testing
./src/loopyStressCLI --duration 10
```

### Running Examples

```bash
# Echo server (run in background)
./examples/echo_server &

# DNS lookup
./examples/dns_lookup google.com

# File watcher
./examples/file_watcher /tmp

# Interactive pub/sub chat
./examples/pubsub_chat --interactive

# Networked pub/sub
./examples/pubsub_network --test
```

## Code Style

### Naming Conventions

Follow the API conventions documented in `docs/API_CONVENTIONS.md`:

- **Functions**: `loopy<Module><Action>()` - camelCase, always `loopy` prefix
- **Constants**: `LOOPY_<MODULE>_<NAME>` - UPPERCASE with underscores
- **Callback typedefs**: `loopy<Module><Event>Callback` - PascalCase
- **Iterator typedefs**: `loopy<Module><Event>Fn` - PascalCase
- **Config structs**: `loopy<Module>Config` - PascalCase

Examples:

```c
// Good
loopyStreamRead(stream, buffer, size, callback, userData);
LOOPY_STREAM_MAX_FDS
loopyStreamReadCallback
loopySubscriptionIterFn

// Bad
stream_read(stream, ...);  // Wrong prefix
STREAM_MAX_FDS            // Missing LOOPY_ prefix
stream_read_cb            // Wrong naming style
```

### Memory Management

**Rule**: Use consistent `loopy<Module>New()` and `loopy<Module>Free()` pairs.

```c
// Good
loopyStream *stream = loopyStreamNew(loop, host, port);
// ...
loopyStreamFree(stream);

// Bad
loopyStreamCreate();     // Use "New" not "Create"
loopyStreamDestroy();    // Use "Free" not "Destroy"
```

### Parameter Ordering

**Standard order for all functions:**

1. **Handle/context** (if applicable) - e.g., `loopyStream *stream`
2. **Required input parameters** - e.g., `const char *host`
3. **Optional input parameters** - documented in function comment
4. **Callback function** (if async) - `loopy<X>Callback callback`
5. **User data** - `void *userData`
6. **Output parameters** (sync only) - e.g., `int *outValue`

```c
// Good - follows standard order
bool loopyStreamWrite(loopyStream *stream,
                      const void *data,
                      size_t length,
                      loopyStreamWriteCallback callback,
                      void *userData);

// Bad - callback before required parameters
bool loopyStreamWrite(loopyStream *stream,
                      loopyStreamWriteCallback callback,
                      const void *data,
                      size_t length,
                      void *userData);
```

### Encapsulation

- **Opaque structs**: Use forward declaration in public header, struct definition in `loopyInternal.h`
- **Accessor functions**: Always provide `Get`/`Set`/`Is` functions for important properties
- **Config structs**: Keep transparent (users need to initialize them), provide init functions

```c
// Good - loopyStream is opaque
struct loopyStream;  // Public header
typedef struct loopyStream loopyStream;

// Accessors
loopyLoop *loopyStreamGetLoop(const loopyStream *s);
void *loopyStreamGetData(const loopyStream *s);
void loopyStreamSetData(loopyStream *s, void *data);

// Bad - exposing internal state
struct loopyStream {
    int fd;
    int flags;
    // ... internals
};
```

### Error Handling

**Rule**: Use the `loopyStatus` enum for consistency.

```c
// Return status for complex operations
loopyStatus result = loopyDNSResolve(dns, host, callback, userData);
if (result != LOOPY_OK) {
    // Handle error
    fprintf(stderr, "Error: %s\n", loopyStatusString(result));
}

// Boolean for simple success/fail
bool ok = loopyTimerCancel(timer);
if (!ok) {
    fprintf(stderr, "Failed to cancel timer\n");
}

// Get module-specific errors
const char *error = loopyNetGetError(net);
if (error) {
    fprintf(stderr, "Network error: %s\n", error);
}
```

### Callback Patterns

**Rule**: First parameter is always the handle, last is always `void *userData`.

```c
// Good - standard callback signature
typedef void (*loopyStreamReadCallback)(
    loopyStream *stream,
    const void *data,
    size_t nread,
    int status,
    void *userData);

// Bad - userData not last
typedef void (*loopyStreamReadCallback)(
    loopyStream *stream,
    void *userData,
    const void *data,
    size_t nread,
    int status);
```

### Comments and Documentation

Use Doxygen-style comments for all public functions:

```c
/**
 * Read data from a stream asynchronously.
 *
 * The callback will be invoked when data is available or an error occurs.
 * Multiple read requests can be pending simultaneously.
 *
 * @param stream    The stream handle (required)
 * @param callback  Invoked with data (required)
 * @param userData  Passed to callback (optional)
 * @return true if request accepted, false if stream is closed or invalid
 *
 * @note Thread-unsafe: must be called from the loop's thread
 * @note The stream takes ownership of the callback registration
 * @see loopyStreamClose() to close the stream
 *
 * Example:
 * @code
 * static void onRead(loopyStream *s, const void *data, size_t n, int status, void *ud) {
 *     if (status == LOOPY_OK) {
 *         printf("Received: %.*s\n", (int)n, (const char*)data);
 *     }
 * }
 * loopyStreamRead(stream, onRead, NULL);
 * @endcode
 */
bool loopyStreamRead(loopyStream *stream,
                     loopyStreamReadCallback callback,
                     void *userData);
```

### Code Formatting

- **Line length**: Aim for 80-100 characters (hard limit 120)
- **Indentation**: 4 spaces (no tabs)
- **Braces**: Opening brace on same line (K&R style)
- **Spacing**: Space after keywords (if, for, while)

```c
// Good
if (condition) {
    doSomething();
    for (int i = 0; i < n; i++) {
        processItem(i);
    }
} else {
    doOtherThing();
}

// Bad
if(condition){
    doSomething();
}else{
    doOtherThing();
}
```

### Header Guards

Use `#pragma once` for simplicity and modern compiler support:

```c
#pragma once

// Content...
```

## Submitting Changes

### Before You Start

1. Check existing issues and PRs to avoid duplicate work
2. Create an issue to discuss major changes before implementing
3. For bugs, provide a minimal reproducible example

### Creating a Pull Request

1. **Fork and branch**: Create a feature branch from `main`

   ```bash
   git checkout -b feature/your-feature-name
   ```

2. **Make changes**: Follow code style guidelines above

3. **Test thoroughly**:

   ```bash
   make -j8                    # Rebuild
   ctest --verbose             # Run tests
   ./src/loopyStressCLI        # Stress test
   ```

4. **Commit with clear messages**:

   ```bash
   git commit -m "Add feature: description of what and why"
   ```

5. **Push and create PR**:

   ```bash
   git push origin feature/your-feature-name
   ```

6. **PR checklist**:
   - [ ] Tests pass (`ctest --verbose`)
   - [ ] No memory leaks (`valgrind ./src/loopyUnitTest`)
   - [ ] Documentation updated (if API change)
   - [ ] Commit messages clear and descriptive
   - [ ] Code follows style guidelines

## Architecture Guidelines

When adding new features, ensure:

1. **Module isolation**: Changes should be contained within a module
2. **Opaque interfaces**: Expose only what's necessary
3. **Consistent patterns**: Follow existing conventions in the codebase
4. **Error handling**: Use appropriate status codes
5. **Platform support**: Test on at least Linux and macOS
6. **Documentation**: Update docs/ for user-facing features
7. **Tests**: Add unit tests and integration tests

See `docs/ARCHITECTURE.md` for the module hierarchy and design principles.

## Testing Requirements

### Unit Tests

Add tests in the appropriate section of `src/loopyUnitTest.c`:

```c
// Good - clear test structure
{
    loopyLoop *loop = loopyNew();
    loopyStream *stream = loopyStreamNew(loop, "127.0.0.1", 9000);

    // Test code
    TEST_ASSERT(stream != NULL);

    loopyStreamFree(stream);
    loopyFree(loop);
}
```

### Integration Tests

For features that need end-to-end testing, add example tests to `examples/`:

1. Create example file demonstrating feature
2. Add `--test` mode that validates behavior
3. Register with CTest in `examples/CMakeLists.txt`

### Platform Testing

- Linux (epoll): Tested on GitHub Actions
- macOS (kqueue): Tested on GitHub Actions
- Other platforms: Manual testing appreciated

## Documentation

### For User-Facing Features

1. **Update README.md** if adding a major feature
2. **Add docs/FEATURE_GUIDE.md** for complex features
3. **Update existing docs** (e.g., `docs/USAGE_GUIDE.md`)
4. **Add example** demonstrating the feature
5. **Document in Doxygen style** in the header

### For Internal Changes

1. **Update docs/ARCHITECTURE.md** if changing module structure
2. **Update docs/API_CONVENTIONS.md** if changing conventions
3. **Update CHANGELOG.md** for significant changes

## Release Process

The maintainers handle releases. For each release:

1. Update version in `CMakeLists.txt`
2. Update `CHANGELOG.md` with user-facing changes
3. Tag release: `git tag -a v1.0.1 -m "Release 1.0.1"`
4. Push tag: `git push origin v1.0.1`

## Questions?

- Check docs/ directory for architecture and usage guides
- Open an issue for bugs or feature requests
- Discussions welcome in existing issues and PRs

---

**Thank you for contributing to loopy!**
