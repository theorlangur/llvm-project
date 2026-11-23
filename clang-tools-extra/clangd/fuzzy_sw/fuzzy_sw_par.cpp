//===--- fuzzy_sw_par.cpp - SIMD-accelerated Smith-Waterman fuzzy matcher -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "fuzzy_sw.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE4_1__)
#include <smmintrin.h>
#endif

namespace fuzzy_sw {

template <class T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

namespace {

template <class T> struct Span {
  T *Begin = nullptr;
  T *End = nullptr;

  Span() = default;
  Span(T *B, T *E) : Begin(B), End(E) {}
  Span(T *B, size_t L) : Begin(B), End(B + L) {}

  auto begin() { return Begin; }
  auto end() { return End; }
  auto begin() const { return Begin; }
  auto end() const { return End; }

  size_t size() const { return static_cast<size_t>(End - Begin); }

  T &operator[](size_t i) { return Begin[i]; }
  const T &operator[](size_t i) const { return Begin[i]; }
};

struct jthread {
  jthread() = default;
  template <class... T>
  jthread(T &&...args) : T_(std::forward<T>(args)...) {}
  jthread(jthread &&rhs) noexcept : T_(std::move(rhs.T_)) {}
  jthread(const jthread &) = delete;
  ~jthread() {
    if (T_.joinable())
      T_.join();
  }
  std::thread T_;
};

struct barrier {
  explicit barrier(size_t gSize = 0) : Counter(gSize), GroupSize(gSize) {}

  size_t Counter = 0;
  size_t GroupSize = 0;
  size_t Epoch = 0;
  std::mutex Mutex;
  std::condition_variable Cond;

  explicit operator bool() const { return GroupSize > 0; }

  void change_group_size(size_t s) { Counter = GroupSize = s; }

  void arrive_and_wait() {
    std::unique_lock<std::mutex> G(Mutex);
    if (--Counter == 0) {
      ++Epoch;
      Counter = GroupSize;
      Cond.notify_all();
    } else {
      auto E = Epoch;
      Cond.wait(G, [&] { return Epoch > E; });
    }
  }
};

constexpr uint8_t tolower_u8(uint8_t c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + 32) : c;
}

constexpr size_t GetCeilPow2(size_t v) {
  if (v == 0 || (v & (v - 1)) == 0)
    return v == 0 ? 1 : v;
  size_t res = 1;
  while (res < v)
    res <<= 1;
  return res;
}

constexpr size_t GetLog2(size_t v) {
  size_t res = 0;
  while (v > 1) {
    v >>= 1;
    ++res;
  }
  return res;
}

bool isDelimiter(char c) {
  switch (c) {
  case ' ':
  case '_':
  case '.':
  case ':':
  case '-':
  case '=':
  case ',':
    return true;
  default:
    return false;
  }
}

template <typename IntType, size_t W> struct simd_prims;

template <typename IntType, size_t W, typename simd_t>
struct simd_prims_base {
  static constexpr size_t Width = W;
  using int_type_t = IntType;
  using simd_base_t = simd_t;

  using input_t = std::array<std::string_view, W>;
  using input_span_t = Span<std::string_view>;
  using input_char_src_t = std::array<CharSource *, W>;
  using input_char_src_span_t = Span<CharSource *>;
  using scores_t = std::array<IntType, W>;
  using lut_data_t = int8_t;
  static constexpr IntType kMaskVal = static_cast<IntType>(-1);

  static void set_idx(size_t idx, int_type_t v, simd_base_t &t) {
    int_type_t *p = reinterpret_cast<int_type_t *>(&t);
    p[idx] = v;
  }

  static scores_t unpack(simd_base_t o) {
    scores_t s;
    const int_type_t *pB = reinterpret_cast<const int_type_t *>(&o);
    for (size_t i = 0; i < Width; ++i)
      s[i] = pB[i];
    return s;
  }
};

#if defined(__AVX2__)

template <> struct simd_prims<int8_t, 32> : simd_prims_base<int8_t, 32, __m256i> {
  static auto set1(int_type_t v) { return _mm256_set1_epi8(v); }
  static auto sub(simd_base_t o1, simd_base_t o2) {
    return _mm256_sub_epi8(o1, o2);
  }
  static auto add(simd_base_t o1, simd_base_t o2) {
    return _mm256_add_epi8(o1, o2);
  }
  static auto max(simd_base_t o1, simd_base_t o2) {
    return _mm256_max_epi8(o1, o2);
  }
  static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) {
    return _mm256_blendv_epi8(o1, o2, m);
  }
  static auto eq(simd_base_t o1, simd_base_t o2) {
    return _mm256_cmpeq_epi8(o1, o2);
  }
  static auto _and(simd_base_t o1, simd_base_t o2) {
    return _mm256_and_si256(o1, o2);
  }
  static auto _or(simd_base_t o1, simd_base_t o2) {
    return _mm256_or_si256(o1, o2);
  }
  static auto _xor(simd_base_t o1, simd_base_t o2) {
    return _mm256_xor_si256(o1, o2);
  }
  static auto load(int_type_t const (&src)[Width]) {
    return _mm256_lddqu_si256(reinterpret_cast<const simd_base_t *>(src));
  }
};

