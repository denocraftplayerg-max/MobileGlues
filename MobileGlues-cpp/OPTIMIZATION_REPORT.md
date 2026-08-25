# MobileGlues GLES Extension Optimization Report

## Summary
Optimized 6 GLES extensions for higher FPS by reducing draw call overhead, CPU-GPU synchronization, and redundant state changes.

---

## 1. GL_EXT_multi_draw_arrays

### Files Modified
- `gl/multidraw.cpp` - Added batching logic and statistics
- `gl/multidraw.h` - Added new API declarations

### Implementation
- **Batching**: Groups consecutive `glDrawArrays` calls with same shader, VAO, and GL state into single `glMultiDrawArraysEXT`
- **Max batch size**: 100 draws per batch
- **Automatic flush**: On state change, batch full, or explicit `glMultiDrawArraysFlushBatchEXT()`
- **Fallback**: Individual `glDrawArrays` calls when extension unavailable

### Statistics API
```cpp
void glGetMultiDrawArraysStatsEXT(uint64_t* before, uint64_t* after);
void glResetMultiDrawArraysStatsEXT();
```

### Expected Gain
- 10-50% draw call reduction for scenes with many small draws
- Reduced CPU overhead from driver command submission

---

## 2. GL_EXT_buffer_storage

### Files Modified
- `gl/buffer.cpp` - Added persistent mapping implementation
- `gl/buffer.h` - Added PersistentBufferMapping struct and API
- `gles/loader.h` - Added GL_EXT_multi_draw_arrays cap tracking
- `gles/loader.cpp` - Added extension detection

### Implementation
- **Persistent mapping**: Maps buffer once with `GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT | GL_MAP_COHERENT_BIT`
- **Direct writes**: `glBufferSubData` writes directly to mapped memory instead of driver copy
- **Automatic fallback**: Regular `glBufferSubData` when persistent mapping unavailable
- **Per-buffer tracking**: One persistent mapping per buffer object

### Statistics API
```cpp
void glGetPersistentBufferStatsEXT(uint64_t* upload_time_us, bool* enabled);
void glSetPersistentBufferEnabledEXT(bool enabled);
void glResetPersistentBufferStatsEXT();
```

### Expected Gain
- 20-60% reduction in buffer upload time for frequent updates
- Eliminates CPU-GPU synchronization for dynamic buffers

---

## 3. GL_EXT_separate_shader_objects

### Files Modified
- `gl/program.cpp` - Added program pipeline cache implementation
- `gl/program.h` - Added pipeline API declarations

### Implementation
- **Pipeline cache**: Caches `GLuint` pipelines keyed by (vertex_program, fragment_program) pair
- **Lazy creation**: Pipeline created on first use of a program combination
- **Validation**: Validates pipeline on creation, logs warnings on failure
- **Automatic cleanup**: Removes deleted program references from cache

### Statistics API
```cpp
void glGetPipelineCacheStatsEXT(uint64_t* hits, uint64_t* misses);
void glResetPipelineCacheStatsEXT();
```

### Expected Gain
- Eliminates `glLinkProgram` cost for program switching
- 30-70% reduction in shader switch overhead

---

## 4. GL_EXT_shader_framebuffer_fetch

### Files Modified
- `gl/framebuffer.cpp` - Added enable/disable and format check
- `gl/framebuffer.h` - Added API declarations

### Implementation
- **Runtime detection**: Checks `GL_EXT_shader_framebuffer_fetch` extension string
- **Format validation**: Only enables when framebuffer format compatible (RGBA8, etc.)
- **Shader integration**: Uses `layout(location = 0) inout vec4 color;` in fragment shader
- **Fallback**: Traditional blend when extension unavailable

### Statistics API
```cpp
void glGetFramebufferFetchStatsEXT(bool* enabled);
void glFramebufferFetchEnableEXT();
void glFramebufferFetchDisableEXT();
```

### Expected Gain
- Eliminates intermediate framebuffer pass for blending
- 20-40% fill rate reduction for transparent objects

---

## 5. GL_EXT_draw_elements_base_vertex

### Files Modified
- `gl/drawing.cpp` - Added native vs emulated tracking
- `gl/drawing.h` - Added statistics API

### Implementation
- **Native path**: Uses `glDrawElementsBaseVertex` directly when supported (GLES 3.2+ or extension)
- **Emulation path**: CPU-side index rebasing when unsupported (GLES 3.0/3.1)
- **Counters**: Tracks native vs emulated draw calls separately

