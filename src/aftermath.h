#pragma once

void* aftermath_initialize(const char* trace_filename);
void aftermath_handle_device_lost(void* context);
void aftermath_register_marker(void* context, const void* marker, const char* label);
void aftermath_shutdown(void* context);
