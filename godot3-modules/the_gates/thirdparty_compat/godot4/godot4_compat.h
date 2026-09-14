#pragma once

// Force-included into the broker sources the Godot 3 renderer compiles
// straight out of godot/modules/the_gates/network, so the wire protocol,
// framing and CIDR policy keep a single source. Maps the Godot 4 names those
// files use onto their Godot 3 equivalents; the include paths live in the
// sibling core/ directories.
#include "core/io/ip_address.h"
#include "core/pool_vector.h"
#include "core/variant.h"

using IPAddress = IP_Address;
using PackedStringArray = PoolStringArray;
