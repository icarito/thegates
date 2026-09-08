#ifndef TG_INPUT_SYNC_H
#define TG_INPUT_SYNC_H

#include "core/math/vector2.h"
#include "core/ustring.h"

#include "zmq.hpp"

static const String INPUT_SYNC_ADDRESS("ipc:///tmp/input_sync");

// Renderer half of the input channel: the launcher binds and sends Godot 4
// InputEvents as text Variants, we connect and replay them into this engine's
// input queue through input_event_compat.
class InputSync {
	zmq::socket_t sock;
	Vector2 input_scale = Vector2(1, 1);

public:
	void socket_connect(const String &p_address = INPUT_SYNC_ADDRESS);
	// The launcher sends positions in shared-texture space; a window manager
	// that resizes the renderer's window makes the viewport a different size.
	void set_input_scale(const Vector2 &p_scale) { input_scale = p_scale; }
	void receive_input_events();
	void close();

	InputSync();
	~InputSync();
};

#endif // TG_INPUT_SYNC_H