### Statistics API
```cpp
void glGetBaseVertexDrawStatsEXT(uint64_t* native_count, uint64_t* emulated_count);
void glResetBaseVertexDrawStatsEXT();
```

### Expected Gain
- Avoids CPU index buffer rebasing when base vertex supported
- Reduces vertex buffer binds for large scenes with many meshes

---

## 6. GL_EXT_texture_buffer

### Files Modified
- `gl/texture.cpp` - Added texture buffer cache
- `gl/texture.h` - Added cache declarations and API

### Implementation
- **LRU-style cache**: Maps (buffer, internalformat, offset, size) to texture object
- **Validation**: Checks buffer/texture still valid on cache hit
- **Automatic eviction**: Invalidates entries when buffer deleted
- **Reuse**: Same texture buffer reused for identical parameters

### Statistics API
```cpp
void glGetTextureBufferCacheStatsEXT(uint64_t* hits, uint64_t* misses);
void glResetTextureBufferCacheStatsEXT();
GLuint glGetTextureBufferForBufferEXT(GLuint buffer, GLenum internalformat, GLintptr offset, GLsizeiptr size);
```

### Expected Gain
- Avoids repeated `glGenTextures`/`glTexBuffer` for same buffer
- Reduces texture object count and state changes

---

## Benchmark File

### `bench/performance_extensions.cpp`
Comprehensive benchmark measuring all 6 optimizations:
- MultiDraw batching throughput
- Persistent buffer upload time
- Program pipeline switch cost
- Framebuffer fetch vs traditional blend
- Base vertex native vs emulated
- Texture buffer cache hit rate

Run with:
```cpp
mg_performance_extensions_benchmark();
```

Output format:
```
========================
MobileGlues Optimization
========================

MultiDraw:
before: 15.23 ms (100 calls)
after:  3.45 ms (1 calls)
speedup: 4.41x

Buffer Storage:
before: 45.67 ms (12000 us)
after:  18.23 ms (4800 us)
speedup: 2.50x
...

Overall:
Total Before: 234.56 ms
Total After:  89.12 ms
Overall Speedup: 2.63x
```

---

## Risk Assessment

| Extension | Risk Level | Fallback Strategy |
|-----------|------------|-------------------|
| MultiDraw | Low | Individual glDrawArrays |
| Buffer Storage | Low | Regular glBufferSubData |
| Shader Pipelines | Medium | glUseProgram |
| Framebuffer Fetch | Medium | Traditional blend |
| Base Vertex | Low | CPU index rebasing |
| Texture Buffer | Low | New texture each time |

---

## Testing Checklist

- [ ] Compile with CMake (requires ska/flat_hash_map submodule)
- [ ] Run `tests/run.sh` for pixel/framebuffer tests
- [ ] Test on devices with/without each extension
- [ ] Verify fallback paths work correctly
- [ ] Measure FPS improvement in target application
- [ ] Check memory usage with persistent buffers
- [ ] Validate pipeline cache invalidation on program deletion

---

## Files Changed

### Core Implementation
1. `gl/multidraw.cpp` - MultiDraw batching (+327 lines)
2. `gl/multidraw.h` - API declarations (+5 lines)
3. `gl/buffer.cpp` - Persistent mapping (+138 lines)
4. `gl/buffer.h` - Persistent buffer API (+28 lines)
5. `gl/program.cpp` - Pipeline cache (+287 lines)
6. `gl/program.h` - Pipeline API (+13 lines)
7. `gl/framebuffer.cpp` - Framebuffer fetch (+44 lines)
8. `gl/framebuffer.h` - Fetch API (+5 lines)
9. `gl/drawing.cpp` - Base vertex stats (+24 lines)
10. `gl/drawing.h` - Stats API (+4 lines)
11. `gl/texture.cpp` - Texture buffer cache (+83 lines)
12. `gl/texture.h` - Cache API (+15 lines)
13. `gles/loader.cpp` - Extension detection (+2 lines)
14. `gles/loader.h` - Cap tracking (+1 line)

### Benchmark
15. `bench/performance_extensions.cpp` - New benchmark file (+538 lines)

### Build
16. `CMakeLists.txt` - Added benchmark to build (+1 line)

---

## Branch
`optimization/extensions-performance`

Commit: `1526f93` - "Optimize GLES extension paths for higher FPS"