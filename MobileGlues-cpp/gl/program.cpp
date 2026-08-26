// MobileGlues - gl/program.cpp
// Copyright (c) 2025 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1
// SPDX-License-Identifier: LGPL-2.1-only

#include <regex.h>
#include "GL/glext.h"
#include "GLES3/gl32.h"
#include "log.h"
#include "shader.h"
#include "program.h"
#include <regex>
#include <cstring>
#include <iostream>
#include "../config/settings.h"
#include "drawing.h"

#define DEBUG 0

extern UnorderedMap<GLuint, bool> shader_map_is_sampler_buffer_emulated;
UnorderedMap<GLuint, bool> program_map_is_sampler_buffer_emulated;

enum class ShouldGenerateFSState : int {
    Never = 0, Maybe = 1, Unknown = 2
};

UnorderedMap<GLuint, ShouldGenerateFSState> program_map_should_generate_fs;

// ============================================================================
// GL_EXT_separate_shader_objects - RESOLUCAO DE FUNCOES
// ============================================================================

typedef GLuint(GLAPIENTRY* mg_pfn_create_shader_programv_ext)(GLenum, GLsizei, const GLchar* const*);
typedef void(GLAPIENTRY* mg_pfn_gen_program_pipelines_ext)(GLsizei, GLuint*);
typedef void(GLAPIENTRY* mg_pfn_bind_program_pipeline_ext)(GLuint);
typedef void(GLAPIENTRY* mg_pfn_use_program_stages_ext)(GLuint, GLbitfield, GLuint);
typedef void(GLAPIENTRY* mg_pfn_delete_program_pipelines_ext)(GLsizei, const GLuint*);

static mg_pfn_create_shader_programv_ext  g_create_shader_programv_ext = nullptr;
static mg_pfn_gen_program_pipelines_ext   g_gen_program_pipelines_ext = nullptr;
static mg_pfn_bind_program_pipeline_ext   g_bind_program_pipeline_ext = nullptr;
static mg_pfn_use_program_stages_ext      g_use_program_stages_ext = nullptr;
static mg_pfn_delete_program_pipelines_ext g_delete_program_pipelines_ext = nullptr;

static bool g_separate_shader_resolved = false;
static bool g_separate_shader_available = false;

extern "C" void* gles;

static bool mg_gles_has_extension(const char* name) {
    if (!GLES.glGetStringi || !GLES.glGetIntegerv) return false;
    GLint count = 0;
    GLES.glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* s = GLES.glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (s && strcmp(reinterpret_cast<const char*>(s), name) == 0) return true;
    }
    return false;
}

static bool mg_separate_shader_objects_available() {
    if (g_separate_shader_resolved) return g_separate_shader_available;
    g_separate_shader_resolved = true;
    if (mg_gles_has_extension("GL_EXT_separate_shader_objects")) {
        if (gles && gles != reinterpret_cast<void*>(~(uintptr_t)0)) {
            g_create_shader_programv_ext  = reinterpret_cast<mg_pfn_create_shader_programv_ext>(dlsym(gles, "glCreateShaderProgramvEXT"));
            g_gen_program_pipelines_ext   = reinterpret_cast<mg_pfn_gen_program_pipelines_ext>(dlsym(gles, "glGenProgramPipelinesEXT"));
            g_bind_program_pipeline_ext   = reinterpret_cast<mg_pfn_bind_program_pipeline_ext>(dlsym(gles, "glBindProgramPipelineEXT"));
            g_use_program_stages_ext      = reinterpret_cast<mg_pfn_use_program_stages_ext>(dlsym(gles, "glUseProgramStagesEXT"));
            g_delete_program_pipelines_ext = reinterpret_cast<mg_pfn_delete_program_pipelines_ext>(dlsym(gles, "glDeleteProgramPipelinesEXT"));
            g_separate_shader_available = (g_create_shader_programv_ext && g_gen_program_pipelines_ext &&
                                          g_bind_program_pipeline_ext && g_use_program_stages_ext);
        }
    }
    return g_separate_shader_available;
}

// Cache de programas separados por shader
struct separate_shader_entry_t {
    GLuint program = 0;
    uint64_t use_count = 0;
};

static std::unordered_map<GLuint, separate_shader_entry_t> g_separate_vs_cache;
static std::unordered_map<GLuint, separate_shader_entry_t> g_separate_fs_cache;
static std::unordered_map<GLuint, separate_shader_entry_t> g_separate_gs_cache;
static std::mutex g_separate_shader_mutex;

