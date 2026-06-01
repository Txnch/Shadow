#include "tt.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace {

    static constexpr int TT_WAYS = 4;

    static constexpr int DEFAULT_HASH_MB = 64;

    static constexpr int MAX_HASH_MB = 65536;

    struct alignas(64) TTBucket {
        TTEntry e[TT_WAYS];
    };

    static std::vector<TTBucket> table;
    static uint64_t g_mask = 0;
    static uint8_t g_gen = 1;
    static int g_hash_mb = 0;


    static inline int bound_bonus(TTFlag flag) {
        if (flag == TT_EXACT) return 256;
        if (flag == TT_BETA)  return 128;
        return 0;
    }

    static inline int entry_score(const TTEntry& e) {
        const int age = (int(g_gen) - int(e.gen)) & 0xFF;
        return e.depth * 8 + bound_bonus(e.flag) - age * 4;
    }

    static inline int incoming_score(int depth, TTFlag flag) {
        return depth * 8 + bound_bonus(flag);
    }

    static size_t floor_pow2(size_t x) {
        if (x <= 1)
            return 1;

        size_t p = 1;
        while ((p << 1) <= x)
            p <<= 1;

        return p;
    }

    static size_t buckets_for_mb(int mb) {
        const size_t bytes = size_t(std::max(1, mb)) * 1024ULL * 1024ULL;
        size_t buckets = bytes / sizeof(TTBucket);
        if (buckets < 1)
            buckets = 1;
        return floor_pow2(buckets);
    }

    static void clear_table_contents() {
        for (TTBucket& b : table) {
            for (int w = 0; w < TT_WAYS; ++w) {
                b.e[w].key = 0;
                b.e[w].depth = 0;
                b.e[w].score = 0;
                b.e[w].flag = TT_EXACT;
                b.e[w].best_move = 0;
                b.e[w].static_eval = 0;
                b.e[w].gen = 0;
            }
        }
    }

    static void ensure_table() {
        if (!table.empty())
            return;

        if (!tt_resize_mb(DEFAULT_HASH_MB)) {
            table.resize(1);
            g_mask = 0;
            g_hash_mb = 1;
            g_gen = 1;
            clear_table_contents();
        }
    }

    static inline TTEntry* pick_replacement(TTBucket& b, uint64_t key) {
        for (int w = 0; w < TT_WAYS; ++w)
            if (b.e[w].key == key)
                return &b.e[w];

        for (int w = 0; w < TT_WAYS; ++w)
            if (b.e[w].key == 0)
                return &b.e[w];

        TTEntry* worst = &b.e[0];
        int worstScore = entry_score(*worst);

        for (int w = 1; w < TT_WAYS; ++w) {
            const int s = entry_score(b.e[w]);
            if (s < worstScore) {
                worstScore = s;
                worst = &b.e[w];
            }
        }

        return worst;
    }

} // namespace

bool tt_resize_mb(int mb) {
    const int clampedMb = std::max(1, std::min(mb, MAX_HASH_MB));
    const size_t buckets = buckets_for_mb(clampedMb);

    try {
        std::vector<TTBucket> newTable;
        newTable.resize(buckets);

        table.swap(newTable);
        g_mask = uint64_t(buckets - 1);
        g_hash_mb = clampedMb;
        g_gen = 1;

        clear_table_contents();
        return true;
    }
    catch (...) {
        return false;
    }
}

int tt_hash_mb() {
    ensure_table();
    return g_hash_mb;
}

void tt_new_search() {
    ensure_table();

    g_gen = uint8_t(g_gen + 1);
    if (g_gen == 0)
        g_gen = 1;
}

void tt_clear() {
    ensure_table();
    clear_table_contents();
}

TTEntry* tt_probe(uint64_t key) {
    ensure_table();

    TTBucket& b = table[size_t(key & g_mask)];
    for (int w = 0; w < TT_WAYS; ++w)
        if (b.e[w].key == key)
            return &b.e[w];

    return nullptr;
}

void tt_prefetch(uint64_t key) {
    ensure_table();

    const TTBucket& b = table[size_t(key & g_mask)];
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(reinterpret_cast<const char*>(&b), _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(&b, 0, 3);
#else
    (void)b;
#endif
}

int tt_hashfull() {
    ensure_table();

    const size_t buckets = std::min<size_t>(1024, table.size());
    if (buckets == 0)
        return 0;

    int used = 0;
    for (size_t i = 0; i < buckets; ++i)
        for (int w = 0; w < TT_WAYS; ++w)
            if (table[i].e[w].key != 0 && table[i].e[w].gen == g_gen)
                ++used;

    return int((used * 1000ULL) / (buckets * TT_WAYS));
}

void tt_store(uint64_t key, int depth, int score, TTFlag flag, Move best_move, int static_eval) {
    ensure_table();

    TTBucket& b = table[size_t(key & g_mask)];
    TTEntry* e = pick_replacement(b, key);

    if (e->key != key && e->key != 0) {
        if (incoming_score(depth, flag) < entry_score(*e))
            return;
    }

    if (e->key == key) {
        if (!best_move)
            best_move = e->best_move;

        if (depth + 2 < e->depth && e->flag == TT_EXACT && flag != TT_EXACT) {
            e->gen = g_gen;
            return;
        }
    }

    e->key = key;
    e->depth = depth;
    e->score = score;
    e->flag = flag;
    e->best_move = best_move;
    e->static_eval = static_eval;
    e->gen = g_gen;
}
