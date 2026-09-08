#include "gl_external_texture.h"

#include "../ipc/zmq_runtime.h"
#include "core/error_macros.h"
#include "core/print_string.h"
#include "core/variant.h"

#include "flingfd.h"
#include "zmq.hpp"

#include <unistd.h>

#include GLES3_INCLUDE_H

// Declared rather than pulling <GL/glx.h>, which redefines the GL types glad
// already provides.
extern "C" void *glXGetProcAddressARB(const unsigned char *procName);

namespace {

// GL_EXT_memory_object / GL_EXT_memory_object_fd. Not in Godot 3's glad.
const GLenum GL_HANDLE_TYPE_OPAQUE_FD = 0x9586;
const GLenum GL_DEDICATED_MEMORY_OBJECT = 0x9581;

typedef void (*CreateMemoryObjectsFunc)(GLsizei, GLuint *);
typedef void (*MemoryObjectParameterivFunc)(GLuint, GLenum, const GLint *);
typedef void (*ImportMemoryFdFunc)(GLuint, GLuint64, GLenum, GLint);
typedef void (*TexStorageMem2DFunc)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
typedef void (*DeleteMemoryObjectsFunc)(GLsizei, const GLuint *);

CreateMemoryObjectsFunc gl_create_memory_objects = nullptr;
MemoryObjectParameterivFunc gl_memory_object_parameteriv = nullptr;
ImportMemoryFdFunc gl_import_memory_fd = nullptr;
TexStorageMem2DFunc gl_tex_storage_mem_2d = nullptr;
DeleteMemoryObjectsFunc gl_delete_memory_objects = nullptr;

void *load_gl_proc(const char *p_name) {
	return glXGetProcAddressARB(reinterpret_cast<const unsigned char *>(p_name));
}

bool load_memory_object_extension() {
	if (gl_tex_storage_mem_2d != nullptr) {
		return true;
	}

	gl_create_memory_objects = (CreateMemoryObjectsFunc)load_gl_proc("glCreateMemoryObjectsEXT");
	gl_memory_object_parameteriv = (MemoryObjectParameterivFunc)load_gl_proc("glMemoryObjectParameterivEXT");
	gl_import_memory_fd = (ImportMemoryFdFunc)load_gl_proc("glImportMemoryFdEXT");
	gl_tex_storage_mem_2d = (TexStorageMem2DFunc)load_gl_proc("glTexStorageMem2DEXT");
	gl_delete_memory_objects = (DeleteMemoryObjectsFunc)load_gl_proc("glDeleteMemoryObjectsEXT");

	return gl_create_memory_objects != nullptr && gl_memory_object_parameteriv != nullptr &&
			gl_import_memory_fd != nullptr && gl_tex_storage_mem_2d != nullptr &&
			gl_delete_memory_objects != nullptr;
}

} // namespace

bool TGGLExternalTexture::recv_filehandle(const String &p_path) {
	filehandle = flingfd_simple_recv(p_path.utf8().get_data()); // WARNING: BLOCKING COMMAND
	ERR_FAIL_COND_V_MSG(filehandle < 0, false, "Receive filehandle failed");

	// Vulkan hands out its external memory as a dma-buf, and dma-buf implements
	// llseek precisely so importers can size it.
	const off_t probed = lseek(filehandle, 0, SEEK_END);
	lseek(filehandle, 0, SEEK_SET);
	ERR_FAIL_COND_V_MSG(probed <= 0, false,
			"Cannot size the shared allocation: the launcher's exported fd does not report a length");

	alloc_size = (uint64_t)probed;
	return true;
}

bool TGGLExternalTexture::import(int p_width, int p_height) {
	ERR_FAIL_COND_V_MSG(filehandle < 0, false, "Receive filehandle first");
	ERR_FAIL_COND_V_MSG(alloc_size == 0, false, "Receive allocation size first");
	ERR_FAIL_COND_V_MSG(!load_memory_object_extension(), false,
			"GL_EXT_memory_object_fd is unavailable; this GPU driver cannot import the launcher's Vulkan allocation");

	width = p_width;
	height = p_height;

	gl_create_memory_objects(1, &memory_object);

	// The launcher allocates with VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
	// so the fd backs one whole VkDeviceMemory and GL must treat it the same.
	const GLint dedicated = GL_TRUE;
	gl_memory_object_parameteriv(memory_object, GL_DEDICATED_MEMORY_OBJECT, &dedicated);

	// glImportMemoryFdEXT takes ownership of the fd.
	gl_import_memory_fd(memory_object, (GLuint64)alloc_size, GL_HANDLE_TYPE_OPAQUE_FD, (GLint)filehandle);
	filehandle = -1;

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	gl_tex_storage_mem_2d(GL_TEXTURE_2D, 1, GL_RGBA8, width, height, memory_object, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	const GLenum err = glGetError();
	ERR_FAIL_COND_V_MSG(err != GL_NO_ERROR, false,
			"Importing the shared allocation failed with GL error " + itos(err));

	glGenFramebuffers(1, &read_fbo);
	glGenFramebuffers(1, &draw_fbo);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	ERR_FAIL_COND_V_MSG(status != GL_FRAMEBUFFER_COMPLETE, false,
			"Shared texture is not framebuffer-complete, status " + itos(status));

	imported = true;
	print_line(vformat("[EXT-TEXTURE] imported %dx%d from %d bytes of shared Vulkan memory",
			width, height, (int64_t)alloc_size));
	return true;
}

bool TGGLExternalTexture::copy_from_texture(unsigned int p_src_texture, int p_src_width, int p_src_height) {
	if (!imported || p_src_texture == 0 || p_src_width <= 0 || p_src_height <= 0) {
		return false;
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p_src_texture, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);

	// Source rect is flipped in Y: GL's framebuffer origin is bottom-left and
	// the launcher samples the shared image with Vulkan's top-left origin.
	glBlitFramebuffer(0, p_src_height, p_src_width, 0, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	return true;
}

void TGGLExternalTexture::close() {
	if (read_fbo != 0) {
		glDeleteFramebuffers(1, &read_fbo);
		read_fbo = 0;
	}
	if (draw_fbo != 0) {
		glDeleteFramebuffers(1, &draw_fbo);
		draw_fbo = 0;
	}
	if (texture != 0) {
		glDeleteTextures(1, &texture);
		texture = 0;
	}
	if (memory_object != 0 && gl_delete_memory_objects != nullptr) {
		gl_delete_memory_objects(1, &memory_object);
		memory_object = 0;
	}
	imported = false;
}

TGGLExternalTexture::TGGLExternalTexture() {
}

TGGLExternalTexture::~TGGLExternalTexture() {
	close();
}
