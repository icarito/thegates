#ifndef TG_GL_EXTERNAL_TEXTURE_H
#define TG_GL_EXTERNAL_TEXTURE_H

#include "core/ustring.h"

#ifdef OSX_ENABLED
static const String FILEHANDLE_PATH("ipc:///tmp/external_texture");
#else
static const String FILEHANDLE_PATH("/tmp/external_texture");
#endif

// The launcher allocates the shared image with Vulkan and exports it: an
// opaque POSIX fd on Linux, an IOSurface on macOS (MoltenVK). Godot 3 has no
// RenderingDevice, so this engine adopts the same image through the platform's
// desktop GL interop and blits the root viewport into it once per frame.
//
// On Linux, glImportMemoryFdEXT needs the exact
// VkMemoryAllocateInfo::allocationSize. Mesa exports the memory as a dma-buf,
// whose llseek reports that size, so the launcher does not have to send it.
class TGGLExternalTexture {
#ifdef OSX_ENABLED
	void *surface = nullptr;
	bool bgra = false;
#else
	int filehandle = -1;
	uint64_t alloc_size = 0;
	unsigned int memory_object = 0;
#endif

	unsigned int texture_target = 0;
	unsigned int texture = 0;
	unsigned int read_fbo = 0;
	unsigned int draw_fbo = 0;

	int width = 0;
	int height = 0;
	bool imported = false;

	bool import_platform_texture();

public:
	bool recv_filehandle(const String &p_path = FILEHANDLE_PATH);

	// Must run on the thread holding the GL context.
	bool import(int p_width, int p_height);
	bool copy_from_texture(unsigned int p_src_texture, int p_src_width, int p_src_height);

	// Whether the shared image stores its channels as BGRA.
#ifdef OSX_ENABLED
	bool is_bgra() const { return bgra; }
#else
	bool is_bgra() const { return false; }
#endif
	bool is_imported() const { return imported; }
	void close();

	TGGLExternalTexture();
	~TGGLExternalTexture();
};

#endif // TG_GL_EXTERNAL_TEXTURE_H
