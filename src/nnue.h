#pragma once

#include "move.h"
#include <cstdint>
#include <string>

class Position;

namespace nnue {

    static constexpr int KING_BUCKETS = 4;
    static constexpr int INPUTS = 768 * KING_BUCKETS;
    static constexpr int HIDDEN = 1024;
    static constexpr int OUTPUT_BUCKETS = 8;
    static constexpr uint32_t MAGIC = 0x45554E4Eu;
    static constexpr uint32_t VERSION_V6_BUCKETED = 6;
    static constexpr int QA = 255;
    static constexpr int QB = 64;
    static constexpr int BUCKET_DIVISOR = 4;
    static constexpr int NETWORK_SCALE = 317;

    static constexpr int KingBuckets[32] = {
        0, 0, 1, 1,
        2, 2, 2, 2,
        3, 3, 3, 3,
        3, 3, 3, 3,
        3, 3, 3, 3,
        3, 3, 3, 3,
        3, 3, 3, 3,
        3, 3, 3, 3
    };

    struct AccumulatorPair {
        alignas(64) int16_t white[HIDDEN]{};
        alignas(64) int16_t black[HIDDEN]{};
    };

    struct DirtyPiece {
        Piece pc = NO_PIECE;
        Square sq = SQ_NONE;
    };

    struct DirtyPieces {
        DirtyPiece sub0{};
        DirtyPiece add0{};
        DirtyPiece sub1{};
        DirtyPiece add1{};
    };

    bool init(const std::string& filepath);
    bool is_ready();
    bool loaded_from_embedded();

    int evaluate(const Position& pos);
    int evaluate_from_pair(const AccumulatorPair& pair, const Position& pos);
    int evaluate_without_piece(const AccumulatorPair& base_pair, const Position& pos, Square sq);
    int get_king_bucket(Color pov, Square ksq);

    void refresh_pair(const Position& pos, AccumulatorPair& pair);
    void refresh_acc(const Position& pos, Color pov, int16_t out_acc[HIDDEN]);
    int feature_index_stm_manual(Color pov, Square ksq, Piece pc, Square sq);

    void add_feature(int16_t acc[HIDDEN], int feat_idx);
    void sub_feature(int16_t acc[HIDDEN], int feat_idx);
    void add_sub_feature(int16_t acc[HIDDEN], int add_feat_idx, int sub_feat_idx);
    void apply_dirty(int16_t acc[HIDDEN],
        Color pov,
        Square ksq,
        const DirtyPieces& dp);
    void clear_finny_table();
    void refresh_from_finny(const Position& pos, Color pov, int16_t out_acc[HIDDEN]);

}