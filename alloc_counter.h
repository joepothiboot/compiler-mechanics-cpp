// alloc_counter.h — global operator new replacement that counts heap
// allocations, so tests can assert "zero allocations" instead of claiming it.
// Replacement operator new must be defined exactly once per program: include
// this from a single .cpp (every study file here is its own executable).
#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>

static std::size_t g_allocations = 0;

void *operator new(std::size_t n) {
  ++g_allocations;
  if (void *p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
