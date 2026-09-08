#ifndef TG_ZMQ_RUNTIME_H
#define TG_ZMQ_RUNTIME_H

#include "core/ustring.h"

namespace zmq {
class context_t;
}

zmq::context_t &tg_zmq_context();
void tg_zmq_shutdown();

// Value of the `--tg-ipc-dir <abs path>` launcher argument. Godot 4's fork
// stores this in a main.cpp global; out of tree it is parsed from argv.
String tg_cmdline_value(const String &p_flag);

String tg_resolve_ipc_address(const String &p_address);

#endif // TG_ZMQ_RUNTIME_H
