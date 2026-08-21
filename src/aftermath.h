#pragma once

void* aftermath_initialize(const char* trace_filename);
void aftermath_handle_device_lost(void* context);
void aftermath_shutdown(void* context);