// Pipeline cache
struct pipeline_key_t {
    GLuint vs, fs, gs;
    bool operator==(const pipeline_key_t& o) const { return vs == o.vs && fs == o.fs && gs == o.gs; }
};
struct pipeline_hash_t {
    size_t operator()(const pipeline_key_t& k) const noexcept {
        return ((size_t)k.vs * 73856093) ^ ((size_t)k.fs * 19349663) ^ ((size_t)k.gs * 83492791);
    }
};

struct pipeline_entry_t {
    GLuint pipeline = 0;
    uint64_t last_used = 0;
    uint64_t hit_count = 0;
};

static std::unordered_map<pipeline_key_t, pipeline_entry_t, pipeline_hash_t> g_pipeline_cache;
static std::mutex g_pipeline_mutex;
static uint64_t g_pipeline_frame = 0;

// ============================================================================
// CRIAR PROGRAMA SEPARADO PARA UM SHADER
// ============================================================================

static GLuint mg_get_or_create_separate_shader(GLuint shader, GLenum type) {
    std::unordered_map<GLuint, separate_shader_entry_t>* cache = nullptr;
    switch (type) {
    case GL_VERTEX_SHADER:   cache = &g_separate_vs_cache; break;
    case GL_FRAGMENT_SHADER: cache = &g_separate_fs_cache; break;
    case GL_GEOMETRY_SHADER: cache = &g_separate_gs_cache; break;
    default: return 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_separate_shader_mutex);
        auto it = cache->find(shader);
        if (it != cache->end() && it->second.program != 0) {
            it->second.use_count++;
            return it->second.program;
        }
    }

    if (!mg_separate_shader_objects_available() || !g_create_shader_programv_ext) return 0;

    GLint source_len = 0;
    GLES.glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &source_len);
    if (source_len <= 0) return 0;

    std::vector<GLchar> source(source_len);
    GLsizei actual_len = 0;
    GLES.glGetShaderSource(shader, source_len, &actual_len, source.data());

    const GLchar* src_ptr = source.data();
    GLuint program = g_create_shader_programv_ext(type, 1, &src_ptr);

    if (program) {
        std::lock_guard<std::mutex> lock(g_separate_shader_mutex);
        (*cache)[shader] = {program, 1};
    }

    return program;
}

// ============================================================================
// OBTER OU CRIAR PIPELINE
// ============================================================================

static GLuint mg_get_or_create_pipeline(GLuint vs, GLuint fs, GLuint gs) {
    pipeline_key_t key{vs, fs, gs};

    {
        std::lock_guard<std::mutex> lock(g_pipeline_mutex);
        auto it = g_pipeline_cache.find(key);
        if (it != g_pipeline_cache.end()) {
            it->second.last_used = ++g_pipeline_frame;
            it->second.hit_count++;
            return it->second.pipeline;
        }
    }

    if (!mg_separate_shader_objects_available()) return 0;

    GLuint pipeline = 0;
    g_gen_program_pipelines_ext(1, &pipeline);
    if (!pipeline) return 0;

    g_bind_program_pipeline_ext(pipeline);

    if (vs) {
        GLuint sp = mg_get_or_create_separate_shader(vs, GL_VERTEX_SHADER);
        if (sp) g_use_program_stages_ext(pipeline, GL_VERTEX_SHADER_BIT, sp);
    }
    if (fs) {
        GLuint sp = mg_get_or_create_separate_shader(fs, GL_FRAGMENT_SHADER);
        if (sp) g_use_program_stages_ext(pipeline, GL_FRAGMENT_SHADER_BIT, sp);
    }
    if (gs) {
        GLuint sp = mg_get_or_create_separate_shader(gs, GL_GEOMETRY_SHADER);
        if (sp) g_use_program_stages_ext(pipeline, GL_GEOMETRY_SHADER_BIT, sp);
    }

    std::lock_guard<std::mutex> lock(g_pipeline_mutex);
    g_pipeline_cache[key] = {pipeline, ++g_pipeline_frame, 1};
    return pipeline;
}

// ============================================================================
// glUseProgram OTIMIZADO COM PIPELINE SEPARADO
// ============================================================================

static thread_local GLuint g_current_pipeline = 0;

