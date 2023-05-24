#pragma once

#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#ifdef NDEBUG
#pragma GCC diagnostic ignored "-Wunused-variable"
#if (__clang_major__ > 12) || (!defined(__llvm__) && defined(__GNUC__))
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#endif
