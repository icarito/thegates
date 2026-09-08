#ifndef TG_GL_EXTERNAL_TEXTURE_H
#define TG_GL_EXTERNAL_TEXTURE_H

#include "core/ustring.h"

static const String FILEHANDLE_PATH("/tmp/external_texture");

// The launcher allocates the shared image with Vulkan and exports it as an
// opaque POSIX fd. Godot 3 has no RenderingDevice, so this engine adopts the
// same allocation through GL_EXT_memory_object_fd and blits the root
// viewport into it once per frame.
//
// glImportMemoryFdEXT needs the exact VkMemoryAllocateInfo::allocationSize.
// Mesa exports the memory as a dma-buf, whose llseek reports that size, so
// the launcher does not have to send it.
class TGGLExternalTexture {
	int filehandle = -1;
	uint64_t alloc_size = 0;

	unsigned int memory_object = 0;
	unsigned int texture = 0;
	unsigned int read_fbo = 0;
	unsigned int draw_fbo = 0;

	int width = 0;
	int height = 0;
	bool imported = false;

public:
	bool recv_filehandle(const String &p_path = FILEHANDLE_PATH);

	// Must run on the thread holding the GL context.
	bool import(int p_width, int p_height);
	bool copy_from_texture(unsigned int p_src_texture, int p_src_width, int p_src_height);

	bool is_imported() const { return imported; }
	void close();

	TGGLExternalTexture();
	~TGGLExternalTexture();
};

#endif // TG_GL_EXTERNAL_TEXTURE_H