void glUseProgram(GLuint program) {
    LOG()
    LOG_D("glUseProgram(%d)", program)

    if (program == gl_state->current_program) {
        return;
    }

    // Tentar usar pipeline separado se disponivel
    if (mg_separate_shader_objects_available() && program != 0) {
        GLint num_shaders = 0;
        GLES.glGetProgramiv(program, GL_ATTACHED_SHADERS, &num_shaders);

        if (num_shaders > 0) {
            std::vector<GLuint> shaders(num_shaders);
            GLsizei count = 0;
            GLES.glGetAttachedShaders(program, num_shaders, &count, shaders.data());

            GLuint vs = 0, fs = 0, gs = 0;
            for (GLsizei i = 0; i < count; ++i) {
                GLint type = 0;
                GLES.glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
                if (type == GL_VERTEX_SHADER) vs = shaders[i];
                else if (type == GL_FRAGMENT_SHADER) fs = shaders[i];
                else if (type == GL_GEOMETRY_SHADER) gs = shaders[i];
            }

            if (vs && fs) {
                GLuint pipeline = mg_get_or_create_pipeline(vs, fs, gs);
                if (pipeline && pipeline != g_current_pipeline) {
                    g_bind_program_pipeline_ext(pipeline);
                    g_current_pipeline = pipeline;
                    gl_state->current_program = program;
                    CHECK_GL_ERROR
                    return;
                }
            }
        }
    }

    // Fallback: usar programa monolitico tradicional
    if (program != gl_state->current_program) {
        gl_state->current_program = program;
        g_current_pipeline = 0;
        GLES.glUseProgram(program);
        CHECK_GL_ERROR
    }
}

// ============================================================================
// RESTO DAS FUNCOES (sem alteracoes)
// ============================================================================

std::string updateLayoutLocation(const std::string& esslSource, GLuint color, const char* name) {
    const std::string& shaderCode = esslSource;
    std::string pattern = std::string(R"((layout\s*$[^)]*location\s*=\s*\d+[^)]*$\s*)?)") +
                          R"(out\s+((?:highp|mediump|lowp|\w+\s+)*\w+)\s+)" + name + R"(\s*;)";
    std::string replacement = "layout (location = " + std::to_string(color) + ") out $2 " + name + ";";
    std::regex reg(pattern);
    return std::regex_replace(shaderCode, reg, replacement);
}

void glBindFragDataLocation(GLuint program, GLuint color, const GLchar* name) {
    LOG()
    LOG_D("glBindFragDataLocation(%d, %d, %s)", program, color, name)
    if (strlen(name) > 8 && strncmp(name, "outColor", 8) == 0) {
        const char* numberStr = name + 8;
        bool isNumber = true;
        for (int i = 0; numberStr[i] != '\0'; ++i) {
            if (!isdigit(numberStr[i])) { isNumber = false; break; }
        }
        if (isNumber) {
            unsigned int extractedColor = static_cast<unsigned int>(std::stoul(numberStr));
            if (extractedColor == color) {
                LOG_D("Find outColor* with color *, skipping")
                return;
            }
        }
    }
    const std::string origin_glsl = shaderInfo.frag_data_changed ? shaderInfo.frag_data_changed_converted : shaderInfo.converted;
    shaderInfo.frag_data_changed_converted = updateLayoutLocation(origin_glsl, color, name);
    shaderInfo.frag_data_changed = 1;
}

static std::string DefaultFSSource;
static unsigned CurrentDefaultFSSourceVersion = 0;

void GenerateDefaultFSSource() {
    if (CurrentDefaultFSSourceVersion != hardware->es_version) {
        CurrentDefaultFSSourceVersion = hardware->es_version;
        std::ostringstream ss;
        ss << "#version " << CurrentDefaultFSSourceVersion << " es\n";
        ss << "precision mediump float;\n\n";
        ss << "out vec4 fragColor;\n\n";
        ss << "void main() {\n";
        ss << "    fragColor = vec4(1.0, 1.0, 1.0, 1.0);\n";
        ss << "}\n";
        DefaultFSSource = ss.str();
    }
}

static UnorderedMap<unsigned, GLuint> DefaultFSMap;

