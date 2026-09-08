#include "renderer_lifecycle.h"

#include "../ipc/command_sync.h"
#include "../ipc/input_sync.h"
#include "../ipc/zmq_runtime.h"
#include "gl_external_texture.h"

#include "core/os/input.h"
#include "core/os/os.h"
#include "core/print_string.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "servers/visual_server.h"

namespace {

const uint64_t HEARTBEAT_INTERVAL_MSEC = 1000;

// RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM in the Godot 4 launcher. Godot 3
// has no RenderingDevice, so the value is sent as the plain int the launcher's
// RenderResult.set_texture_format matches on.
const int DATA_FORMAT_R8G8B8A8_UNORM = 36;

TGRendererLifecycle *singleton = nullptr;

} // namespace

TGRendererLifecycle *TGRendererLifecycle::get_singleton() {
	return singleton;
}

bool TGRendererLifecycle::engage() {
	print_line("[RENDERER-START]");

	// The launcher allocated the shared image at --resolution; matching the
	// window keeps the per-frame blit 1:1 instead of rescaling.
	shared_size = shared_texture_size();
	OS::get_singleton()->set_window_size(Size2(shared_size.width, shared_size.height));

	command_sync = memnew(CommandSync);
	command_sync->socket_connect();

	Array filehandle_arg;
	filehandle_arg.append(tg_resolve_ipc_address(FILEHANDLE_PATH));
	command_sync->send_command("send_filehandle", filehandle_arg);

	ext_texture = memnew(TGGLExternalTexture);
	print_line("TGGLExternalTexture: waiting for filehandle");
	if (!ext_texture->recv_filehandle()) {
		return false;
	}

	Array format_arg;
	format_arg.append(DATA_FORMAT_R8G8B8A8_UNORM);
	command_sync->send_command("ext_texture_format", format_arg);

	input_sync = memnew(InputSync);
	input_sync->socket_connect();

	VisualServer::get_singleton()->connect("frame_post_draw", this, "_on_frame_post_draw");
	return true;
}

// The launcher allocated the shared image at the size it passed as
// --resolution, which a gate's own window settings can diverge from. Importing
// at any other size would alias the allocation.
Size2i TGRendererLifecycle::shared_texture_size() const {
	const String resolution = tg_cmdline_value("--resolution");
	const Vector<String> parts = resolution.split("x");
	if (parts.size() == 2 && parts[0].is_valid_integer() && parts[1].is_valid_integer()) {
		return Size2i(parts[0].to_int(), parts[1].to_int());
	}

	const Size2 window = OS::get_singleton()->get_window_size();
	return Size2i((int)window.width, (int)window.height);
}

void TGRendererLifecycle::import_shared_texture() {
	if (!ext_texture->import(shared_size.width, shared_size.height)) {
		// Matches the Godot 4 renderer: a failed handshake kills the process so
		// the launcher reports an error instead of showing an empty gate.
		CRASH_NOW_MSG("Shared texture import failed. Exiting child.");
	}
}

void TGRendererLifecycle::forward_mouse_mode() {
	const int mode = (int)Input::get_singleton()->get_mouse_mode();
	if (mode == last_mouse_mode) {
		return;
	}
	last_mouse_mode = mode;

	Array args;
	args.append(mode);
	command_sync->send_command("set_mouse_mode", args);
}

void TGRendererLifecycle::_on_frame_post_draw() {
	if (command_sync == nullptr || ext_texture == nullptr || input_sync == nullptr) {
		return;
	}

	if (!ext_texture->is_imported()) {
		import_shared_texture();
	}

	SceneTree *tree = SceneTree::get_singleton();
	if (tree != nullptr && tree->get_root() != nullptr) {
		const Size2 source = tree->get_root()->get_size();
		const RID viewport_texture = VisualServer::get_singleton()->viewport_get_texture(
				tree->get_root()->get_viewport_rid());
		ext_texture->copy_from_texture(
				VisualServer::get_singleton()->texture_get_texid(viewport_texture),
				(int)source.width, (int)source.height);

		if (shared_size.width > 0 && shared_size.height > 0) {
			input_sync->set_input_scale(Vector2(source.width / (float)shared_size.width,
					source.height / (float)shared_size.height));
		}
	}

	frames_seen++;
	if (!first_frame_sent && frames_seen > 2) {
		command_sync->send_command("first_frame");
		first_frame_sent = true;
	}

	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	if (first_frame_sent && now - last_heartbeat_ms > HEARTBEAT_INTERVAL_MSEC) {
		command_sync->send_command("heartbeat");
		last_heartbeat_ms = now;
	}

	input_sync->receive_input_events();
	forward_mouse_mode();

	command_sync->poll_monitor();
	if (!command_sync->is_peer_connected()) {
		CRASH_NOW_MSG("CommandSync peer disconnected. Exiting child.");
	}
}

void TGRendererLifecycle::teardown() {
	if (VisualServer::get_singleton() != nullptr &&
			VisualServer::get_singleton()->is_connected("frame_post_draw", this, "_on_frame_post_draw")) {
		VisualServer::get_singleton()->disconnect("frame_post_draw", this, "_on_frame_post_draw");
	}

	if (ext_texture != nullptr) {
		memdelete(ext_texture);
		ext_texture = nullptr;
	}
	if (input_sync != nullptr) {
		input_sync->close();
		memdelete(input_sync);
		input_sync = nullptr;
	}
	if (command_sync != nullptr) {
		command_sync->close();
		memdelete(command_sync);
		command_sync = nullptr;
	}
}

void TGRendererLifecycle::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_frame_post_draw"), &TGRendererLifecycle::_on_frame_post_draw);
}

TGRendererLifecycle::TGRendererLifecycle() {
	singleton = this;
}

TGRendererLifecycle::~TGRendererLifecycle() {
	teardown();
	singleton = nullptr;
}

bool tg_renderer_is_enabled() {
	return !tg_cmdline_value("--tg-ipc-dir").empty();
}

void tg_renderer_engage() {
	if (TGRendererLifecycle::get_singleton() != nullptr) {
		return;
	}

	TGRendererLifecycle *lifecycle = memnew(TGRendererLifecycle);
	if (!lifecycle->engage()) {
		CRASH_NOW_MSG("Renderer handshake with the launcher failed.");
	}
	print_line("[RENDERER-READY]");
}

void tg_renderer_teardown() {
	TGRendererLifecycle *lifecycle = TGRendererLifecycle::get_singleton();
	if (lifecycle != nullptr) {
		memdelete(lifecycle);
	}
	tg_zmq_shutdown();
}
