#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MALLOC_CAP_INTERNAL = 1 };
bool heap_caps_check_integrity_all(bool print_errors);
size_t heap_caps_get_free_size(uint32_t capabilities);
size_t heap_caps_get_largest_free_block(uint32_t capabilities);
