#pragma once

#include <stddef.h>
#include <stdint.h>

/// Basic sandboxing that you always want. Call this immediately upon program start.
void sandbox_level_one();

/// Run at the program start after evaluating command-line arguments. It will limit certain policies that we never need.
/// Initialize the window system integration before calling this.
void sandbox_level_two(size_t connect_port_count = 0, const uint16_t* connect_ports = nullptr);

/// Run right before starting parsing untrusted data, after setting up all internal state and connections.
void sandbox_level_three();
