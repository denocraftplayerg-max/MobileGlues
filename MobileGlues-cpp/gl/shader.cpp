// MobileGlues - gl/shader.cpp
// Copyright (c) 2025 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1
// SPDX-License-Identifier: LGPL-2.1-only

#include <cctype>
#include "shader.h"
#include <GL/gl.h>
#include "log.h"
#include "program.h"
#include "../gles/loader.h"
#include "../includes.h"
#include "glsl/glsl_for_es.h"
#include "../config/settings.h"
#include "FSR1/FSR1.h"

#define DEBUG 0

struct shader_t shaderInfo;
UnorderedMap<GLuint, bool> shader_map_is_sampler_buffer_emulated;

// ============================================================================
// GL_EXT_shader_framebuffer_fetch - DETECCAO
// ============================================================================
static bool g_framebuffer_fetch_resolved = false;
static bool g_framebuffer_fetch_available = false;

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

static bool mg_framebuffer_fetch_available() {
    if (g_framebuffer_fetch_resolved) return g_framebuffer_fetch_available;
    g_framebuffer_fetch_resolved = true;
    g_framebuffer_fetch_available = mg_gles_has_extension("GL_EXT_shader_framebuffer_fetch") ||
                                     mg_gles_has_extension("GL_EXT_shader_framebuffer_fetch_non_coherent");
    if (g_framebuffer_fetch_available) {
        LOG_D("GL_EXT_shader_framebuffer_fetch disponivel");
    }
    return g_framebuffer_fetch_available;
}

// ============================================================================
// INJECAO DE SHADER FRAMEBUFFER FETCH (sem std::regex)
// Substitui 'out vec4' por 'inout vec4' para location 0 em fragment shaders
// ============================================================================

static std::string mg_inject_framebuffer_fetch(const std::string& source, GLenum shaderType) {
    if (!mg_framebuffer_fetch_available()) return source;
    if (shaderType != GL_FRAGMENT_SHADER) return source;

    // Verificar se ja tem inout
    if (source.find("inout vec4") != std::string::npos ||
        source.find("inout mediump vec4") != std::string::npos) {
        return source;
    }

    std::string result = source;

    // Adicionar extensao apos #version
    size_t version_end = result.find('\n');
    if (version_end != std::string::npos &&
        result.find("#extension GL_EXT_shader_framebuffer_fetch") == std::string::npos) {
        result.insert(version_end + 1, "#extension GL_EXT_shader_framebuffer_fetch : require\n");
    }

    // Substituir 'layout(location = 0) out vec4' por 'layout(location = 0) inout vec4'
    // Busca simples sem regex para compatibilidade com NDK
    const char* search = "layout(location = 0) out vec4";
    const char* replace = "layout(location = 0) inout vec4";
    size_t pos = result.find(search);
    if (pos != std::string::npos) {
        result.replace(pos, strlen(search), replace);
    }

    // Tentar outras variacoes comuns
    const char* search2 = "layout(location=0) out vec4";
    pos = result.find(search2);
    if (pos != std::string::npos) {
        result.replace(pos, strlen(search2), "layout(location=0) inout vec4");
    }

    const char* search3 = "out vec4 fragColor";
    pos = result.find(search3);
    if (pos != std::string::npos) {
        result.replace(pos, strlen(search3), "inout vec4 fragColor");
    }

    const char* search4 = "out vec4 gl_FragColor";
    pos = result.find(search4);
    if (pos != std::string::npos) {
        result.replace(pos, strlen(search4), "inout vec4 gl_FragColor");
    }

    return result;
}

// ============================================================================
// FUNCOES ORIGINAIS
// ============================================================================

bool can_run_essl3(unsigned int esversion, const char* glsl) {
    if (strncmp(glsl, "#version 100", 12) == 0) return true;
    unsigned int glsl_version = 0;
    if (strncmp(glsl, "#version 300 es", 15) == 0) glsl_version = 300;
    else if (strncmp(glsl, "#version 310 es", 15) == 0) glsl_version = 310;
    else if (strncmp(glsl, "#version 320 es", 15) == 0) glsl_version = 320;
    else return false;
    return esversion >= glsl_version;
}

