#ifndef TG_COMMAND_H
#define TG_COMMAND_H

#include "core/array.h"
#include "core/reference.h"

class Command : public Reference {
	GDCLASS(Command, Reference);

	String name;
	Array args;

protected:
	static void _bind_methods();

public:
	void set_name(const String &p_name) { name = p_name; }
	String get_name() { return name; }

	void set_args(const Array &p_args) { args = p_args; }
	Array get_args() { return args; }

	Command();
	~Command();
};

#endif // TG_COMMAND_H