template <>
struct simd_prims<int16_t, 16> : simd_prims_base<int16_t, 16, __m256i> {
  static auto set1(int_type_t v) { return _mm256_set1_epi16(v); }
  static auto sub(simd_base_t o1, simd_base_t o2) {
    return _mm256_sub_epi16(o1, o2);
  }
  static auto add(simd_base_t o1, simd_base_t o2) {
    return _mm256_add_epi16(o1, o2);
  }
  static auto max(simd_base_t o1, simd_base_t o2) {
    return _mm256_max_epi16(o1, o2);
  }
  static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) {
    return _mm256_blendv_epi8(o1, o2, m);
  }
  static auto eq(simd_base_t o1, simd_base_t o2) {
    return _mm256_cmpeq_epi16(o1, o2);
  }
  static auto _and(simd_base_t o1, simd_base_t o2) {
    return _mm256_and_si256(o1, o2);
  }
  static auto _or(simd_base_t o1, simd_base_t o2) {
    return _mm256_or_si256(o1, o2);
  }
  static auto _xor(simd_base_t o1, simd_base_t o2) {
    return _mm256_xor_si256(o1, o2);
  }
  static auto load(int_type_t const (&src)[Width]) {
    return _mm256_lddqu_si256(reinterpret_cast<const simd_base_t *>(src));
  }
};

template <> struct simd_prims<int32_t, 8> : simd_prims_base<int32_t, 8, __m256i> {
  static auto set1(int_type_t v) { return _mm256_set1_epi32(v); }
  static auto sub(simd_base_t o1, simd_base_t o2) {
    return _mm256_sub_epi32(o1, o2);
  }
  static auto add(simd_base_t o1, simd_base_t o2) {
    return _mm256_add_epi32(o1, o2);
  }
  static auto max(simd_base_t o1, simd_base_t o2) {
    return _mm256_max_epi32(o1, o2);
  }
  static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) {
    return _mm256_blendv_epi8(o1, o2, m);
  }
  static auto eq(simd_base_t o1, simd_base_t o2) {
    return _mm256_cmpeq_epi32(o1, o2);
  }
  static auto _and(simd_base_t o1, simd_base_t o2) {
    return _mm256_and_si256(o1, o2);
  }
  static auto _or(simd_base_t o1, simd_base_t o2) {
    return _mm256_or_si256(o1, o2);
  }
  static auto _xor(simd_base_t o1, simd_base_t o2) {
    return _mm256_xor_si256(o1, o2);
  }
  static auto load(int_type_t const (&src)[Width]) {
    return _mm256_lddqu_si256(reinterpret_cast<const simd_base_t *>(src));
  }
};

#elif defined(__SSE4_1__)

template <> struct simd_prims<int8_t, 16> : simd_prims_base<int8_t, 16, __m128i> {
  static auto set1(int_type_t v) { return _mm_set1_epi8(v); }
  static auto sub(simd_base_t o1, simd_base_t o2) {
    return _mm_sub_epi8(o1, o2);
  }
  static auto add(simd_base_t o1, simd_base_t o2) {
    return _mm_add_epi8(o1, o2);
  }
  static auto max(simd_base_t o1, simd_base_t o2) {
    return _mm_max_epi8(o1, o2);
  }
  static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) {
    return _mm_blendv_epi8(o1, o2, m);
  }
  static auto eq(simd_base_t o1, simd_base_t o2) {
    return _mm_cmpeq_epi8(o1, o2);
  }
  static auto _and(simd_base_t o1, simd_base_t o2) {
    return _mm_and_si128(o1, o2);
  }
  static auto _or(simd_base_t o1, simd_base_t o2) {
    return _mm_or_si128(o1, o2);
  }
  static auto _xor(simd_base_t o1, simd_base_t o2) {
    return _mm_xor_si128(o1, o2);
  }
  static auto load(int_type_t const (&src)[Width]) {
    return _mm_loadu_si128(reinterpret_cast<const simd_base_t *>(src));
  }
};

template <> struct simd_prims<int16_t, 8> : simd_prims_base<int16_t, 8, __m128i> {
  static auto set1(int_type_t v) { return _mm_set1_epi16(v); }
  static auto sub(simd_base_t o1, simd_base_t o2) {
    return _mm_sub_epi16(o1, o2);
  }
  static auto add(simd_base_t o1, simd_base_t o2) {
    return _mm_add_epi16(o1, o2);
  }
  static auto max(simd_base_t o1, simd_base_t o2) {
    return _mm_max_epi16(o1, o2);
  }
  static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) {
    return _mm_blendv_epi8(o1, o2, m);
  }
  static auto eq(simd_base_t o1, simd_base_t o2) {
    return _mm_cmpeq_epi16(o1, o2);
  }
  static auto _and(simd_base_t o1, simd_base_t o2) {
    return _mm_and_si128(o1, o2);
  }
  static auto _or(simd_base_t o1, simd_base_t o2) {
    return _mm_or_si128(o1, o2);
  }
  static auto _xor(simd_base_t o1, simd_base_t o2) {
    return _mm_xor_si128(o1, o2);
  }
  static auto load(int_type_t const (&src)[Width]) {
    return _mm_loadu_si128(reinterpret_cast<const simd_base_t *>(src));
  }
};

