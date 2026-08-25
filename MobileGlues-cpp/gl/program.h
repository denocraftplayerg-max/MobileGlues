// MobileGlues - gl/program.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_PROGRAM_H
#define MOBILEGLUES_PROGRAM_H

#include <GL/gl.h>

#ifdef __cplusplus
extern "C"
{
#endif

    GLAPI GLAPIENTRY void glBindFragDataLocation(GLuint program, GLuint color, const GLchar* name);
    GLAPI GLAPIENTRY void glLinkProgram(GLuint program);
    GLAPI GLAPIENTRY void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
    GLAPI GLAPIENTRY void glUseProgram(GLuint program);
    GLAPI GLAPIENTRY GLuint glCreateProgram();
    GLAPI GLAPIENTRY void glAttachShader(GLuint program, GLuint shader);
    GLAPI GLAPIENTRY GLuint glCreateShader(GLenum shaderType);
    GLAPI GLAPIENTRY void glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length,
                                                 GLchar* uniformName);

    // GL_EXT_separate_shader_objects - Program Pipeline support
    GLAPI GLAPIENTRY void glGenProgramPipelines(GLsizei n, GLuint* pipelines);
    GLAPI GLAPIENTRY void glDeleteProgramPipelines(GLsizei n, const GLuint* pipelines);
    GLAPI GLAPIENTRY void glBindProgramPipeline(GLuint pipeline);
    GLAPI GLAPIENTRY void glUseProgramStages(GLuint pipeline, GLbitfield stages, GLuint program);
    GLAPI GLAPIENTRY GLboolean glIsProgramPipeline(GLuint pipeline);
    GLAPI GLAPIENTRY void glGetProgramPipelineiv(GLuint pipeline, GLenum pname, GLint* params);
    GLAPI GLAPIENTRY GLuint glCreateShaderProgramv(GLenum type, GLsizei count, const GLchar* const* strings);
    
    // Pipeline cache stats
    GLAPI GLAPIENTRY void glGetPipelineCacheStatsEXT(uint64_t* hits, uint64_t* misses);
    GLAPI GLAPIENTRY void glResetPipelineCacheStatsEXT();

#ifdef __cplusplus
}
#endif

#endif // MOBILEGLUES_PROGRAM_H
