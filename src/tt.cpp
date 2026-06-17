#include "tt.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <new>
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

}

struct TTContext {
    std::vector<TTBucket> table;
    uint64_t mask = 0;
    uint8_t gen = 1;
    int hash_mb = 0;
};

namespace {

    static std::atomic<int> g_configured_hash_mb{ DEFAULT_HASH_MB };
    static TTContext global_context;
    static thread_local TTContext* active_context = nullptr;

    static inline TTContext& current_context()
    {
        return active_context ? *active_context : global_context;
    }


    static inline int bound_bonus(TTFlag flag) {
        if (flag == TT_EXACT) return 256;
        if (flag == TT_BETA)  return 128;
        return 0;
    }

    static inline int entry_score(const TTContext& ctx, const TTEntry& e) {
        const int age = (int(ctx.gen) - int(e.gen)) & 0xFF;
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

    static int clamp_hash_mb(int mb) {
        return std::max(1, std::min(mb, MAX_HASH_MB));
    }

    static void clear_table_contents(TTContext& ctx) {
        for (TTBucket& b : ctx.table) {
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

    static bool resize_context(TTContext& ctx, int mb) {
        const int clampedMb = clamp_hash_mb(mb);
        const size_t buckets = buckets_for_mb(clampedMb);

        try {
            std::vector<TTBucket> newTable;
            newTable.resize(buckets);

            ctx.table.swap(newTable);
            ctx.mask = uint64_t(buckets - 1);
            ctx.hash_mb = clampedMb;
            ctx.gen = 1;

            clear_table_contents(ctx);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    static void ensure_global_table() {
        const int configuredMb = clamp_hash_mb(g_configured_hash_mb.load(std::memory_order_relaxed));
        if (!global_context.table.empty() && global_context.hash_mb == configuredMb)
            return;

        if (!resize_context(global_context, configuredMb)) {
            global_context.table.resize(1);
            global_context.mask = 0;
            global_context.hash_mb = 1;
            global_context.gen = 1;
            clear_table_contents(global_context);
        }
    }

    static void ensure_context(TTContext& ctx) {
        if (&ctx == &global_context) {
            ensure_global_table();
            return;
        }

        if (!ctx.table.empty())
            return;

        if (!resize_context(ctx, ctx.hash_mb > 0 ? ctx.hash_mb : DEFAULT_HASH_MB)) {
            ctx.table.resize(1);
            ctx.mask = 0;
            ctx.hash_mb = 1;
            ctx.gen = 1;
            clear_table_contents(ctx);
        }
    }

    static inline TTEntry* pick_replacement(TTContext& ctx, TTBucket& b, uint64_t key) {
        for (int w = 0; w < TT_WAYS; ++w)
            if (b.e[w].key == key)
                return &b.e[w];

        for (int w = 0; w < TT_WAYS; ++w)
            if (b.e[w].key == 0)
                return &b.e[w];

        TTEntry* worst = &b.e[0];
        int worstScore = entry_score(ctx, *worst);

        for (int w = 1; w < TT_WAYS; ++w) {
            const int s = entry_score(ctx, b.e[w]);
            if (s < worstScore) {
                worstScore = s;
                worst = &b.e[w];
            }
        }

        return worst;
    }

} // namespace

bool tt_resize_mb(int mb) {
    const int clampedMb = clamp_hash_mb(mb);
    TTContext& ctx = current_context();
    if (!resize_context(ctx, clampedMb))
        return false;

    if (&ctx == &global_context)
        g_configured_hash_mb.store(clampedMb, std::memory_order_relaxed);
    return true;
}

int tt_hash_mb() {
    TTContext& ctx = current_context();
    if (&ctx != &global_context && ctx.hash_mb > 0)
        return ctx.hash_mb;

    return clamp_hash_mb(g_configured_hash_mb.load(std::memory_order_relaxed));
}

void tt_new_search() {
    TTContext& ctx = current_context();
    ensure_context(ctx);

    ctx.gen = uint8_t(ctx.gen + 1);
    if (ctx.gen == 0)
        ctx.gen = 1;
}

void tt_clear() {
    TTContext& ctx = current_context();
    ensure_context(ctx);

    clear_table_contents(ctx);
}

TTEntry* tt_probe(uint64_t key) {
    TTContext& ctx = current_context();
    if (ctx.table.empty())
        return nullptr;

    TTBucket& b = ctx.table[size_t(key & ctx.mask)];
    for (int w = 0; w < TT_WAYS; ++w)
        if (b.e[w].key == key)
            return &b.e[w];

    return nullptr;
}

void tt_prefetch(uint64_t key) {
    TTContext& ctx = current_context();
    if (ctx.table.empty())
        return;

    const TTBucket& b = ctx.table[size_t(key & ctx.mask)];
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(reinterpret_cast<const char*>(&b), _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(&b, 0, 3);
#else
    (void)b;
#endif
}

int tt_hashfull() {
    TTContext& ctx = current_context();
    ensure_context(ctx);

    const size_t buckets = std::min<size_t>(1024, ctx.table.size());
    if (buckets == 0)
        return 0;

    int used = 0;
    const uint8_t current_gen = ctx.gen;
    for (size_t i = 0; i < buckets; ++i)
    {
        for (int w = 0; w < TT_WAYS; ++w)
            if (ctx.table[i].e[w].key != 0
                && ctx.table[i].e[w].gen == current_gen)
                ++used;
    }

    return int((used * 1000ULL) / (buckets * TT_WAYS));
}

void tt_store(uint64_t key, int depth, int score, TTFlag flag, Move best_move, int static_eval) {
    TTContext& ctx = current_context();
    if (ctx.table.empty())
        return;

    const uint8_t current_gen = ctx.gen;
    TTBucket& b = ctx.table[size_t(key & ctx.mask)];
    TTEntry* e = pick_replacement(ctx, b, key);

    if (e->key != key && e->key != 0) {
        if (incoming_score(depth, flag) < entry_score(ctx, *e))
            return;
    }

    if (e->key == key) {
        if (!best_move)
            best_move = e->best_move;

        if (depth + 2 < e->depth && e->flag == TT_EXACT && flag != TT_EXACT) {
            e->gen = current_gen;
            return;
        }
    }

    e->depth = depth;
    e->score = score;
    e->flag = flag;
    e->best_move = best_move;
    e->static_eval = static_eval;
    e->gen = current_gen;
    e->key = key;
}

TTContext* tt_create_context(int mb)
{
    TTContext* context = new (std::nothrow) TTContext();
    if (!context)
        return nullptr;

    if (!resize_context(*context, mb)) {
        delete context;
        return nullptr;
    }

    return context;
}

void tt_destroy_context(TTContext* context)
{
    delete context;
}

void tt_bind_context(TTContext* context)
{
    active_context = context;
}

TTContext* tt_current_context()
{
    return active_context;
}