template <> struct simd_prims<int32_t, 4> : simd_prims_base<int32_t, 4, __m128i> {
  static auto set1(int_type_t v) { return _mm_set1_epi32(v); }
  static auto sub(simd_base_t o1, simd_base_t o2) {
    return _mm_sub_epi32(o1, o2);
  }
  static auto add(simd_base_t o1, simd_base_t o2) {
    return _mm_add_epi32(o1, o2);
  }
  static auto max(simd_base_t o1, simd_base_t o2) {
    return _mm_max_epi32(o1, o2);
  }
  static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) {
    return _mm_blendv_epi8(o1, o2, m);
  }
  static auto eq(simd_base_t o1, simd_base_t o2) {
    return _mm_cmpeq_epi32(o1, o2);
  }
  static auto _and(simd_base_t o1, simd_base_t o2) {
    return _mm_and_si128(o1, o2);
  }
  static auto _or(simd_base_t o1, simd_base_t o2) {
    return _mm_or_si128(o1, o2);
  }
  static auto _xor(simd_base_t o1, simd_base_t o2) {
    return _mm_xor_si128(o1, o2);
  }
  static auto load(int_type_t const (&src)[Width]) {
    return _mm_loadu_si128(reinterpret_cast<const simd_base_t *>(src));
  }
};

#else // Portable scalar fallback

struct ScalarLane {
  int32_t v = 0;
};

template <> struct simd_prims<int32_t, 1> : simd_prims_base<int32_t, 1, ScalarLane> {
  static ScalarLane set1(int_type_t v) { return {v}; }
  static ScalarLane sub(ScalarLane o1, ScalarLane o2) { return {o1.v - o2.v}; }
  static ScalarLane add(ScalarLane o1, ScalarLane o2) { return {o1.v + o2.v}; }
  static ScalarLane max(ScalarLane o1, ScalarLane o2) {
    return {o1.v > o2.v ? o1.v : o2.v};
  }
  static ScalarLane blend(ScalarLane o1, ScalarLane o2, ScalarLane m) {
    return (m.v & 0x80000000u) ? o2 : o1;
  }
  static ScalarLane eq(ScalarLane o1, ScalarLane o2) {
    return {o1.v == o2.v ? -1 : 0};
  }
  static ScalarLane _and(ScalarLane o1, ScalarLane o2) { return {o1.v & o2.v}; }
  static ScalarLane _or(ScalarLane o1, ScalarLane o2) { return {o1.v | o2.v}; }
  static ScalarLane _xor(ScalarLane o1, ScalarLane o2) { return {o1.v ^ o2.v}; }
  static ScalarLane load(int_type_t const (&src)[Width]) { return {src[0]}; }
};

#endif

template <class simd_t, class T = typename simd_t::lut_data_t>
struct alignas(64) ScoreCache {
  T data[256][256]{};

  void GenerateGScores(Config const &cfg) {
    for (int i = 0; i < 256; ++i) {
      for (int j = 0; j < 256; ++j) {
        if (i == j)
          data[i][j] = static_cast<T>(cfg.full_match_bonus);
        else if (tolower_u8(static_cast<uint8_t>(i)) ==
                 tolower_u8(static_cast<uint8_t>(j)))
          data[i][j] = static_cast<T>(cfg.icase_match_bonus);
        else
          data[i][j] = static_cast<T>(cfg.mismatch_penalty);
      }
    }
  }

  void Gather(char q, typename simd_t::simd_base_t tgt,
              typename simd_t::simd_base_t &out_scores) const {
    const auto *pB =
        reinterpret_cast<const typename simd_t::int_type_t *>(&tgt);
    auto *pS = reinterpret_cast<typename simd_t::int_type_t *>(&out_scores);
    const uint8_t uq = static_cast<uint8_t>(q);
    for (size_t i = 0; i < simd_t::Width; ++i) {
      const uint8_t ut = static_cast<uint8_t>(pB[i]);
      pS[i] = static_cast<typename simd_t::int_type_t>(data[uq][ut]);
    }
  }
};

