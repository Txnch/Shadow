#pragma once
#include <cstdint>



constexpr int MAX_MOVES = 256;
static const int MAX_PLY = 256;




enum Color : int { WHITE, BLACK, COLOR_NB };
inline constexpr Color operator~(Color c) { return Color(c ^ 1); }

enum PieceType : int {
    NO_PIECE_TYPE,
    PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING,
    PIECE_TYPE_NB
};

enum Piece : int {
    NO_PIECE,
    W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
    B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    PIECE_NB
};

inline constexpr Color piece_color(Piece p) {
    if (p >= B_PAWN) return BLACK;
    if (p >= W_PAWN) return WHITE;
    return COLOR_NB;
}

inline constexpr PieceType piece_type(Piece p) {
    return (p == NO_PIECE) ? NO_PIECE_TYPE : PieceType((p - 1) % 6 + 1);
}

inline constexpr Piece make_piece(Color c, PieceType pt) {
    if (pt == NO_PIECE_TYPE) return NO_PIECE;
    return Piece((c == WHITE ? 0 : 6) + pt);
}


enum Square : int {
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NONE,
    SQUARE_NB
};

enum File : int { FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H, FILE_NB };
enum Rank : int { RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8, RANK_NB };

inline constexpr File file_of(Square s) { return File(int(s) & 7); }
inline constexpr Rank rank_of(Square s) { return Rank(int(s) >> 3); }


enum CastlingRight : int {
    NO_CASTLING = 0,
    WHITE_OO = 1 << 0,
    WHITE_OOO = 1 << 1,
    BLACK_OO = 1 << 2,
    BLACK_OOO = 1 << 3
};



using Move = uint32_t;

enum MoveFlag : uint32_t {
    MOVE_NONE = 0,
    MOVE_CAPTURE = 1 << 0,
    MOVE_DOUBLE_PUSH = 1 << 1,
    MOVE_EN_PASSANT = 1 << 2,
    MOVE_CASTLING = 1 << 3,
    MOVE_PROMOTION = 1 << 4
};

constexpr int FROM_SHIFT = 0;
constexpr int TO_SHIFT = 6;
constexpr int PROMO_SHIFT = 12;
constexpr int FLAG_SHIFT = 16;

inline constexpr Move make_move(
    Square from,
    Square to,
    MoveFlag flags = MOVE_NONE,
    PieceType promo = NO_PIECE_TYPE
) {
    return (Move(from) << FROM_SHIFT)
        | (Move(to) << TO_SHIFT)
        | (Move(promo) << PROMO_SHIFT)
        | (Move(flags) << FLAG_SHIFT);
}

inline constexpr Move make_promotion(Square from, Square to, PieceType promo, bool capture = false) {
    return make_move(from, to,
        MoveFlag(MOVE_PROMOTION | (capture ? MOVE_CAPTURE : MOVE_NONE)),
        promo
    );
}

inline constexpr Move make_castling(Square from, Square to) {
    return make_move(from, to, MOVE_CASTLING);
}

inline constexpr Move make_en_passant(Square from, Square to) {
    return make_move(from, to, MoveFlag(MOVE_EN_PASSANT | MOVE_CAPTURE));
}



inline constexpr Square from_sq(Move m) {
    return Square((m >> FROM_SHIFT) & 0x3F);
}

inline constexpr Square to_sq(Move m) {
    return Square((m >> TO_SHIFT) & 0x3F);
}

inline constexpr PieceType promotion_type(Move m) {
    return PieceType((m >> PROMO_SHIFT) & 0xF);
}

inline constexpr uint32_t move_flags(Move m) {
    return (m >> FLAG_SHIFT) & 0xFF;
}

inline constexpr bool is_capture(Move m) { return move_flags(m) & MOVE_CAPTURE; }
inline constexpr bool is_double_push(Move m) { return move_flags(m) & MOVE_DOUBLE_PUSH; }
inline constexpr bool is_en_passant(Move m) { return move_flags(m) & MOVE_EN_PASSANT; }
inline constexpr bool is_castling(Move m) { return move_flags(m) & MOVE_CASTLING; }
inline constexpr bool is_promotion(Move m) { return move_flags(m) & MOVE_PROMOTION; }

