#pragma once
#include "bitboard.h"
#include "move.h"
#include "nnue.h"

#include <array>
#include <cstdint>
#include <string>

struct Undo {
    Move move = 0;
    Piece captured = NO_PIECE;
    Square captured_sq = SQ_NONE;
    Square ep_square = SQ_NONE;
    int castling_rights = 0;
    uint64_t hash_key = 0;
    uint64_t pawn_key = 0;
    uint64_t non_pawn_key[COLOR_NB]{};
    int halfmove_clock = 0;
    int fullmove_number = 1;
    int previous_null_ply = -1;
    bool is_null = false;
    nnue::DirtyPieces dp{};
};

class Position {
public:
    Position();
    Position(const Position& other);
    Position& operator=(const Position& other) = default;

    void clear();
    void set_fen(const std::string& fen);

    bool make_move(Move m, bool assume_pseudo_legal = false);
    void undo_move();
    void do_null_move();
    void undo_null_move();

    Bitboard pieces(Color c) const;
    Bitboard pieces(PieceType pt) const;
    Bitboard all_pieces() const { return occ_all; }
    Piece piece_on(Square s) const;
    Square king_square(Color c) const;
    bool gives_check(Move m) const;
    bool is_legal(Move m) const;
    bool is_pseudo_legal(Move m) const;
    bool is_repetition_draw(int ply_from_root) const;
    Color side_to_move() const { return side; }
    Square ep_square()   const { return ep; }
    int castling()       const { return castling_right; }
    int halfmove_clock() const { return halfmove_clock_state; }
    int fullmove_number() const { return fullmove_number_state; }
    uint64_t hash() const { return hash_key; }
    uint64_t pawn_key() const { return pawn_hash_key; }
    uint64_t non_pawn_key(Color c) const { return non_pawn_hash_key[c]; }

    int current_ply() const { return ply; }
    const Undo& state_at_ply(int p) const { return history[p]; }

    Bitboard checkers() const { return (side < COLOR_NB) ? checkers_bb : BB_EMPTY; }
    Bitboard blockers_for_king(Color c) const { return blockers_for_king_bb[c]; }
    Bitboard pinners(Color c) const { return pinners_bb[c]; }
    Bitboard check_squares(PieceType pt) const {
        return (unsigned(pt) < PIECE_TYPE_NB) ? check_squares_bb[pt] : BB_EMPTY;
    }

private:
    std::array<Bitboard, PIECE_NB> piece_bb{};
    std::array<Piece, SQUARE_NB> board{};

    Bitboard occ[COLOR_NB]{};
    Bitboard occ_all = BB_EMPTY;

    Color side = WHITE;
    Square ep = SQ_NONE;
    int castling_right = NO_CASTLING;
    int halfmove_clock_state = 0;
    int fullmove_number_state = 1;

    static constexpr int MAX_GAME_PLY = 2048;
    std::array<Undo, MAX_GAME_PLY> history{};
    int ply = 0;
    int last_null_ply = -1;
    uint64_t hash_key = 0;
    uint64_t pawn_hash_key = 0;
    std::array<uint64_t, COLOR_NB> non_pawn_hash_key{ 0, 0 };

    Bitboard checkers_bb = BB_EMPTY;
    std::array<Bitboard, COLOR_NB> blockers_for_king_bb{ BB_EMPTY, BB_EMPTY };
    std::array<Bitboard, COLOR_NB> pinners_bb{ BB_EMPTY, BB_EMPTY };
    std::array<Bitboard, PIECE_TYPE_NB> check_squares_bb{};
    void refresh_check_info();
};

extern Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
extern Bitboard KnightAttacks[SQUARE_NB];
extern Bitboard KingAttacks[SQUARE_NB];

void init_attacks();
void init_zobrist();
bool is_square_attacked(const Position& pos, Square sq, Color by);
bool in_check(const Position& pos, Color c);
