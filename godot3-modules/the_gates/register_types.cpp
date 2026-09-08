#include "register_types.h"

#include "core/class_db.h"
#include "ipc/command.h"
#include "renderer/renderer_lifecycle.h"

void register_the_gates_types() {
	ClassDB::register_class<Command>();
	ClassDB::register_class<TGRendererLifecycle>();

	// Without the launcher's IPC directory this binary is a plain Godot 3
	// engine, so the handshake — which blocks on the shared texture — is
	// skipped rather than hanging.
	if (tg_renderer_is_enabled()) {
		tg_renderer_engage();
	}
}

void unregister_the_gates_types() {
	tg_renderer_teardown();
}
