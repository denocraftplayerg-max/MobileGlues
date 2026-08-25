// MobileGlues - gl/buffer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
#ifndef MOBILEGLUES_BUFFER_H
#define GL_GLEXT_PROTOTYPES
#include "../config/settings.h"
#include "../gles/loader.h"
#include "../includes.h"
#include "glcorearb.h"
#include "log.h"
#include "mg.h"
#include <GL/gl.h>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <atomic>

#ifdef __cplusplus
extern "C"
{
#endif

    // The real/driver name bound to target, as GLES.glGetIntegerv(<target>_BINDING)
    // would report it -- the two functions above answer with this layer's own
    // names, which the driver has never heard of. Computed from tracked state, so
    // it costs nothing and is safe to call per draw, and it is the value to hand
    // straight back to GLES.glBindBuffer when restoring a temporary bind.
    //
    // Only valid where the driver's binding still agrees with the tracked one; the
    // comment on the definition in gl/buffer.cpp lists where it does not.
    GLuint mg_driver_bound_buffer(GLenum target);

    // Persistent buffer mapping support (GL_EXT_buffer_storage)
    struct PersistentBufferMapping {
        GLuint buffer = 0;
        void* mapped_ptr = nullptr;
        GLintptr offset = 0;
        GLsizeiptr length = 0;
        GLbitfield access = 0;
        bool active = false;
    };

    // Track persistent mappings per buffer
    extern std::unordered_map<GLuint, PersistentBufferMapping> g_persistent_buffer_mappings;
    extern std::atomic<uint64_t> g_buffer_upload_time_us;
    extern std::atomic<bool> g_persistent_buffer_enabled;

    // Get or create persistent mapping for a buffer
    void* get_persistent_buffer_mapping(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access);
    void flush_persistent_buffer_mapping(GLuint buffer);
    void unmap_persistent_buffer(GLuint buffer);

    GLuint gen_buffer();

    GLboolean has_buffer(GLuint key);

    void modify_buffer(GLuint key, GLuint value);

    void remove_buffer(GLuint key);

    GLuint find_real_buffer(GLuint key);

    // key is a *_BINDING query enum, the kind glGetIntegerv is given.
    GLuint find_bound_buffer(GLenum key);
    // target is a bind target, the kind glBindBuffer is given. Not interchangeable
    // with the above: each rejects the other's enums and returns 0.
    GLuint find_bound_buffer_by_target(GLenum target);

    GLuint gen_array();

    GLboolean has_array(GLuint key);

    void modify_array(GLuint key, GLuint value);

    void remove_array(GLuint key);

    GLuint find_real_array(GLuint key);

    GLuint find_bound_array();

    static GLenum get_binding_query(GLenum target);

    void InitBufferMap(size_t expectedSize);

    void InitVertexArrayMap(size_t expectedSize);

    GLAPI GLAPIENTRY void glGenBuffers(GLsizei n, GLuint* buffers);

    GLAPI GLAPIENTRY void glDeleteBuffers(GLsizei n, const GLuint* buffers);

    GLAPI GLAPIENTRY GLboolean glIsBuffer(GLuint buffer);

    GLAPI GLAPIENTRY void glBindBuffer(GLenum target, GLuint buffer);

    GLAPI GLAPIENTRY void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                                            GLsizeiptr size);

    GLAPI GLAPIENTRY void glBindBufferBase(GLenum target, GLuint index, GLuint buffer);

    GLAPI GLAPIENTRY void glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);

    GLAPI GLAPIENTRY void glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer);

    GLAPI GLAPIENTRY void glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset,
                                           GLsizeiptr size);

    GLAPI GLAPIENTRY GLboolean glUnmapBuffer(GLenum target);

    GLAPI GLAPIENTRY void* glMapBuffer(GLenum target, GLenum access);

    GLAPI GLAPIENTRY void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);

    GLAPI GLAPIENTRY void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);

    GLAPI GLAPIENTRY void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);

    GLAPI GLAPIENTRY void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);

    // Persistent buffer stats API
    GLAPI GLAPIENTRY void glGetPersistentBufferStatsEXT(uint64_t* upload_time_us, bool* enabled);
    GLAPI GLAPIENTRY void glSetPersistentBufferEnabledEXT(bool enabled);
    GLAPI GLAPIENTRY void glResetPersistentBufferStatsEXT();

    GLAPI GLAPIENTRY void glGenVertexArrays(GLsizei n, GLuint* arrays);

    GLAPI GLAPIENTRY void glDeleteVertexArrays(GLsizei n, const GLuint* arrays);

    GLAPI GLAPIENTRY GLboolean glIsVertexArray(GLuint array);

    GLAPI GLAPIENTRY void glBindVertexArray(GLuint array);

#ifdef __cplusplus
}
#endif

#define MOBILEGLUES_BUFFER_H

#endif // MOBILEGLUES_BUFFER_H