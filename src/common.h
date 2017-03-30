// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2017 University of Massachusetts, Amherst

#pragma once
#ifndef MESH__COMMON_H
#define MESH__COMMON_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

#include "static/staticlog.h"
#include "utility/ilog2.h"

using std::lock_guard;
using std::mutex;
using std::mt19937_64;
using std::function;

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName &);              \
  void operator=(const TypeName &)

// dynamic (runtime) assert
#ifndef NDEBUG
#define d_assert_msg(expr, fmt, ...) \
  ((likely(expr))                    \
       ? static_cast<void>(0)        \
       : mesh::internal::__mesh_assert_fail(#expr, __FILE__, __PRETTY_FUNCTION__, __LINE__, fmt, __VA_ARGS__))

#define d_assert(expr)                   \
  ((likely(expr)) ? static_cast<void>(0) \
                  : mesh::internal::__mesh_assert_fail(#expr, __FILE__, __PRETTY_FUNCTION__, __LINE__, ""))
#else
#define d_assert_msg(expr, fmt, ...)
#define d_assert(expr)
#endif

namespace mesh {

void debug(const char *fmt, ...);

static constexpr size_t MinObjectSize = 16;

inline constexpr size_t class2Size(const int i) {
  return static_cast<size_t>(1ULL << (i + staticlog(MinObjectSize)));
}

inline int size2Class(const size_t sz) {
  return static_cast<int>(HL::ilog2((sz < 8) ? 8 : sz) - staticlog(MinObjectSize));
}

namespace internal {

void StopTheWorld() noexcept;
void StartTheWorld() noexcept;

inline static mutex *getSeedMutex() {
  static char muBuf[sizeof(mutex)];
  static mutex *mu = new (muBuf) mutex();
  return mu;
}

// we must re-initialize our seed on program startup and after fork.
// Must be called with getSeedMutex() held
inline mt19937_64 *initSeed() {
  static char mtBuf[sizeof(mt19937_64)];

  static_assert(sizeof(mt19937_64::result_type) == sizeof(uint64_t), "expected 64-bit result_type for PRNG");

  // seed this Mersenne Twister PRNG with entropy from the host OS
  std::random_device rd;
  return new (mtBuf) std::mt19937_64(rd());
}

// cryptographically-strong thread-safe PRNG seed
inline uint64_t seed() {
  static mt19937_64 *mt = NULL;

  lock_guard<mutex> lock(*getSeedMutex());

  if (unlikely(mt == nullptr))
    mt = initSeed();

  return (*mt)();
}

// assertions that don't attempt to recursively malloc
void __attribute__((noreturn))
__mesh_assert_fail(const char *assertion, const char *file, const char *func, int line, const char *fmt, ...);
}  // namespace internal
}  // namespace mesh

#endif  // MESH__COMMON_H