bool is_direct_shader(const char* glsl) {
    return can_run_essl3(hardware->es_version, glsl);
}

bool check_if_sampler_buffer_used(std::string str) {
    return str.find("samplerBuffer") != std::string::npos;
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
    LOG()
    shaderInfo.id = 0;
    shaderInfo.converted = "";
    shaderInfo.frag_data_changed_converted.clear();
    shaderInfo.frag_data_changed = 0;
    size_t l = 0;
    for (int i = 0; i < count; i++)
        l += (length && length[i] >= 0) ? length[i] : strlen(string[i]);
    std::string glsl_src, essl_src;
    glsl_src.reserve(l + 1);
    if (length) {
        for (int i = 0; i < count; i++) {
            if (length[i] >= 0) glsl_src += std::string_view(string[i], length[i]);
            else glsl_src += string[i];
        }
    } else {
        for (int i = 0; i < count; i++) glsl_src += string[i];
    }

    bool is_sampler_buffer_emulated = hardware->emulate_texture_buffer && check_if_sampler_buffer_used(glsl_src);

    if (is_direct_shader(glsl_src.c_str())) {
        LOG_D("[INFO] [Shader] Direct shader source: ")
        LOG_D("%s", glsl_src.c_str())
        essl_src = glsl_src;
    } else {
        int glsl_version = getGLSLVersion(glsl_src.c_str());
        LOG_D("[INFO] [Shader] Shader source: ")
        LOG_D("%s", glsl_src.c_str())
        GLint shaderType;
        GLES.glGetShaderiv(shader, GL_SHADER_TYPE, &shaderType);
        int return_code = 0;
        essl_src = GLSLtoGLSLES(glsl_src.c_str(), shaderType, hardware->es_version, glsl_version, return_code);

        if (essl_src.empty()) {
            LOG_E("Failed to convert shader %d.", shader)
            return;
        }
        LOG_D("\n[INFO] [Shader] Converted Shader source: \n%s", essl_src.c_str())
    }

    // INJETAR FRAMEBUFFER FETCH se disponivel
    GLint shaderType = 0;
    GLES.glGetShaderiv(shader, GL_SHADER_TYPE, &shaderType);
    essl_src = mg_inject_framebuffer_fetch(essl_src, shaderType);

    if (!essl_src.empty()) {
        shaderInfo.id = shader;
        shaderInfo.converted = essl_src;
        const char* s[] = {essl_src.c_str()};
        GLES.glShaderSource(shader, count, s, nullptr);
        if (hardware->emulate_texture_buffer)
            shader_map_is_sampler_buffer_emulated[shader] = is_sampler_buffer_emulated;
    } else
        LOG_E("Failed to convert glsl.")
    CHECK_GL_ERROR
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
    LOG()
    GLES.glGetShaderiv(shader, pname, params);
    if (global_settings.ignore_error >= IgnoreErrorLevel::Partial && pname == GL_COMPILE_STATUS && !*params) {
        GLchar infoLog[512];
        GLES.glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        LOG_W_FORCE("Shader %d compilation failed: \n%s", shader, infoLog)
        LOG_W_FORCE("Now try to cheat.")
        *params = GL_TRUE;
    }
    CHECK_GL_ERROR
}

GLuint glCreateShader(GLenum shaderType) {
    if (global_settings.fsr1_setting != FSR1_Quality_Preset::Disabled && !fsrInitialized) {
        InitFSRResources();
    }
    LOG()
    LOG_D("glCreateShader(%s)", glEnumToString(shaderType))
    GLuint shader = GLES.glCreateShader(shaderType);
    if (shader != 0 && hardware->emulate_texture_buffer) shader_map_is_sampler_buffer_emulated[shader] = false;
    CHECK_GL_ERROR
    return shader;
}