template <class simd_t, char... d> struct alignas(64) Delimiters {
  static constexpr size_t kPow2 = GetCeilPow2(sizeof...(d));
  static constexpr size_t kLog2 = GetLog2(kPow2);
  typename simd_t::int_type_t dMasks[sizeof...(d)][simd_t::Width]{};
  using V = typename simd_t::simd_base_t;

  template <size_t... I> constexpr void FillD(std::index_sequence<I...>) {
    auto fill_d = [](typename simd_t::int_type_t (&dest)[simd_t::Width],
                     char c) {
      for (size_t i = 0; i < simd_t::Width; ++i)
        dest[i] = static_cast<typename simd_t::int_type_t>(c);
    };
    (fill_d(dMasks[I], d), ...);
  }

  constexpr Delimiters() { FillD(std::make_index_sequence<sizeof...(d)>()); }

  template <size_t... I>
  void Eq(V *temp, V syms, std::index_sequence<I...>) const {
    ((temp[I] = simd_t::eq(syms, simd_t::load(dMasks[I]))), ...);
  }

  template <size_t... I> void OrStage(V *temp, std::index_sequence<I...>) const {
    ((temp[I] = simd_t::_or(temp[I], temp[I + sizeof...(I)])), ...);
  }

  template <size_t... I> void OrLog(V *temp, std::index_sequence<I...>) const {
    ((OrStage(temp, std::make_index_sequence<(1u << (kLog2 - I - 1))>())), ...);
  }

  V MatchDelimiters(V syms) const {
    V temp[kPow2] = {};
    Eq(temp, syms, std::make_index_sequence<sizeof...(d)>());
    OrLog(temp, std::make_index_sequence<kLog2>());
    return temp[0];
  }
};

