#define _CRT_SECURE_NO_WARNINGS
#include "nnue.h"

#include "position.h"
#include "bitboard.h"
#include "simd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace nnue {

    static constexpr uint32_t MAGIC = 0x45554E4Eu;
    static constexpr uint32_t VERSION_V5_BASIC = 5;
    static constexpr int QA = 255;
    static constexpr int QB = 64;
    static constexpr float NETWORK_SCALE = 400.0f;

    static bool g_ready = false;

    static std::vector<int16_t> g_W1;
    static std::vector<int16_t> g_b1;
    static std::vector<int16_t> g_W2;
    static float g_b2 = 0.0f;
    static int g_b2_scaled = 0;

    static inline bool read_all(FILE* f, void* dst, size_t bytes) {
        return std::fread(dst, 1, bytes, f) == bytes;
    }

    static std::filesystem::path executable_dir() {
#if defined(_WIN32)
        std::wstring buffer(MAX_PATH, L'\0');
        while (true) {
            const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (len == 0)
                return {};
            if (len < buffer.size()) {
                buffer.resize(len);
                return std::filesystem::path(buffer).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
#elif defined(__linux__)
        std::array<char, 4096> buffer{};
        const ssize_t len = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (len <= 0)
            return {};
        buffer[size_t(len)] = '\0';
        return std::filesystem::path(buffer.data()).parent_path();
#else
        return {};
#endif
    }

    static FILE* fopen_read_binary(const std::filesystem::path& path) {
#if defined(_WIN32)
        return _wfopen(path.c_str(), L"rb");
#else
        return std::fopen(path.string().c_str(), "rb");
#endif
    }

    static inline uint32_t read_u32(FILE* f, bool& ok) {
        uint32_t x = 0;
        ok = read_all(f, &x, sizeof(x));
        return x;
    }

    static inline int piece_type_index(PieceType pt) {
        if (pt < PAWN || pt > KING) return -1;
        return static_cast<int>(pt) - static_cast<int>(PAWN);
    }

    static inline Square orient_square(Color pov, Square sq) {
        if (pov == BLACK)
            sq = Square(int(sq) ^ 56);
        return sq;
    }

    static inline int feature_index_stm(Color pov, Piece pc, Square sq) {
        if (pc == NO_PIECE || sq < SQ_A1 || sq > SQ_H8)
            return -1;

        sq = orient_square(pov, sq);

        const int ptIdx = piece_type_index(piece_type(pc));
        if (ptIdx < 0)
            return -1;

        const int colorIdx = (piece_color(pc) == pov) ? 0 : 1;
        const int plane = colorIdx * 6 + ptIdx;
        return plane * 64 + int(sq);
    }

    static inline const int16_t* w1_row(int feat_idx) {
        return &g_W1[size_t(feat_idx) * HIDDEN];
    }

    static int evaluate_from_accs(const int16_t acc_stm[HIDDEN],
        const int16_t acc_ntm[HIDDEN]) {
        int64_t sum = shadow_simd::dot_screlu_i16(acc_stm, g_W2.data(), HIDDEN);
        sum += shadow_simd::dot_screlu_i16(acc_ntm, g_W2.data() + HIDDEN, HIDDEN);

        const int64_t scale_num = static_cast<int64_t>(NETWORK_SCALE);
        const int64_t scale_den = static_cast<int64_t>(QA) * QA * QB;

        const int out = g_b2_scaled + static_cast<int>((sum * scale_num) / scale_den);
        return out;
    }

    int evaluate_from_pair(const AccumulatorPair& pair, Color stm) {
        if (!g_ready)
            return 0;

        return stm == WHITE
            ? evaluate_from_accs(pair.white, pair.black)
            : evaluate_from_accs(pair.black, pair.white);
    }

    bool init(const std::string& filepath) {
        g_ready = false;
        g_W1.clear();
        g_b1.clear();
        g_W2.clear();
        g_b2 = 0.0f;
        g_b2_scaled = 0;

        const std::filesystem::path requestedPath(filepath);

        FILE* f = fopen_read_binary(requestedPath);
        if (!f && requestedPath.is_relative()) {
            const std::filesystem::path fallbackPath = executable_dir() / requestedPath;
            f = fopen_read_binary(fallbackPath);
        }
        if (!f)
            return false;

        bool ok = true;
        const uint32_t magic = read_u32(f, ok);
        const uint32_t version = read_u32(f, ok);
        const uint32_t inputs = read_u32(f, ok);
        const uint32_t hidden = read_u32(f, ok);
        const uint32_t qa = read_u32(f, ok);
        const uint32_t qb = read_u32(f, ok);

        if (!ok || magic != MAGIC || version != VERSION_V5_BASIC ||
            inputs != INPUTS || hidden != HIDDEN || qa != QA || qb != QB) {
            std::fclose(f);
            return false;
        }

        g_W1.assign(INPUTS * HIDDEN, 0);
        g_b1.assign(HIDDEN, 0);
        g_W2.assign(2 * HIDDEN, 0);

        if (!read_all(f, g_W1.data(), g_W1.size() * sizeof(int16_t)) ||
            !read_all(f, g_b1.data(), g_b1.size() * sizeof(int16_t)) ||
            !read_all(f, g_W2.data(), g_W2.size() * sizeof(int16_t)) ||
            !read_all(f, &g_b2, sizeof(g_b2))) {
            g_W1.clear();
            g_b1.clear();
            g_W2.clear();
            g_b2 = 0.0f;
            g_b2_scaled = 0;
            std::fclose(f);
            return false;
        }

        g_b2_scaled = static_cast<int>(std::round(g_b2 * NETWORK_SCALE));

        std::fclose(f);
        g_ready = true;
        return true;
    }

    bool is_ready() {
        return g_ready;
    }

    int feature_index_stm_manual(Color pov, Piece pc, Square sq) {
        return feature_index_stm(pov, pc, sq);
    }

    void refresh_acc(const Position& pos, Color pov, int16_t out_acc[HIDDEN]) {
        if (!g_ready) {
            std::fill_n(out_acc, HIDDEN, int16_t(0));
            return;
        }

        shadow_simd::copy_i16(out_acc, g_b1.data(), HIDDEN);

        Bitboard occ = pos.all_pieces();
        while (occ) {
            const Square sq = pop_lsb(occ);
            const Piece pc = pos.piece_on(sq);
            const int idx = feature_index_stm(pov, pc, sq);
            if (idx >= 0)
                shadow_simd::add_i16(out_acc, w1_row(idx), HIDDEN);
        }
    }

    void add_feature(int16_t acc[HIDDEN], int feat_idx) {
        if (!g_ready || feat_idx < 0 || feat_idx >= INPUTS)
            return;
        shadow_simd::add_i16(acc, w1_row(feat_idx), HIDDEN);
    }

    void sub_feature(int16_t acc[HIDDEN], int feat_idx) {
        if (!g_ready || feat_idx < 0 || feat_idx >= INPUTS)
            return;
        shadow_simd::sub_i16(acc, w1_row(feat_idx), HIDDEN);
    }

    void add_sub_feature(int16_t acc[HIDDEN], int add_feat_idx, int sub_feat_idx) {
        if (!g_ready)
            return;

        const bool addOk = add_feat_idx >= 0 && add_feat_idx < INPUTS;
        const bool subOk = sub_feat_idx >= 0 && sub_feat_idx < INPUTS;

        if (addOk && subOk) {
            shadow_simd::add_sub_i16(acc, w1_row(add_feat_idx), w1_row(sub_feat_idx), HIDDEN);
            return;
        }

        if (addOk)
            shadow_simd::add_i16(acc, w1_row(add_feat_idx), HIDDEN);
        if (subOk)
            shadow_simd::sub_i16(acc, w1_row(sub_feat_idx), HIDDEN);
    }

    void apply_dirty(int16_t acc[HIDDEN], Color pov, const DirtyPieces& dp) {
        if (!g_ready)
            return;

        auto feat_idx = [&](const DirtyPiece& d) {
            return feature_index_stm(pov, d.pc, d.sq);
            };

        add_sub_feature(acc, feat_idx(dp.add0), feat_idx(dp.sub0));
        add_sub_feature(acc, feat_idx(dp.add1), feat_idx(dp.sub1));
    }

    int evaluate(const Position& pos) {
        if (!g_ready)
            return 0;

        AccumulatorPair pair{};
        refresh_acc(pos, WHITE, pair.white);
        refresh_acc(pos, BLACK, pair.black);
        return evaluate_from_pair(pair, pos.side_to_move());
    }

} // namespace nnue