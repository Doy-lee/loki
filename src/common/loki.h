// Copyright (c) 2018, The Loki Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef LOKI_H
#define LOKI_H

#define LOKI_HOUR(val) ((val) * LOKI_MINUTES(60))
#define LOKI_MINUTES(val) val * 60

#define TRACY_ENABLE
#include "tracy/Tracy.hpp"

// NOTE: Custom helper macro that Tracy doesn't have. Don't want to have to come
// up with a variable name for each tracing zone. Just generate one for me
// please.
#if defined(TRACY_ENABLE)
  #if defined TRACY_HAS_CALLSTACK && defined TRACY_CALLSTACK
    #define ZoneUniqueNamedNC(name, color, active) static const tracy::SourceLocationData TracyConcat(__tracy_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, color }; tracy::ScopedZone TracyConcat(unused_, __LINE__)( &TracyConcat(__tracy_source_location,__LINE__), TRACY_CALLSTACK, active );
  #else
    #define ZoneUniqueNamedNC(name, color, active) static const tracy::SourceLocationData TracyConcat(__tracy_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, color }; tracy::ScopedZone TracyConcat(unused_, __LINE__)( &TracyConcat(__tracy_source_location,__LINE__), active );
  #endif
#else
  #define ZoneUniqueNamedNC(name, color, active)
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <iterator>
#include <cassert>

#define LOKI_RPC_DOC_INTROSPECT

namespace loki
{
double      round           (double);
double      exp2            (double);

constexpr uint64_t clamp_u64(uint64_t val, uint64_t min, uint64_t max)
{
  assert(min <= max);
  if (val < min) val = min;
  else if (val > max) val = max;
  return val;
}

template <typename lambda_t>
struct deferred
{
private:
  lambda_t lambda;
  bool cancelled = false;
public:
  deferred(lambda_t lambda) : lambda(lambda) {}
  void invoke() { lambda(); cancelled = true; } // Invoke early instead of at destruction
  void cancel() { cancelled = true; } // Cancel invocation at destruction
  ~deferred() { if (!cancelled) lambda(); }
};

template <typename lambda_t>
#ifdef __GNUG__
[[gnu::warn_unused_result]]
#endif
deferred<lambda_t> defer(lambda_t lambda) { return lambda; }

struct defer_helper
{
  template <typename lambda_t>
  deferred<lambda_t> operator+(lambda_t lambda) { return lambda; }
};

uint32_t constexpr MATERIAL_BLUE_600        = 0x1E88E5;
uint32_t constexpr MATERIAL_DEEP_PURPLE_600 = 0x5E35B1;
uint32_t constexpr MATERIAL_PURPLE_600      = 0x8E24AA;
uint32_t constexpr MATERIAL_LIGHT_BLUE_600  = 0x039BE5;

uint32_t constexpr MATERIAL_RED_600         = 0xE53935;
uint32_t constexpr MATERIAL_PINK_600        = 0xD81B60;
uint32_t constexpr MATERIAL_DEEP_ORANGE_600 = 0xF4511E;
uint32_t constexpr MATERIAL_ORANGE_600      = 0xFB8C00;
uint32_t constexpr MATERIAL_LIGHT_GREEN_600 = 0x7CB342;
uint32_t constexpr MATERIAL_LIME_600        = 0xC0CA33;
uint32_t constexpr MATERIAL_TEAL_600        = 0x00897B;

uint32_t constexpr TRACE_SERVICE_NODE_LIST_COLOR       = MATERIAL_BLUE_600;
uint32_t constexpr TRACE_SERVICE_NODE_QUORUM_COP_COLOR = MATERIAL_DEEP_PURPLE_600;
uint32_t constexpr TRACE_SERVICE_NODE_VOTING_COLOR     = MATERIAL_PURPLE_600;
uint32_t constexpr TRACE_SERVICE_NODE_LIST_SWARM_COLOR = MATERIAL_LIGHT_BLUE_600;

uint32_t constexpr TRACE_LOKI_NAME_SYSTEM              = MATERIAL_TEAL_600;

uint32_t constexpr TRACE_CRYPTONOTE_COLOR              = MATERIAL_RED_600;
uint32_t constexpr TRACE_BLOCKCHAIN_COLOR              = MATERIAL_PINK_600;
uint32_t constexpr TRACE_CRYPTONOTE_PROTOCOL_COLOR     = MATERIAL_DEEP_ORANGE_600;
uint32_t constexpr TRACE_TXPOOL_COLOR                  = MATERIAL_ORANGE_600;
uint32_t constexpr TRACE_DB_LMDB_COLOR                 = MATERIAL_LIGHT_GREEN_600;
uint32_t constexpr TRACE_BLOCKCHAIN_DB_COLOR           = MATERIAL_LIME_600;

#define LOKI_TOKEN_COMBINE2(x, y) x ## y
#define LOKI_TOKEN_COMBINE(x, y) LOKI_TOKEN_COMBINE2(x, y)
#define LOKI_DEFER auto const LOKI_TOKEN_COMBINE(loki_defer_, __LINE__) = loki::defer_helper() + [&]()

template <typename T, size_t N>
constexpr size_t array_count(T (&)[N]) { return N; }

template <typename T, size_t N>
constexpr size_t char_count(T (&)[N]) { return N - 1; }

}; // namespace Loki

#endif // LOKI_H