template <class simd_t> struct SIMDImpl {
  using vec = typename simd_t::simd_base_t;
  using input_t = typename simd_t::input_t;
  using input_char_src_t = typename simd_t::input_char_src_t;
  using input_span_t = typename simd_t::input_span_t;
  using input_char_src_span_t = typename simd_t::input_char_src_span_t;

  template <class In> struct span_for;
  template <> struct span_for<input_t> { using type = input_span_t; };
  template <> struct span_for<input_char_src_t> {
    using type = input_char_src_span_t;
  };

  static constexpr size_t kWidth = simd_t::Width;
  static constexpr size_t kTargetCacheSize = 64;
  Config m_Config;
  ScoreCache<simd_t> m_LUTCache;
  const Delimiters<simd_t, ' ', '_', '.', ':', '-', '=', ','> m_Delimiters;

  vec delim_bonus{};
  vec extend_gap_penalty_v{};
  vec open_extend_gap_penalty_v{};

  struct Context {
    const SIMDImpl &impl;
    std::vector<vec> H_prev;
    std::vector<vec> H_cur;
    std::vector<vec> E;

    size_t m_CacheLastOffset = size_t(-1);
    vec m_CachedTargets[kTargetCacheSize]{};
    size_t m_CachedValidityMask[kTargetCacheSize]{};
    vec m_CachedTargetValidityVecMask[kTargetCacheSize]{};

    explicit Context(const SIMDImpl &im) : impl(im) {}

    void pack_chars(input_span_t const &targets, int j, vec &out_orig_case,
                    vec &out_mask, size_t &out_bitmask) {
      for (size_t i = 0, n = targets.size(); i < n; ++i) {
        if (!(out_bitmask & (size_t(1) << i)))
          continue;
        auto const &t = targets[i];
        if (size_t(j) < t.length())
          simd_t::set_idx(i, static_cast<typename simd_t::int_type_t>(t[j]),
                          out_orig_case);
        else {
          out_bitmask &= ~(size_t(1) << i);
          simd_t::set_idx(i, 0, out_mask);
        }
      }
    }

    void fill_cache_from_sources(input_char_src_span_t const &targets,
                                 int offset) {
      std::memset(m_CachedValidityMask, 0, sizeof(m_CachedValidityMask));
      std::memset(m_CachedTargetValidityVecMask, 0,
                  sizeof(m_CachedTargetValidityVecMask));

      char local_buf[kTargetCacheSize];
      for (size_t s = 0, n = targets.size(); s < n; ++s) {
        auto *pSrc = targets[s];
        size_t l = pSrc->read(local_buf, offset, kTargetCacheSize);
        size_t m = size_t(1) << s;
        for (size_t i = 0; i < l; ++i) {
          auto *pInt = reinterpret_cast<typename simd_t::int_type_t *>(
              &m_CachedTargets[i]);
          pInt[s] = static_cast<typename simd_t::int_type_t>(local_buf[i]);
          m_CachedValidityMask[i] |= m;
          auto *pMask = reinterpret_cast<typename simd_t::int_type_t *>(
              &m_CachedTargetValidityVecMask[i]);
          pMask[s] = simd_t::kMaskVal;
        }
      }
    }

    void pack_chars(input_char_src_span_t const &targets, int j,
                    vec &out_orig_case, vec &out_mask, size_t &out_bitmask) {
      if (size_t(j) < m_CacheLastOffset ||
          size_t(j) >= (m_CacheLastOffset + kTargetCacheSize)) {
        m_CacheLastOffset = j;
        fill_cache_from_sources(targets, j);
      }
      size_t off = j - m_CacheLastOffset;
      out_bitmask = m_CachedValidityMask[off];
      out_mask = m_CachedTargetValidityVecMask[off];
      out_orig_case = m_CachedTargets[off];
    }

    size_t get_max_length(input_char_src_span_t const &targets) const {
      size_t maxTargetLen = 0;
      for (auto const *sv : targets)
        maxTargetLen = std::max(maxTargetLen, sv->length());
      return maxTargetLen;
    }

    size_t get_max_length(input_span_t const &targets) const {
      size_t maxTargetLen = 0;
      for (auto const &sv : targets)
        maxTargetLen = std::max(maxTargetLen, sv.length());
      return maxTargetLen;
    }

    template <class InType>
    typename simd_t::scores_t sw_score_simd(std::string_view const &query,
                                            InType const &targets) {
      m_CacheLastOffset = size_t(-1);
      size_t maxTargetLen = get_max_length(targets);
      const size_t queryLen = query.length();
      const int k_max_penalty_per_char =
          std::min(impl.m_Config.extend_gap_penalty + impl.m_Config.open_gap_penalty,
                   impl.m_Config.mismatch_penalty);
      const int k_max_penalty = int(queryLen) * k_max_penalty_per_char;

      const vec max_penalty = simd_t::set1(
          static_cast<typename simd_t::int_type_t>(k_max_penalty));
      const vec zero = simd_t::set1(0);

      vec best = simd_t::set1(0);

      H_prev.resize(maxTargetLen + 1);
      std::fill(H_prev.begin(), H_prev.end(), zero);
      H_cur.resize(maxTargetLen + 1);
      E.resize(maxTargetLen + 1);
      std::fill(E.begin(), E.end(), max_penalty);

      const vec mismatch_penalty_v = simd_t::set1(
          static_cast<typename simd_t::int_type_t>(impl.m_Config.mismatch_penalty));

      for (size_t i = 1; i <= queryLen; ++i) {
        H_cur[0] = zero;
        vec F = zero;
        char qc = query[i - 1];
        bool apply_delimiter_bonus = (i == 1);
        vec prev_is_delim = zero;
        vec valid_targets_mask = simd_t::set1(simd_t::kMaskVal);
        size_t valid_targets_bitmask = size_t(-1);

        for (size_t j = 1; j <= maxTargetLen; ++j) {
          vec target_chars_orig{};
          pack_chars(targets, int(j - 1), target_chars_orig, valid_targets_mask,
                     valid_targets_bitmask);
          vec is_delim = impl.m_Delimiters.MatchDelimiters(target_chars_orig);
          E[j] = simd_t::max(
              simd_t::add(E[j], impl.extend_gap_penalty_v),
              simd_t::add(H_cur[j - 1], impl.open_extend_gap_penalty_v));
          F = simd_t::max(
              simd_t::add(F, impl.extend_gap_penalty_v),
              simd_t::add(H_prev[j], impl.open_extend_gap_penalty_v));

          vec match_score{};
          impl.m_LUTCache.Gather(qc, target_chars_orig, match_score);
          vec mismatches = simd_t::eq(match_score, mismatch_penalty_v);
          vec delim_bonus_mask =
              apply_delimiter_bonus
                  ? simd_t::blend(
                        simd_t::_and(simd_t::_xor(is_delim, prev_is_delim),
                                     prev_is_delim),
                        zero, mismatches)
                  : zero;
          vec delim_bonus_val =
              apply_delimiter_bonus
                  ? simd_t::blend(zero, impl.delim_bonus, delim_bonus_mask)
                  : zero;
          match_score = simd_t::add(delim_bonus_val, match_score);

          vec diag = simd_t::add(H_prev[j - 1], match_score);
          vec H = simd_t::max(simd_t::max(zero, diag), simd_t::max(E[j], F));
          H = simd_t::blend(zero, H, valid_targets_mask);
          H_cur[j] = H;

          best = simd_t::max(H, best);
          prev_is_delim = is_delim;
        }
        std::swap(H_prev, H_cur);
      }

      return simd_t::unpack(best);
    }
  };

  bool QueryFits(std::string_view const &q) const {
    const int min_val =
        static_cast<int>(std::numeric_limits<typename simd_t::int_type_t>::min());
    const int max_val =
        static_cast<int>(std::numeric_limits<typename simd_t::int_type_t>::max());
    const int worst_neg =
        int(q.length()) *
        std::min(m_Config.open_gap_penalty + m_Config.extend_gap_penalty,
                 m_Config.mismatch_penalty);
    const int best_pos =
        int(q.length()) * m_Config.full_match_bonus +
        m_Config.delimiter_boundary_bonus;
    return min_val <= worst_neg && best_pos <= max_val;
  }

  void Setup(Config const &cfg) {
    m_Config = cfg;
    m_LUTCache.GenerateGScores(m_Config);
    delim_bonus = simd_t::set1(
        static_cast<typename simd_t::int_type_t>(cfg.delimiter_boundary_bonus));
    extend_gap_penalty_v = simd_t::set1(
        static_cast<typename simd_t::int_type_t>(cfg.extend_gap_penalty));
    open_extend_gap_penalty_v = simd_t::set1(
        static_cast<typename simd_t::int_type_t>(cfg.open_gap_penalty +
                                                 cfg.extend_gap_penalty));
  }
};

