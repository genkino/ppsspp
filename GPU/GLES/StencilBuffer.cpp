// Copyright (c) 2014- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "gfx_es2/glsl_program.h"
#include "gfx_es2/gl_state.h"
#include "Core/Reporting.h"
#include "GPU/GLES/Framebuffer.h"
#include "GPU/GLES/ShaderManager.h"

static const char *stencil_fs =
#ifdef USING_GLES
"#version 100\n"
"precision highp float;\n"
#endif
"varying vec2 v_texcoord0;\n"
"uniform float u_stencilValue;\n"
"uniform sampler2D tex;\n"
"float roundAndScaleTo255f(in float x) { return floor(x * 255.99); }\n"
"void main() {\n"
"  vec4 index = texture2D(tex, v_texcoord0);\n"
"  gl_FragColor = vec4(u_stencilValue);\n"
"  if (roundAndScaleTo255f(u_stencilValue) != roundAndScaleTo255f(index.a)) discard;\n"
"}\n";

static const char *stencil_vs =
#ifdef USING_GLES
"#version 100\n"
"precision highp float;\n"
#endif
"attribute vec4 a_position;\n"
"attribute vec2 a_texcoord0;\n"
"varying vec2 v_texcoord0;\n"
"void main() {\n"
"  v_texcoord0 = a_texcoord0;\n"
"  gl_Position = a_position;\n"
"}\n";

static bool MaskedEqual(u32 addr1, u32 addr2) {
	return (addr1 & 0x03FFFFFF) == (addr2 & 0x03FFFFFF);
}

bool FramebufferManager::NotifyStencilUpload(u32 addr, int size) {
	if (!MayIntersectFramebuffer(addr)) {
		return false;
	}

	VirtualFramebuffer *dstBuffer = 0;
	for (size_t i = 0; i < vfbs_.size(); ++i) {
		VirtualFramebuffer *vfb = vfbs_[i];
		if (MaskedEqual(vfb->fb_address, addr)) {
			dstBuffer = vfb;
		}
	}
	if (!dstBuffer) {
		return false;
	}

	GLSLProgram *program = 0;
	if (!stencilUploadProgram_) {
		std::string errorString;
		stencilUploadProgram_ = glsl_create_source(stencil_vs, stencil_fs, &errorString);
		if (!stencilUploadProgram_) {
			ERROR_LOG_REPORT(G3D, "Failed to compile stencilUploadProgram! This shouldn't happen.\n%s", errorString.c_str());
		} else {
			glsl_bind(stencilUploadProgram_);
		}

		GLint u_tex = glsl_uniform_loc(stencilUploadProgram_, "tex");
		glUniform1i(u_tex, 0);
	} else {
		glsl_bind(stencilUploadProgram_);
	}

	shaderManager_->DirtyLastShader();

	MakePixelTexture(Memory::GetPointer(addr), dstBuffer->format, dstBuffer->fb_stride, dstBuffer->width, dstBuffer->height);
	DisableState();
	glstate.blend.set(true);
	glstate.blendEquation.set(GL_FUNC_ADD);
	glstate.blendFuncSeparate.set(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
	glstate.stencilTest.enable();
	glstate.stencilOp.set(GL_REPLACE, GL_REPLACE, GL_REPLACE);

	// TODO: Doing it the slow way for now.
	int passes = 0;

	switch (dstBuffer->format) {
	case GE_FORMAT_565:
		// Well, this doesn't make much sense.
		return false;
	case GE_FORMAT_5551:
		passes = 2;
		break;
	case GE_FORMAT_4444:
		passes = 16;
		break;
	case GE_FORMAT_8888:
		passes = 256;
		break;
	}

	if (dstBuffer->fbo) {
		fbo_bind_as_render_target(dstBuffer->fbo);
	}
	glViewport(0, 0, dstBuffer->renderWidth, dstBuffer->renderHeight);

	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);

	const float scale = 1.0f / (passes - 1);
	GLint u_stencilValue = glsl_uniform_loc(stencilUploadProgram_, "u_stencilValue");
	for (int i = 0; i < passes; ++i) {
		glsl_bind(stencilUploadProgram_);
		glUniform1f(u_stencilValue, i * scale);
		if (dstBuffer->format == GE_FORMAT_4444) {
			glstate.stencilFunc.set(GL_ALWAYS, Convert4To8(i), 0xFF);
		} else if (dstBuffer->format == GE_FORMAT_5551) {
			glstate.stencilFunc.set(GL_ALWAYS, i ? 0xFF : 0x00, 0xFF);
		} else if (dstBuffer->format == GE_FORMAT_8888) {
			glstate.stencilFunc.set(GL_ALWAYS, i, 0xFF);
		}
		DrawActiveTexture(0, 0, 0, dstBuffer->width, dstBuffer->height, dstBuffer->width, dstBuffer->height, false, 0.0f, 0.0f, 1.0f, 1.0f, stencilUploadProgram_);
	}

	if (currentRenderVfb_) {
		RebindFramebuffer();
	} else {
		fbo_unbind();
	}
	glstate.viewport.restore();
	return true;
}