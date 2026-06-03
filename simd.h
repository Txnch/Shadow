#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm> 

#if defined(__AVX2__) || defined(_M_AVX2)
#include <immintrin.h>
#endif

namespace shadow_simd {

    static inline void copy_i16(int16_t* dst, const int16_t* src, std::size_t n) {
#if defined(__AVX2__) || defined(_M_AVX2)
        std::size_t i = 0;
        const std::size_t n16 = n & ~std::size_t(15);
        for (; i < n16; i += 16) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), v);
        }

        for (; i < n; ++i)
            dst[i] = src[i];
#else
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = src[i];
#endif
    }

    static inline void add_i16(int16_t* dst, const int16_t* src, std::size_t n) {
#if defined(__AVX2__) || defined(_M_AVX2)
        std::size_t i = 0;
        const std::size_t n16 = n & ~std::size_t(15);
        for (; i < n16; i += 16) {
            const __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
            const __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_add_epi16(d, s));
        }

        for (; i < n; ++i)
            dst[i] = static_cast<int16_t>(dst[i] + src[i]);
#else
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = static_cast<int16_t>(dst[i] + src[i]);
#endif
    }

    static inline void sub_i16(int16_t* dst, const int16_t* src, std::size_t n) {
#if defined(__AVX2__) || defined(_M_AVX2)
        std::size_t i = 0;
        const std::size_t n16 = n & ~std::size_t(15);
        for (; i < n16; i += 16) {
            const __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
            const __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_sub_epi16(d, s));
        }

        for (; i < n; ++i)
            dst[i] = static_cast<int16_t>(dst[i] - src[i]);
#else
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = static_cast<int16_t>(dst[i] - src[i]);
#endif
    }

    static inline void add_sub_i16(int16_t* dst, const int16_t* add, const int16_t* sub, std::size_t n) {
#if defined(__AVX2__) || defined(_M_AVX2)
        std::size_t i = 0;
        const std::size_t n16 = n & ~std::size_t(15);
        for (; i < n16; i += 16) {
            __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
            const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(add + i));
            const __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(sub + i));
            d = _mm256_add_epi16(d, a);
            d = _mm256_sub_epi16(d, s);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), d);
        }

        for (; i < n; ++i)
            dst[i] = static_cast<int16_t>(dst[i] + add[i] - sub[i]);
#else
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = static_cast<int16_t>(dst[i] + add[i] - sub[i]);
#endif
    }

    static inline int64_t dot_screlu_i16(const int16_t* acc, const int16_t* weights, std::size_t n) {
#if defined(__AVX2__) || defined(_M_AVX2)
        std::size_t i = 0;

        __m256i sum_0 = _mm256_setzero_si256();
        __m256i sum_1 = _mm256_setzero_si256();
        __m256i sum_2 = _mm256_setzero_si256();
        __m256i sum_3 = _mm256_setzero_si256();

        const __m256i zero = _mm256_setzero_si256();
        const __m256i max_val = _mm256_set1_epi16(255);

        const std::size_t n16 = n & ~std::size_t(15);

        for (; i < n16; i += 16) {
            __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + i));
            a = _mm256_max_epi16(a, zero);
            a = _mm256_min_epi16(a, max_val);

            __m256i a_sq = _mm256_mullo_epi16(a, a);

            __m256i w = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i));

            __m128i a_sq_lo128 = _mm256_castsi256_si128(a_sq);
            __m128i a_sq_hi128 = _mm256_extracti128_si256(a_sq, 1);
            __m128i w_lo128 = _mm256_castsi256_si128(w);
            __m128i w_hi128 = _mm256_extracti128_si256(w, 1);

            __m256i a_sq32_lo = _mm256_cvtepu16_epi32(a_sq_lo128);
            __m256i a_sq32_hi = _mm256_cvtepu16_epi32(a_sq_hi128);

            __m256i w32_lo = _mm256_cvtepi16_epi32(w_lo128);
            __m256i w32_hi = _mm256_cvtepi16_epi32(w_hi128);

            __m256i p32_lo = _mm256_mullo_epi32(a_sq32_lo, w32_lo);
            __m256i p32_hi = _mm256_mullo_epi32(a_sq32_hi, w32_hi);

            __m256i p64_0 = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(p32_lo));
            __m256i p64_1 = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(p32_lo, 1));
            __m256i p64_2 = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(p32_hi));
            __m256i p64_3 = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(p32_hi, 1));

            sum_0 = _mm256_add_epi64(sum_0, p64_0);
            sum_1 = _mm256_add_epi64(sum_1, p64_1);
            sum_2 = _mm256_add_epi64(sum_2, p64_2);
            sum_3 = _mm256_add_epi64(sum_3, p64_3);
        }

        sum_0 = _mm256_add_epi64(sum_0, sum_1);
        sum_2 = _mm256_add_epi64(sum_2, sum_3);
        sum_0 = _mm256_add_epi64(sum_0, sum_2);

        alignas(32) int64_t buffer[4];
        _mm256_store_si256(reinterpret_cast<__m256i*>(buffer), sum_0);
        int64_t out = buffer[0] + buffer[1] + buffer[2] + buffer[3];

        for (; i < n; ++i) {
            int16_t val = std::max<int16_t>(0, std::min<int16_t>(acc[i], 255));
            int32_t sq = static_cast<int32_t>(val) * val;
            out += static_cast<int64_t>(sq) * weights[i];
        }

        return out;
#else
        int64_t out = 0;
        for (std::size_t i = 0; i < n; ++i) {
            int16_t val = std::max<int16_t>(0, std::min<int16_t>(acc[i], 255));
            int32_t sq = static_cast<int32_t>(val) * val;
            out += static_cast<int64_t>(sq) * weights[i];
        }
        return out;
#endif
    }

} // namespace shadow_simd