int scalarMatch(std::string_view const &query, std::string_view const &target,
                Config const &cfg) {
  if (query.empty() || target.empty())
    return 0;
  const size_t n = target.size();
  const int open_extend = cfg.open_gap_penalty + cfg.extend_gap_penalty;
  const int k_max_penalty =
      int(query.size()) * std::min(open_extend, cfg.mismatch_penalty);

  std::vector<int> H_prev(n + 1, 0);
  std::vector<int> H_cur(n + 1, 0);
  std::vector<int> E(n + 1, k_max_penalty);

  int best = 0;
  for (size_t i = 1; i <= query.size(); ++i) {
    H_cur[0] = 0;
    int F = 0;
    const char qc = query[i - 1];
    const bool apply_delim = (i == 1);
    bool prev_is_delim = false;

    for (size_t j = 1; j <= n; ++j) {
      const char tc = target[j - 1];
      const bool cur_is_delim = isDelimiter(tc);
      E[j] = std::max(E[j] + cfg.extend_gap_penalty, H_cur[j - 1] + open_extend);
      F = std::max(F + cfg.extend_gap_penalty, H_prev[j] + open_extend);

      int score = cfg.mismatch_penalty;
      if (qc == tc)
        score = cfg.full_match_bonus;
      else if (tolower_u8(static_cast<uint8_t>(qc)) ==
               tolower_u8(static_cast<uint8_t>(tc)))
        score = cfg.icase_match_bonus;

      if (apply_delim && prev_is_delim && !cur_is_delim &&
          score != cfg.mismatch_penalty)
        score += cfg.delimiter_boundary_bonus;

      int H = std::max({0, H_prev[j - 1] + score, E[j], F});
      H_cur[j] = H;
      best = std::max(best, H);
      prev_is_delim = cur_is_delim;
    }
    std::swap(H_prev, H_cur);
  }
  return best;
}

} // namespace

int match(std::string_view const &query, std::string_view const &target,
          Config const &cfg) {
  return scalarMatch(query, target, cfg);
}

int match(std::string_view const &query, CharSource *target,
          Config const &cfg) {
  if (!target)
    return 0;
  std::string buf(target->length(), '\0');
  size_t readLen = target->read(buf.data(), 0, buf.size());
  return scalarMatch(query, std::string_view(buf.data(), readLen), cfg);
}

struct SIMDParMatcher::Impl {
  using WorkerFuncT = void (Impl::*)(int wId, void *pSIMD);

  ~Impl() { StopThreads(); }
  void StopThreads();
  void SetupThreads(int threadCount);
  template <class ResultT, class InputT> void Do();
  void WorkerFunc(int workerId);
  template <class SIMD, class input_t = typename SIMD::input_t, class ResultT,
            class InputT>
  void WorkerFuncTpl(int workerId, void *pSIMD);

#if defined(__AVX2__)
  SIMDImpl<simd_prims<int8_t, 32>> m_Narrow8;
  SIMDImpl<simd_prims<int16_t, 16>> m_Mid16;
  SIMDImpl<simd_prims<int32_t, 8>> m_Wide32;
#elif defined(__SSE4_1__)
  SIMDImpl<simd_prims<int8_t, 16>> m_Narrow8;
  SIMDImpl<simd_prims<int16_t, 8>> m_Mid16;
  SIMDImpl<simd_prims<int32_t, 4>> m_Wide32;
#else
  SIMDImpl<simd_prims<int32_t, 1>> m_Wide32;
#endif

  template <class Fn> auto withSelectedSIMD(std::string_view query, Fn &&fn) {
#if defined(__AVX2__) || defined(__SSE4_1__)
    if (m_Narrow8.QueryFits(query))
      return fn(m_Narrow8);
    if (m_Mid16.QueryFits(query))
      return fn(m_Mid16);
#endif
    return fn(m_Wide32);
  }

  size_t m_WorkerWidth = 0;
  bool m_WorkerCancel = false;
  barrier m_Barrier{0};
  std::vector<jthread> m_WorkerThreads;
  std::atomic<size_t> m_WorkerChunkOffset{0};

  template <class ResultT, class InputT> struct WorkerTypedContext {
    std::vector<ResultT> m_WorkerResults;
    InputT *m_pInputTargets = nullptr;
    ResultT *m_pResult = nullptr;
    int m_ScoreThreshold = 0;
  };
  void *m_pWorkerContext = nullptr;
  std::string_view m_Query;

