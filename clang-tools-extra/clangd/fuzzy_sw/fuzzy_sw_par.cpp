#include "fuzzy_sw.hpp"

#include <cstring>
#include <span>
#include <thread>
#include <atomic>
#include <array>
//#include <barrier>
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include <immintrin.h>

namespace fuzzy_sw {
using std::atomic;

// threading
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

  size_t size() const { return end() - begin(); }

  T& operator[](size_t i) { return Begin[i]; }
  const T& operator[](size_t i) const { return Begin[i]; }
};
    }
    //threading
    namespace{
        struct jthread
        {
            jthread() = default;
            template<class... T>
            jthread(T&&... args):_T{std::forward<T>(args)...}{}
            jthread(jthread &&rhs):_T{std::move(rhs._T)}{}
            jthread(const jthread &rhs) = delete;
            ~jthread() { if (_T.joinable()) _T.join(); }
            std::thread _T;
        };

        struct barrier
        {
            barrier(size_t gSize = 0): m_Counter(gSize), m_GroupSize(gSize){}

            size_t m_Counter = 0;
            size_t m_GroupSize = 0;
            size_t m_Epoch = 0;
            std::mutex m_Mutex;
            std::condition_variable m_Cond;

            operator bool() const { return m_GroupSize > 0; }

            void change_group_size(size_t s)
            {
                m_Counter = m_GroupSize = s;
            }

            void arrive_and_wait()
            {
                std::unique_lock<std::mutex> g(m_Mutex);
                if (--m_Counter == 0)
                {
                    ++m_Epoch;
                    m_Counter = m_GroupSize;
                    m_Cond.notify_all();
                    g.unlock();
                }else
                {
                    auto epoch = m_Epoch;
                    m_Cond.wait(g, [&]{ return m_Epoch > epoch;});
                }
            }
        };
    }
    //simd
    namespace{
        template<typename IntType, size_t W>
        struct simd_prims;

        template<typename IntType, size_t W, typename simd_t>
        struct simd_prims_base
        {
            static constexpr size_t Width = W;
            using int_type_t = IntType;
            using simd_base_t = simd_t;

            using input_t = std::array<std::string_view, W>;
            using input_span_t = Span<std::string_view>;
            using input_char_src_t = std::array<CharSource*, W>;
            using input_char_src_span_t = Span<CharSource*>;
            using scores_t = std::array<IntType, W>;
            using lut_data_t = int8_t;
            static constexpr IntType kMaskVal = IntType(-1);

            static auto set_idx(int idx, int_type_t v, simd_base_t &t) 
            { 
                int_type_t *p = reinterpret_cast<int_type_t *>(&t);
                p[idx] = v;
            }

            static auto unpack(simd_base_t o)
            {
                scores_t s;
                int_type_t *pB = reinterpret_cast<int_type_t *>(&o);
                for(size_t i = 0; i < Width; ++i)
                    s[i] = pB[i];
                return s;
            }
        };

        template<>
        struct simd_prims<int16_t, 8>: simd_prims_base<int16_t, 8, __m128i>
        {
            static auto set1(int_type_t v) { return _mm_set1_epi16(v); }
            static auto sub(simd_base_t o1, simd_base_t o2) { return _mm_sub_epi16(o1, o2); }
            static auto add(simd_base_t o1, simd_base_t o2) { return _mm_add_epi16(o1, o2); }
            static auto max(simd_base_t o1, simd_base_t o2) { return _mm_max_epi16(o1, o2); }
            static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) { return _mm_blendv_epi8(o1, o2, m); }
            static auto eq(simd_base_t o1, simd_base_t o2) { return _mm_cmpeq_epi16(o1, o2); }
            static auto _and(simd_base_t o1, simd_base_t o2) { return _mm_and_si128(o1, o2); }
            static auto _or(simd_base_t o1, simd_base_t o2) { return _mm_or_si128(o1, o2); }
            static auto _xor(simd_base_t o1, simd_base_t o2) { return _mm_xor_si128(o1, o2); }
            static auto load(int_type_t const(&src)[Width]) { return _mm_loadu_epi16(src); }
        };

        template<>
        struct simd_prims<int8_t, 16>: simd_prims_base<int8_t, 16, __m128i>
        {
            static auto set1(int_type_t v) { return _mm_set1_epi8(v); }
            static auto sub(simd_base_t o1, simd_base_t o2) { return _mm_sub_epi8(o1, o2); }
            static auto add(simd_base_t o1, simd_base_t o2) { return _mm_add_epi8(o1, o2); }
            static auto max(simd_base_t o1, simd_base_t o2) { return _mm_max_epi8(o1, o2); }
            static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) { return _mm_blendv_epi8(o1, o2, m); }
            static auto eq(simd_base_t o1, simd_base_t o2) { return _mm_cmpeq_epi8(o1, o2); }
            static auto _and(simd_base_t o1, simd_base_t o2) { return _mm_and_si128(o1, o2); }
            static auto _or(simd_base_t o1, simd_base_t o2) { return _mm_or_si128(o1, o2); }
            static auto _xor(simd_base_t o1, simd_base_t o2) { return _mm_xor_si128(o1, o2); }
            static auto load(int_type_t const(&src)[Width]) { return _mm_loadu_epi8(src); }
        };

        template<>
        struct simd_prims<int8_t, 32>: simd_prims_base<int8_t, 32, __m256i>
        {
            static auto set1(int_type_t v) { return _mm256_set1_epi8(v); }
            static auto sub(simd_base_t o1, simd_base_t o2) { return _mm256_sub_epi8(o1, o2); }
            static auto add(simd_base_t o1, simd_base_t o2) { return _mm256_add_epi8(o1, o2); }
            static auto max(simd_base_t o1, simd_base_t o2) { return _mm256_max_epi8(o1, o2); }
            static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) { return _mm256_blendv_epi8(o1, o2, m); }
            static auto eq(simd_base_t o1, simd_base_t o2) { return _mm256_cmpeq_epi8(o1, o2); }
            static auto _and(simd_base_t o1, simd_base_t o2) { return _mm256_and_si256(o1, o2); }
            static auto _or(simd_base_t o1, simd_base_t o2) { return _mm256_or_si256(o1, o2); }
            static auto _xor(simd_base_t o1, simd_base_t o2) { return _mm256_xor_si256(o1, o2); }
            static auto load(int_type_t const(&src)[Width]) { return _mm256_lddqu_si256((const simd_base_t*)src); }
        };

        template<>
        struct simd_prims<int16_t, 16>: simd_prims_base<int16_t, 16, __m256i>
        {
            static auto set1(int_type_t v) { return _mm256_set1_epi16(v); }
            static auto sub(simd_base_t o1, simd_base_t o2) { return _mm256_sub_epi16(o1, o2); }
            static auto add(simd_base_t o1, simd_base_t o2) { return _mm256_add_epi16(o1, o2); }
            static auto max(simd_base_t o1, simd_base_t o2) { return _mm256_max_epi16(o1, o2); }
            static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) { return _mm256_blendv_epi8(o1, o2, m); }
            static auto eq(simd_base_t o1, simd_base_t o2) { return _mm256_cmpeq_epi16(o1, o2); }
            static auto _and(simd_base_t o1, simd_base_t o2) { return _mm256_and_si256(o1, o2); }
            static auto _or(simd_base_t o1, simd_base_t o2) { return _mm256_or_si256(o1, o2); }
            static auto _xor(simd_base_t o1, simd_base_t o2) { return _mm256_xor_si256(o1, o2); }
            static auto load(int_type_t const(&src)[Width]) { return _mm256_lddqu_si256((const simd_base_t*)src); }
        };

        template<>
        struct simd_prims<int32_t, 8>: simd_prims_base<int32_t, 8, __m256i>
        {
            static auto set1(int_type_t v) { return _mm256_set1_epi32(v); }
            static auto sub(simd_base_t o1, simd_base_t o2) { return _mm256_sub_epi32(o1, o2); }
            static auto add(simd_base_t o1, simd_base_t o2) { return _mm256_add_epi32(o1, o2); }
            static auto max(simd_base_t o1, simd_base_t o2) { return _mm256_max_epi32(o1, o2); }
            static auto blend(simd_base_t o1, simd_base_t o2, simd_base_t m) { return _mm256_blendv_epi8(o1, o2, m); }
            static auto eq(simd_base_t o1, simd_base_t o2) { return _mm256_cmpeq_epi32(o1, o2); }
            static auto _and(simd_base_t o1, simd_base_t o2) { return _mm256_and_si256(o1, o2); }
            static auto _or(simd_base_t o1, simd_base_t o2) { return _mm256_or_si256(o1, o2); }
            static auto _xor(simd_base_t o1, simd_base_t o2) { return _mm256_xor_si256(o1, o2); }
            static auto load(int_type_t const(&src)[Width]) { return _mm256_lddqu_si256((const simd_base_t*)src); }
        };
    }

    constexpr uint8_t tolower_u8(uint8_t c) {
      return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
    }

    constexpr size_t GetCeilPow2(size_t v)
    {
        if ((v & (v - 1)) == 0)
            return v;//already
        size_t res = 1;
        while(res < v) res <<= 1;
        return res;
    }
    constexpr size_t GetLog2(size_t v)
    {
        size_t res = 0;
        while(v > 1) v >>= 1, ++res;
        return res;
    }

    template<class simd_t, class T = typename simd_t::lut_data_t>
    struct alignas(64) ScoreCache
    {
        static constexpr Config kCFG{};
        T data[256][256];

        static constexpr ScoreCache<simd_t, T> GenerateGScores()
        {
            using Cache = ScoreCache<simd_t, T>;
            Cache res;
            for(int i = 0; i < 256; ++i)
            {
                for(int j = 0; j < 256; ++j)
                {
                    if (i == j)
                        res.data[i][j] = kCFG.full_match_bonus;
                    else if (tolower_u8(uint8_t(i)) == tolower_u8(uint8_t(j)))
                        res.data[i][j] = kCFG.icase_match_bonus;
                    else
                        res.data[i][j] = kCFG.mismatch_penalty;
                }
            }
            return res;
        }

        void GenerateGScores(Config const& _cfg)
        {
            for(int i = 0; i < 256; ++i)
            {
                for(int j = 0; j < 256; ++j)
                {
                    if (i == j)
                        data[i][j] = _cfg.full_match_bonus;
                    else if (tolower_u8(uint8_t(i)) == tolower_u8(uint8_t(j)))
                        data[i][j] = _cfg.icase_match_bonus;
                    else
                        data[i][j] = _cfg.mismatch_penalty;
                }
            }
        }

        void Gather(char q, typename simd_t::simd_base_t tgt, typename simd_t::simd_base_t &out_scores)
        {
            typename simd_t::int_type_t *pB = reinterpret_cast<typename simd_t::int_type_t *>(&tgt);
            typename simd_t::int_type_t *pS = reinterpret_cast<typename simd_t::int_type_t *>(&out_scores);
            for(size_t i = 0; i < simd_t::Width; ++i)
                pS[i] = data[int(q)][pB[i]];
        }
    };

    template<class simd_t, char... d>
    struct alignas(64) Delimiters
    {
        static constexpr size_t kPow2 = GetCeilPow2(sizeof...(d));
        static constexpr size_t kLog2 = GetLog2(kPow2);
        typename simd_t::int_type_t dMasks[sizeof...(d)][simd_t::Width];

        constexpr Delimiters<simd_t, d...>()
        {
            auto fill_d = [](typename simd_t::int_type_t (&dest)[simd_t::Width], char c)
            {
                for(size_t i = 0; i < simd_t::Width; ++i) dest[i] = c;
            };
            [&]<size_t... I>(std::index_sequence<I...> s){
                (fill_d(dMasks[I], d),...);
            }(std::make_index_sequence<sizeof...(d)>());
        }

        typename simd_t::simd_base_t MatchDelimiters(typename simd_t::simd_base_t syms) const
        {
            using V = typename simd_t::simd_base_t;
            V temp[kPow2] = {};
            //1. check all masks and store results locally
            [&]<size_t... I>(std::index_sequence<I...> s){
                ((temp[I] = simd_t::eq(syms, simd_t::load(dMasks[I]))),...);
            }(std::make_index_sequence<sizeof...(d)>());

            //2. gradually 'or' all pairs
            auto or_stage = [&]<size_t... I>(std::index_sequence<I...> s){
                ((temp[I] = simd_t::_or(temp[I], temp[I + sizeof...(I)])),...);
            };
            [&]<size_t...I>(std::index_sequence<I...> s)
            {
                ((or_stage(std::make_index_sequence<(1 << (kLog2 - I - 1))>())),...);
            }(std::make_index_sequence<kLog2>());

            return temp[0];
        }

        void Generate()
        {
            auto fill_d = [](typename simd_t::int_type_t (&dest)[simd_t::Width], char c)
            {
                for(size_t i = 0; i < simd_t::Width; ++i) dest[i] = c;
            };
            [&]<size_t... I>(std::index_sequence<I...> s){
                (fill_d(dMasks[I], d),...);
            }(std::make_index_sequence<sizeof...(d)>());
        }
    };


    template<class simd_t>
    struct SIMDImpl
    {
        using vec = typename simd_t::simd_base_t;
        using input_t = typename simd_t::input_t;
        using input_char_src_t = typename simd_t::input_char_src_t;
        using input_span_t = typename simd_t::input_span_t;
        using input_char_src_span_t = typename simd_t::input_char_src_span_t;

        template<class In>
        struct span_for;

        template<> struct span_for<input_t> { using type = input_span_t; };
        template<> struct span_for<input_char_src_t> { using type = input_char_src_span_t; };

        static constexpr auto kWidth = simd_t::Width;
        static constexpr auto kTargetCacheSize = 64;
        Config m_Config;
        ScoreCache<simd_t> m_LUTCache;
        const Delimiters<simd_t, ' ', '_', '.', ':', '-', '=', ','> m_Delimiters;

        vec delim_bonus;
        vec extend_gap_penalty_v;
        vec open_extend_gap_penalty_v;

        struct Context
        {
            SIMDImpl &impl;
            std::vector<vec> H_prev;
            std::vector<vec> H_cur;
            std::vector<vec> E;
            vec m_CachedTargets[kTargetCacheSize];
            vec m_CachedTargetValidityVecMask[kTargetCacheSize];
            size_t m_CachedValidityMask[kTargetCacheSize];
            size_t m_CacheLastOffset = size_t(-1);

            Context(SIMDImpl &I):impl(I){}

            void fill_cache_from_sources(typename simd_t::input_char_src_span_t const& targets, int offset);

            void pack_chars(typename simd_t::input_span_t const& targets, int j, typename simd_t::simd_base_t &out_orig_case, typename simd_t::simd_base_t &out_mask, size_t &out_bitmask);
            void pack_chars(typename simd_t::input_char_src_span_t const& targets, int j, typename simd_t::simd_base_t &out_orig_case, typename simd_t::simd_base_t &out_mask, size_t &out_bitmask);

            size_t get_max_length(typename simd_t::input_char_src_span_t const& targets) const;
            size_t get_max_length(typename simd_t::input_span_t const& targets) const;

            template<class InType = typename simd_t::input_span_t>
            typename simd_t::scores_t sw_score_simd(std::string_view const&query, InType const& targets);
        };

        bool QueryFits(std::string_view const& q) const
        {
            return ((int)std::numeric_limits<typename simd_t::int_type_t>::min() <= int(q.length() * (m_Config.open_gap_penalty + m_Config.extend_gap_penalty)));
        }

        void Setup(Config const& cfg)
        {
            m_Config = cfg;
            m_LUTCache.GenerateGScores(m_Config);
            delim_bonus = simd_t::set1(cfg.delimiter_boundary_bonus);
            extend_gap_penalty_v = simd_t::set1(cfg.extend_gap_penalty);
            open_extend_gap_penalty_v = simd_t::set1(cfg.open_gap_penalty + cfg.extend_gap_penalty);
        }
    };

    template<class simd_t>
    void SIMDImpl<simd_t>::Context::pack_chars(typename simd_t::input_span_t const& targets, int j, typename simd_t::simd_base_t &out_orig_case, typename simd_t::simd_base_t &out_mask, size_t &out_bitmask)
    {
        for(size_t i = 0, n = targets.size(); i < n; ++i)
        {
            if (!(out_bitmask & size_t(1) << i))
                continue;

            auto const& t = targets[i];
            if (size_t(j) < t.length())
                simd_t::set_idx(i, t[j], out_orig_case);
            else
            {
                out_bitmask &= ~(size_t(1) << i);
                simd_t::set_idx(i, 0, out_mask);
            }
        }
    }

    template<class simd_t>
    void SIMDImpl<simd_t>::Context::fill_cache_from_sources(typename simd_t::input_char_src_span_t const& targets, int offset)
    {
        std::memset(std::begin(m_CachedValidityMask), 0, sizeof(m_CachedValidityMask));
        std::memset(std::begin(m_CachedTargetValidityVecMask), 0, sizeof(m_CachedTargetValidityVecMask));

        char local_buf[kTargetCacheSize];
        for(size_t s = 0, n = (int)targets.size(); s < n; ++s)
        {
            auto *pSrc = targets[s];
            size_t l = pSrc->read(local_buf, offset, kTargetCacheSize);
            size_t m = size_t(1) << s;
            for(size_t i = 0; i < l; ++i)
            {
                auto *pInt = (typename simd_t::int_type_t *)(&m_CachedTargets[i]);
                pInt[s] = local_buf[i];

                m_CachedValidityMask[i] |= m;
                pInt = (typename simd_t::int_type_t *)(&m_CachedTargetValidityVecMask[i]);
                pInt[s] = typename simd_t::int_type_t(-1);
            }
        }
    }

    template<class simd_t>
    void SIMDImpl<simd_t>::Context::pack_chars(typename simd_t::input_char_src_span_t const& targets, int j, typename simd_t::simd_base_t &out_orig_case, typename simd_t::simd_base_t &out_mask, size_t &out_bitmask)
    {
        //multithreading problem here:
        if (size_t(j) < m_CacheLastOffset || size_t(j) >= (m_CacheLastOffset + kTargetCacheSize))
        {
            m_CacheLastOffset = j;
            fill_cache_from_sources(targets, j);
        }

        size_t off = j - m_CacheLastOffset;
        out_bitmask = m_CachedValidityMask[off];
        out_mask = m_CachedTargetValidityVecMask[off];
        out_orig_case = m_CachedTargets[off];
    }

    template<class simd_t>
    size_t SIMDImpl<simd_t>::Context::get_max_length(typename simd_t::input_char_src_span_t const& targets) const
    {
        size_t maxTargetLen = 0;
        for(auto const& sv : targets) if (auto l = sv->length(); l > maxTargetLen) maxTargetLen = l;
        return maxTargetLen;
    }

    template<class simd_t>
    size_t SIMDImpl<simd_t>::Context::get_max_length(typename simd_t::input_span_t const& targets) const
    {
        size_t maxTargetLen = 0;
        for(auto const& sv : targets) if (auto l = sv.length(); l > maxTargetLen) maxTargetLen = l;
        return maxTargetLen;
    }

    template<class simd_t>
    template<class InType>
    typename simd_t::scores_t SIMDImpl<simd_t>::Context::sw_score_simd(std::string_view const&query, InType const& targets)
    {
        m_CacheLastOffset = size_t(-1);
        size_t maxTargetLen = get_max_length(targets);
        const size_t queryLen = query.length();
        const int k_max_penalty_per_char = (impl.m_Config.extend_gap_penalty + impl.m_Config.open_gap_penalty) < impl.m_Config.mismatch_penalty ? impl.m_Config.extend_gap_penalty + impl.m_Config.open_gap_penalty : impl.m_Config.mismatch_penalty;
        const int k_max_penalty = int(queryLen) * k_max_penalty_per_char;

        const vec max_penalty = simd_t::set1(k_max_penalty);
        const vec zero = simd_t::set1(0);
        
        vec best = simd_t::set1(0);

        H_prev.resize(maxTargetLen + 1);
        std::fill(H_prev.begin(), H_prev.end(), zero);

        H_cur.resize(maxTargetLen + 1);

        E.resize(maxTargetLen + 1);
        std::fill(E.begin(), E.end(), max_penalty);
        
        for(size_t i = 1; i <= queryLen; ++i)
        {
            H_cur[0] = zero;
            vec F = zero;
            char qc = query[i - 1];

            vec prev_is_delim = zero;
            vec valid_targets_mask = simd_t::set1(simd_t::kMaskVal);
            size_t valid_targets_bitmask = size_t(-1);
            for(size_t j = 1; j <= maxTargetLen; ++j)
            {
                vec target_chars_orig;
                pack_chars(targets, j - 1, target_chars_orig, valid_targets_mask, valid_targets_bitmask);
                vec is_delim = impl.m_Delimiters.MatchDelimiters(target_chars_orig);
                E[j] = simd_t::max(simd_t::add(E[j], impl.extend_gap_penalty_v), simd_t::add(H_cur[j - 1], impl.open_extend_gap_penalty_v));
                F = simd_t::max(simd_t::add(F, impl.extend_gap_penalty_v), simd_t::add(H_prev[j], impl.open_extend_gap_penalty_v));

                vec match_score;
                impl.m_LUTCache.Gather(qc, target_chars_orig, match_score);
                vec mismatches = simd_t::eq(match_score, simd_t::set1(-1));
                vec delim_bonus_mask = simd_t::blend(simd_t::_xor(is_delim, prev_is_delim), simd_t::set1(0), mismatches);
                vec delim_bonus_val = simd_t::blend(simd_t::set1(0), impl.delim_bonus, delim_bonus_mask);
                match_score = simd_t::add(delim_bonus_val, match_score);

                auto diag = simd_t::add(H_prev[j - 1], match_score);
                auto H = simd_t::max(simd_t::max(zero, diag), simd_t::max(E[j], F));
                H = simd_t::blend(zero, H, valid_targets_mask);
                H_cur[j] = H;

                best = simd_t::max(H, best);
                prev_is_delim = is_delim;
            }
            std::swap(H_prev, H_cur);
        }

        return simd_t::unpack(best);
    }

    struct SIMDParMatcher::Impl
    {
        using WorkerFuncT = void(Impl::*)(int wId, void* pSIMD);

        ~Impl();
        void StopThreads();
        void SetupThreads(int threadCount);
        template<class Result, class Input>
        void Do();
        void WorkerFunc(int workerId);
        template<class SIMD, class input_t = typename SIMD::input_t, class Result, class Input>
        void WorkerFuncTpl(int workerId, void *pSIMD);

        SIMDImpl<simd_prims<int32_t,8>> m_AVX2x32;
        SIMDImpl<simd_prims<int16_t,16>> m_AVX2x16;
        SIMDImpl<simd_prims<int8_t, 32>> m_AVX2x8;

        int m_WorkerWidth = 0;
        bool m_WorkerCancel = false;
        barrier m_Barrier{0};
        //std::optional<std::barrier<>> m_Barrier;
        std::vector<jthread> m_WorkerThreads;
        atomic<size_t> m_WorkerChunkOffset{0}; // job_idx

        template<class Result, class Input>
        struct WorkerTypedContext
        {
            std::vector<Result> m_WorkerResults;
            Input *m_pInputTargets = nullptr;
            Result *m_pResult = nullptr;
            int m_ScoreThreshold = 0;
        };
        void *m_pWorkerContext = nullptr;
        std::string_view m_Query;

        WorkerFuncT m_WorkerFunc = nullptr;
        void *m_pSIMDTypeErased = nullptr;
    };

    SIMDParMatcher::SIMDParMatcher(Config const& cfg):
        m_Config(cfg),
        m_Impl(new Impl())
    {
        m_Impl->m_AVX2x8.Setup(cfg);
        m_Impl->m_AVX2x16.Setup(cfg);
        m_Impl->m_AVX2x32.Setup(cfg);
    }


    SIMDParMatcher::~SIMDParMatcher()
    {
    }

    SIMDParMatcher::Result SIMDParMatcher::match(std::string_view const&query, Input &&targets, Param const& params)
    {
        Result res;
        std::sort(targets.begin(), targets.end(), [](auto const& S1, auto const& S2){ return S1.length() > S2.length(); });
        //std::ranges::sort(targets, std::greater{}, &std::string_view::length);
        auto Match = [&](auto &impl)
        {
            using simd_impl_t = std::__remove_cvref_t<decltype(impl)>;
            typename simd_impl_t::Context ctx{impl};
            res.reserve(targets.size());
            for(size_t i = 0, n = targets.size(); i < n; i += simd_impl_t::kWidth)
            {
                typename simd_impl_t::input_t block;
                size_t l = (i + simd_impl_t::kWidth) < n ? simd_impl_t::kWidth : (n - i);
                for(size_t j = 0; j < l; ++j)
                    block[j] = targets[i + j];
                auto scores = ctx.sw_score_simd(query, typename simd_impl_t::input_span_t(block.begin(), block.begin() + l));
                for(size_t j = 0; j < l; ++j)
                    if (auto s = scores[i + j]; s > params.scoreThreshold)
                        res.emplace_back(targets[i + j], s);
            }
        };

        if (auto &impl = m_Impl->m_AVX2x8; impl.QueryFits(query))
            Match(impl);
        else if (auto &impl = m_Impl->m_AVX2x16; impl.QueryFits(query))
            Match(impl);
        else
            Match(m_Impl->m_AVX2x32);

        if (params.sortResults)
            std::sort(res.begin(), res.end(), [](auto const& S1, auto const& S2){ return S1.score > S2.score; });
            //std::ranges::sort(res, std::greater{}, &ResultItem::score);
        return res;
    }

    SIMDParMatcher::CharSourceResult SIMDParMatcher::match(std::string_view const&query, CharSourceInput &&targets, Param const& params)
    {
        CharSourceResult res;
        std::sort(targets.begin(), targets.end(), [](auto const& S1, auto const& S2){ return S1->length() > S2->length(); });
        //std::ranges::sort(targets, std::greater{}, &CharSource::length);
        auto Match = [&](auto &impl)
        {
            using simd_impl_t = std::__remove_cvref_t<decltype(impl)>;
            typename simd_impl_t::Context ctx{impl};
            res.reserve(targets.size());
            for(size_t i = 0, n = targets.size(); i < n; i += simd_impl_t::kWidth)
            {
                typename simd_impl_t::input_char_src_t block;
                size_t l = (i + simd_impl_t::kWidth) < n ? simd_impl_t::kWidth : (n - i);
                for(size_t j = 0; j < l; ++j)
                    block[j] = targets[i + j];
                auto scores = ctx.sw_score_simd(query, typename simd_impl_t::input_char_src_span_t(block.begin(), block.begin() + l));
                for(size_t j = 0; j < l; ++j)
                    if (auto s = scores[i + j]; s > params.scoreThreshold)
                        res.emplace_back(targets[i + j], s);
            }
        };

        if (auto &impl = m_Impl->m_AVX2x8; impl.QueryFits(query))
            Match(impl);
        else if (auto &impl = m_Impl->m_AVX2x16; impl.QueryFits(query))
            Match(impl);
        else
            Match(m_Impl->m_AVX2x32);

        if (params.sortResults)
            std::sort(res.begin(), res.end(), [](auto const& S1, auto const& S2){ return S1.score > S2.score; });
            //std::ranges::sort(res, std::greater{}, &CharSourceResultItem::score);
        return res;
    }

    SIMDParMatcher::CharSourceResult SIMDParMatcher::match_par(std::string_view const&query, CharSourceInput &&targets, Param const& params)
    {
        CharSourceResult res;
        Impl::WorkerTypedContext<CharSourceResult,CharSourceInput> ctx;
        m_Impl->m_pWorkerContext = &ctx;
        ctx.m_pInputTargets = &targets;
        ctx.m_pResult = &res;
        ctx.m_ScoreThreshold = params.scoreThreshold;

        m_Impl->m_Query = query;
        std::sort(targets.begin(), targets.end(), [](auto const& S1, auto const& S2){ return S1->length() > S2->length(); });
        //std::ranges::sort(targets, std::greater{}, &CharSource::length);

        if (auto &impl = m_Impl->m_AVX2x8; impl.QueryFits(query))
        {
            using simd_t = decltype(Impl::m_AVX2x8);
            m_Impl->m_WorkerFunc = &Impl::WorkerFuncTpl<simd_t, simd_t::input_char_src_t, CharSourceResult, CharSourceInput>;
            m_Impl->m_pSIMDTypeErased = &impl;
            m_Impl->m_WorkerWidth = std::__remove_cvref_t<decltype(impl)>::kWidth;
        }
        else if (auto &impl = m_Impl->m_AVX2x16; impl.QueryFits(query))
        {
            using simd_t = decltype(Impl::m_AVX2x16);
            m_Impl->m_WorkerFunc = &Impl::WorkerFuncTpl<simd_t, simd_t::input_char_src_t, CharSourceResult, CharSourceInput>;
            m_Impl->m_pSIMDTypeErased = &impl;
            m_Impl->m_WorkerWidth = std::__remove_cvref_t<decltype(impl)>::kWidth;
        }else
        {
            using simd_t = decltype(Impl::m_AVX2x32);
            m_Impl->m_WorkerFunc = &Impl::WorkerFuncTpl<simd_t, simd_t::input_char_src_t, CharSourceResult, CharSourceInput>;
            m_Impl->m_pSIMDTypeErased = &m_Impl->m_AVX2x32;
            m_Impl->m_WorkerWidth = decltype(Impl::m_AVX2x32)::kWidth;
        }

        m_Impl->Do<CharSourceResult,CharSourceInput>();

        if (params.sortResults)
            std::sort(res.begin(), res.end(), [](auto const& S1, auto const& S2){ return S1.score > S2.score; });
            //std::ranges::sort(res, std::greater{}, &CharSourceResultItem::score);

        m_Impl->m_WorkerFunc = nullptr;
        m_Impl->m_pWorkerContext = &ctx;
        m_Impl->m_pSIMDTypeErased = nullptr;
        return res;
    }

    SIMDParMatcher::Result SIMDParMatcher::match_par(std::string_view const&query, Input &&targets, Param const& params)
    {
        Result res;
        Impl::WorkerTypedContext<Result,Input> ctx;
        m_Impl->m_pWorkerContext = &ctx;
        ctx.m_pInputTargets = &targets;
        ctx.m_pResult = &res;
        ctx.m_ScoreThreshold = params.scoreThreshold;

        m_Impl->m_Query = query;
        std::sort(targets.begin(), targets.end(), [](auto const& S1, auto const& S2){ return S1.length() > S2.length(); });
        //std::ranges::sort(targets, std::greater{}, &std::string_view::length);

        if (auto &impl = m_Impl->m_AVX2x8; impl.QueryFits(query))
        {
            using simd_t = decltype(Impl::m_AVX2x8);
            m_Impl->m_WorkerFunc = &Impl::WorkerFuncTpl<simd_t, simd_t::input_t, Result, Input>;
            m_Impl->m_pSIMDTypeErased = &impl;
            m_Impl->m_WorkerWidth = std::__remove_cvref_t<decltype(impl)>::kWidth;
        }
        else if (auto &impl = m_Impl->m_AVX2x16; impl.QueryFits(query))
        {
            using simd_t = decltype(Impl::m_AVX2x16);
            m_Impl->m_WorkerFunc = &Impl::WorkerFuncTpl<simd_t, simd_t::input_t, Result, Input>;
            m_Impl->m_pSIMDTypeErased = &impl;
            m_Impl->m_WorkerWidth = std::__remove_cvref_t<decltype(impl)>::kWidth;
        }else
        {
            using simd_t = decltype(Impl::m_AVX2x32);
            m_Impl->m_WorkerFunc = &Impl::WorkerFuncTpl<simd_t, simd_t::input_t, Result, Input>;
            m_Impl->m_pSIMDTypeErased = &m_Impl->m_AVX2x32;
            m_Impl->m_WorkerWidth = decltype(Impl::m_AVX2x32)::kWidth;
        }

        m_Impl->Do<Result,Input>();

        if (params.sortResults)
            std::sort(res.begin(), res.end(), [](auto const& S1, auto const& S2){ return S1.score > S2.score; });
            //std::ranges::sort(res, std::greater{}, &ResultItem::score);

        m_Impl->m_WorkerFunc = nullptr;
        m_Impl->m_pWorkerContext = &ctx;
        m_Impl->m_pSIMDTypeErased = nullptr;
        return res;
    }

    void SIMDParMatcher::SetupThreads(int threadCount)
    {
        m_Impl->SetupThreads(threadCount);
    }

    void SIMDParMatcher::StopThreads()
    {
        m_Impl->StopThreads();
    }


    /**********************************************************************/
    /* SIMDParMatcher::Impl                                               */
    /**********************************************************************/
    SIMDParMatcher::Impl::~Impl()
    {
        StopThreads();
    }

    void SIMDParMatcher::Impl::StopThreads()
    {
        if (m_Barrier)
        {
            m_WorkerCancel = true;
            m_Barrier.arrive_and_wait();
            m_WorkerThreads.resize(0);//threads will be joined here
            m_WorkerCancel = false;
            m_Barrier.change_group_size(0);
        }
    }

    void SIMDParMatcher::Impl::SetupThreads(int threadCount)
    {
        StopThreads();

        m_Barrier.change_group_size(threadCount + 1);
        for(int i = 0; i < threadCount; ++i)
            m_WorkerThreads.emplace_back(&Impl::WorkerFunc, this, i);
    }


    template<class Result, class Input>
    void SIMDParMatcher::Impl::Do()
    {
        WorkerTypedContext<Result,Input> &ctx = *static_cast<WorkerTypedContext<Result,Input>*>(m_pWorkerContext);
        size_t nThreads = m_WorkerThreads.size();
        bool workerResultsWereEmpty = ctx.m_WorkerResults.empty();
        ctx.m_WorkerResults.resize(nThreads);
        m_WorkerChunkOffset.store(nThreads * m_WorkerWidth, std::memory_order_relaxed);
        if (!workerResultsWereEmpty)
            for(auto &par : ctx.m_WorkerResults) par.clear();

        //this releases worker threads to work
        m_Barrier.arrive_and_wait();

        //now we wait
        m_Barrier.arrive_and_wait();

        for(auto &par : ctx.m_WorkerResults)
            for(auto &l : par) ctx.m_pResult->push_back(l);
    }

    template<class simd_t, class input_t, class Result, class Input>
    void SIMDParMatcher::Impl::WorkerFuncTpl(int workerId, void *pSIMD)
    {
        simd_t &simd = *reinterpret_cast<simd_t*>(pSIMD);
        typename simd_t::Context ctx_simd{simd};
        WorkerTypedContext<Result,Input> &ctx = *static_cast<WorkerTypedContext<Result,Input>*>(m_pWorkerContext);
        auto &lines = *ctx.m_pInputTargets;
        auto &results = ctx.m_WorkerResults[workerId];
        size_t i = workerId * m_WorkerWidth;
        size_t n = lines.size();
        auto beg = lines.begin();
        while(i < n)
        {
            size_t m = (i + m_WorkerWidth) < n ? i + m_WorkerWidth : n;
            auto *pFrom = &lines[i];
            auto *pTo = &*beg + m;
            input_t in{};
            std::copy(pFrom, pTo, in.begin());
            auto sp = typename simd_t::template span_for<input_t>::type{in.begin(), in.begin() + (m - i)};
            auto scores = ctx_simd.sw_score_simd(m_Query, sp);
            for(int j = 0, tn = m - i; j < tn; ++j)
            {
                if (scores[j] > ctx.m_ScoreThreshold)
                    results.emplace_back(lines[i + j], scores[j]);
            }
            i = m_WorkerChunkOffset.fetch_add(m_WorkerWidth, std::memory_order_relaxed);
        }
    }

    void SIMDParMatcher::Impl::WorkerFunc(int workerId)
    {
        while(true)
        {
            m_Barrier.arrive_and_wait();
            if (m_WorkerCancel)
                break;
            //...useful work
            (this->*m_WorkerFunc)(workerId, m_pSIMDTypeErased);
            m_Barrier.arrive_and_wait();
        }
    }
    } // namespace fuzzy_sw