void glLinkProgram(GLuint program) {
    LOG()
    LOG_D("glLinkProgram(%d)", program)

    if (!shaderInfo.converted.empty() && shaderInfo.frag_data_changed) {
        const GLchar* patched = shaderInfo.frag_data_changed_converted.c_str();
        GLES.glShaderSource(shaderInfo.id, 1, &patched, nullptr);
        GLES.glCompileShader(shaderInfo.id);
        GLint status = 0;
        GLES.glGetShaderiv(shaderInfo.id, GL_COMPILE_STATUS, &status);
        if (status != GL_TRUE) {
            char tmp[500];
            GLES.glGetShaderInfoLog(shaderInfo.id, 500, nullptr, tmp);
            LOG_E("Failed to compile patched shader, log:\n%s", tmp)
        }
        GLES.glDetachShader(program, shaderInfo.id);
        GLES.glAttachShader(program, shaderInfo.id);
        CHECK_GL_ERROR
    }
    shaderInfo.id = 0;
    shaderInfo.converted = "";
    shaderInfo.frag_data_changed_converted.clear();
    shaderInfo.frag_data_changed = 0;

    if (program_map_should_generate_fs[program] == ShouldGenerateFSState::Maybe) {
        GenerateDefaultFSSource();
        GLuint& default_fs = DefaultFSMap[CurrentDefaultFSSourceVersion];
        if (!default_fs) {
            default_fs = GLES.glCreateShader(GL_FRAGMENT_SHADER);
            const char* src = DefaultFSSource.c_str();
            GLES.glShaderSource(default_fs, 1, &src, nullptr);
            GLES.glCompileShader(default_fs);
            GLint success = 0;
            GLES.glGetShaderiv(default_fs, GL_COMPILE_STATUS, &success);
            if (!success) {
                GLint logLength = 0;
                GLES.glGetShaderiv(default_fs, GL_INFO_LOG_LENGTH, &logLength);
                std::vector<char> log(logLength);
                GLES.glGetShaderInfoLog(default_fs, logLength, nullptr, log.data());
                LOG_E("Default fragment shader compile error for program %u :\n%s\n", program, log.data());
                GLES.glDeleteShader(default_fs);
                default_fs = 0;
            }
        }
        if (default_fs) {
            LOG_D("Try to attach missing default FS for program %u...", program);
            GLES.glAttachShader(program, default_fs);
        }
    }

    GLES.glLinkProgram(program);
    CHECK_GL_ERROR
}

void glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
    LOG()
    GLES.glGetProgramiv(program, pname, params);
    if (global_settings.ignore_error >= IgnoreErrorLevel::Partial &&
        (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS) && !*params) {
        GLchar infoLog[512];
        GLES.glGetProgramInfoLog(program, 512, nullptr, infoLog);
        LOG_W_FORCE("Program %d linking failed: \n%s", program, infoLog);
        LOG_W_FORCE("Now try to cheat.");
        *params = GL_TRUE;
    }
    CHECK_GL_ERROR
}

void glAttachShader(GLuint program, GLuint shader) {
    LOG()
    LOG_D("glAttachShader(%u, %u)", program, shader)
    if (hardware->emulate_texture_buffer && shader_map_is_sampler_buffer_emulated[shader])
        program_map_is_sampler_buffer_emulated[program] = true;

    GLint type = 0;
    GLES.glGetShaderiv(shader, GL_SHADER_TYPE, &type);
    auto& should_gen_fs_map = program_map_should_generate_fs;
    if (type == GL_FRAGMENT_SHADER) {
        should_gen_fs_map[program] = ShouldGenerateFSState::Never;
    } else if (type == GL_VERTEX_SHADER) {
        auto it = should_gen_fs_map.find(program);
        if (it == should_gen_fs_map.end() || should_gen_fs_map[program] != ShouldGenerateFSState::Never) {
            should_gen_fs_map[program] = ShouldGenerateFSState::Maybe;
        }
    }

    GLES.glAttachShader(program, shader);
    CHECK_GL_ERROR
}

extern UnorderedMap<GLuint, SamplerInfo> g_samplerCacheForSamplerBuffer;

GLuint glCreateProgram() {
    LOG()
    LOG_D("glCreateProgram")
    GLuint program = GLES.glCreateProgram();
    if (hardware->emulate_texture_buffer) {
        program_map_is_sampler_buffer_emulated[program] = false;
        if (g_samplerCacheForSamplerBuffer.find(program) != g_samplerCacheForSamplerBuffer.end()) {
            g_samplerCacheForSamplerBuffer.erase(program);
        }
    }
    program_map_should_generate_fs[program] = ShouldGenerateFSState::Unknown;
    CHECK_GL_ERROR
    return program;
}

void glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length,
                            GLchar* uniformName) {
    LOG()
    LOG_D("glGetActiveUniformName(program: %u, index: %u, bufSize: %d)", program, uniformIndex, bufSize)
    if (length) *length = 0;
    if (bufSize <= 0 || uniformName == nullptr) {
        if (bufSize < 0) GLES.glGetActiveUniform(program, uniformIndex, bufSize, nullptr, nullptr, nullptr, nullptr);
        CHECK_GL_ERROR
        return;
    }
    GLint size = 0;
    GLenum type = 0;
    GLsizei written = 0;
    uniformName[0] = '\0';
    GLES.glGetActiveUniform(program, uniformIndex, bufSize, &written, &size, &type, uniformName);
    if (length) *length = written;
    LOG_D("  -> \"%s\"", uniformName)
    CHECK_GL_ERROR
}
