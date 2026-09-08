def can_build(env, platform):
    # The renderer imports the launcher's Vulkan allocation through
    # GL_EXT_memory_object_fd, which only exists on the X11/GL backend.
    return platform == "x11"


def configure(env):
    pass