  WorkerFuncT m_WorkerFunc = nullptr;
  void *m_pSIMDTypeErased = nullptr;
};

SIMDParMatcher::SIMDParMatcher(Config const &cfg)
    : m_Config(cfg), m_Impl(new Impl()) {
#if defined(__AVX2__) || defined(__SSE4_1__)
  m_Impl->m_Narrow8.Setup(cfg);
  m_Impl->m_Mid16.Setup(cfg);
#endif
  m_Impl->m_Wide32.Setup(cfg);
}

SIMDParMatcher::~SIMDParMatcher() = default;

SIMDParMatcher::Result SIMDParMatcher::match(std::string_view const &query,
                                             Input &&targets,
                                             Param const &params) {
  Result res;
  std::sort(targets.begin(), targets.end(),
            [](auto const &S1, auto const &S2) {
              return S1.length() > S2.length();
            });
  res.reserve(targets.size());
  m_Impl->withSelectedSIMD(query, [&](auto &impl) {
    using simd_impl_t = remove_cvref_t<decltype(impl)>;
    typename simd_impl_t::Context ctx{impl};
    for (size_t i = 0, n = targets.size(); i < n; i += simd_impl_t::kWidth) {
      typename simd_impl_t::input_t block{};
      size_t l = std::min(simd_impl_t::kWidth, n - i);
      for (size_t j = 0; j < l; ++j)
        block[j] = targets[i + j];
      auto scores = ctx.sw_score_simd(
          query, typename simd_impl_t::input_span_t(block.data(), l));
      for (size_t j = 0; j < l; ++j)
        if (auto s = static_cast<int>(scores[j]); s > params.scoreThreshold)
          res.emplace_back(targets[i + j], s);
    }
  });
  if (params.sortResults)
    std::sort(res.begin(), res.end(),
              [](auto const &S1, auto const &S2) {
                return S1.score > S2.score;
              });
  return res;
}

SIMDParMatcher::CharSourceResult
SIMDParMatcher::match(std::string_view const &query, CharSourceInput &&targets,
                      Param const &params) {
  CharSourceResult res;
  std::sort(targets.begin(), targets.end(),
            [](auto const *S1, auto const *S2) {
              return S1->length() > S2->length();
            });
  res.reserve(targets.size());
  m_Impl->withSelectedSIMD(query, [&](auto &impl) {
    using simd_impl_t = remove_cvref_t<decltype(impl)>;
    typename simd_impl_t::Context ctx{impl};
    for (size_t i = 0, n = targets.size(); i < n; i += simd_impl_t::kWidth) {
      typename simd_impl_t::input_char_src_t block{};
      size_t l = std::min(simd_impl_t::kWidth, n - i);
      for (size_t j = 0; j < l; ++j)
        block[j] = targets[i + j];
      auto scores = ctx.sw_score_simd(
          query, typename simd_impl_t::input_char_src_span_t(block.data(), l));
      for (size_t j = 0; j < l; ++j)
        if (auto s = static_cast<int>(scores[j]); s > params.scoreThreshold)
          res.emplace_back(targets[i + j], s);
    }
  });
  if (params.sortResults)
    std::sort(res.begin(), res.end(),
              [](auto const &S1, auto const &S2) {
                return S1.score > S2.score;
              });
  return res;
}

SIMDParMatcher::Result SIMDParMatcher::match_par(std::string_view const &query,
                                                 Input &&targets,
                                                 Param const &params) {
  if (m_Impl->m_WorkerThreads.empty())
    return match(query, std::move(targets), params);

  Result res;
  Impl::WorkerTypedContext<Result, Input> ctx;
  m_Impl->m_pWorkerContext = &ctx;
  ctx.m_pInputTargets = &targets;
  ctx.m_pResult = &res;
  ctx.m_ScoreThreshold = params.scoreThreshold;
  m_Impl->m_Query = query;

  std::sort(targets.begin(), targets.end(),
            [](auto const &S1, auto const &S2) {
              return S1.length() > S2.length();
            });

  m_Impl->withSelectedSIMD(query, [&](auto &impl) {
    using simd_t = remove_cvref_t<decltype(impl)>;
    m_Impl->m_WorkerFunc =
        &Impl::WorkerFuncTpl<simd_t, typename simd_t::input_t, Result, Input>;
    m_Impl->m_pSIMDTypeErased = &impl;
    m_Impl->m_WorkerWidth = simd_t::kWidth;
  });

  m_Impl->Do<Result, Input>();

  if (params.sortResults)
    std::sort(res.begin(), res.end(),
              [](auto const &S1, auto const &S2) {
                return S1.score > S2.score;
              });

  m_Impl->m_WorkerFunc = nullptr;
  m_Impl->m_pWorkerContext = nullptr;
  m_Impl->m_pSIMDTypeErased = nullptr;
  return res;
}

SIMDParMatcher::CharSourceResult
SIMDParMatcher::match_par(std::string_view const &query,
                          CharSourceInput &&targets, Param const &params) {
  if (m_Impl->m_WorkerThreads.empty())
    return match(query, std::move(targets), params);

  CharSourceResult res;
  Impl::WorkerTypedContext<CharSourceResult, CharSourceInput> ctx;
  m_Impl->m_pWorkerContext = &ctx;
  ctx.m_pInputTargets = &targets;
  ctx.m_pResult = &res;
  ctx.m_ScoreThreshold = params.scoreThreshold;
  m_Impl->m_Query = query;

  std::sort(targets.begin(), targets.end(),
            [](auto const *S1, auto const *S2) {
              return S1->length() > S2->length();
            });

  m_Impl->withSelectedSIMD(query, [&](auto &impl) {
    using simd_t = remove_cvref_t<decltype(impl)>;
    m_Impl->m_WorkerFunc =
        &Impl::WorkerFuncTpl<simd_t, typename simd_t::input_char_src_t,
                             CharSourceResult, CharSourceInput>;
    m_Impl->m_pSIMDTypeErased = &impl;
    m_Impl->m_WorkerWidth = simd_t::kWidth;
  });

  m_Impl->Do<CharSourceResult, CharSourceInput>();

  if (params.sortResults)
    std::sort(res.begin(), res.end(),
              [](auto const &S1, auto const &S2) {
                return S1.score > S2.score;
              });

  m_Impl->m_WorkerFunc = nullptr;
  m_Impl->m_pWorkerContext = nullptr;
  m_Impl->m_pSIMDTypeErased = nullptr;
  return res;
}

void SIMDParMatcher::SetupThreads(int threadCount) {
  m_Impl->SetupThreads(threadCount);
}

void SIMDParMatcher::StopThreads() { m_Impl->StopThreads(); }

void SIMDParMatcher::Impl::StopThreads() {
  if (m_Barrier) {
    m_WorkerCancel = true;
    m_Barrier.arrive_and_wait();
    m_WorkerThreads.clear();
    m_WorkerCancel = false;
    m_Barrier.change_group_size(0);
  }
}

void SIMDParMatcher::Impl::SetupThreads(int threadCount) {
  StopThreads();
  if (threadCount <= 0)
    return;
  m_Barrier.change_group_size(static_cast<size_t>(threadCount) + 1);
  for (int i = 0; i < threadCount; ++i)
    m_WorkerThreads.emplace_back(&Impl::WorkerFunc, this, i);
}

template <class ResultT, class InputT> void SIMDParMatcher::Impl::Do() {
  auto &ctx = *static_cast<WorkerTypedContext<ResultT, InputT> *>(m_pWorkerContext);
  size_t nThreads = m_WorkerThreads.size();
  ctx.m_WorkerResults.resize(nThreads);
  for (auto &par : ctx.m_WorkerResults)
    par.clear();
  m_WorkerChunkOffset.store(nThreads * m_WorkerWidth,
                            std::memory_order_relaxed);

  m_Barrier.arrive_and_wait();
  m_Barrier.arrive_and_wait();

  for (auto &par : ctx.m_WorkerResults)
    for (auto &l : par)
      ctx.m_pResult->push_back(l);
}

template <class simd_t, class input_t, class ResultT, class InputT>
void SIMDParMatcher::Impl::WorkerFuncTpl(int workerId, void *pSIMD) {
  simd_t &simd = *reinterpret_cast<simd_t *>(pSIMD);
  typename simd_t::Context ctx_simd{simd};
  auto &ctx = *static_cast<WorkerTypedContext<ResultT, InputT> *>(m_pWorkerContext);
  auto &lines = *ctx.m_pInputTargets;
  auto &results = ctx.m_WorkerResults[workerId];
  size_t i = static_cast<size_t>(workerId) * m_WorkerWidth;
  size_t n = lines.size();
  while (i < n) {
    size_t tn = std::min(m_WorkerWidth, n - i);
    input_t in{};
    std::copy_n(lines.data() + i, tn, in.begin());
    auto sp = typename simd_t::template span_for<input_t>::type{in.data(), tn};
    auto scores = ctx_simd.sw_score_simd(m_Query, sp);
    for (size_t j = 0; j < tn; ++j) {
      if (static_cast<int>(scores[j]) > ctx.m_ScoreThreshold)
        results.emplace_back(lines[i + j], static_cast<int>(scores[j]));
    }
    i = m_WorkerChunkOffset.fetch_add(m_WorkerWidth, std::memory_order_relaxed);
  }
}

void SIMDParMatcher::Impl::WorkerFunc(int workerId) {
  while (true) {
    m_Barrier.arrive_and_wait();
    if (m_WorkerCancel)
      break;
    (this->*m_WorkerFunc)(workerId, m_pSIMDTypeErased);
    m_Barrier.arrive_and_wait();
  }
}

} // namespace fuzzy_sw
