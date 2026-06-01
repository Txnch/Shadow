
#include "bitboard.h"
#include "attacks.h"
#include "position.h"
#include <cassert>
#include <cstdlib>


Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
Bitboard KnightAttacks[SQUARE_NB];
Bitboard KingAttacks[SQUARE_NB];



void init_attacks() {


    for (int c = 0; c < COLOR_NB; ++c)
        for (int s = 0; s < SQUARE_NB; ++s)
            PawnAttacks[c][s] = BB_EMPTY;

    for (int s = 0; s < SQUARE_NB; ++s) {
        KnightAttacks[s] = BB_EMPTY;
        KingAttacks[s] = BB_EMPTY;
    }

    for (int s = 0; s < 64; ++s) {
        Square sq = Square(s);
        Bitboard b = square_bb(sq);

        PawnAttacks[WHITE][s] =
            north_east(b) |
            north_west(b);

        PawnAttacks[BLACK][s] =
            south_east(b) |
            south_west(b);
    }


    for (int s = 0; s < 64; ++s)
    {
        Bitboard a = 0ULL;

        int r = s / 8;
        int f = s % 8;

        auto add = [&](int dr, int df)
            {
                int rr = r + dr;
                int ff = f + df;

                if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8)
                    a |= 1ULL << (rr * 8 + ff);
            };

        add(2, 1);
        add(2, -1);
        add(-2, 1);
        add(-2, -1);
        add(1, 2);
        add(1, -2);
        add(-1, 2);
        add(-1, -2);

        KnightAttacks[s] = a;
    }


    for (int s = 0; s < 64; ++s) {
        Square sq = Square(s);
        Bitboard b = square_bb(sq);

        Bitboard a = BB_EMPTY;

        a |= north(b);
        a |= south(b);
        a |= east(b);
        a |= west(b);

        a |= north_east(b);
        a |= north_west(b);
        a |= south_east(b);
        a |= south_west(b);

        KingAttacks[s] = a;
    }
}





bool is_square_attacked(const Position& pos, Square sq, Color by) {


    if (PawnAttacks[~by][sq] &
        (pos.pieces(PAWN) & pos.pieces(by)))
        return true;


    if (KnightAttacks[sq] &
        pos.pieces(KNIGHT) &
        pos.pieces(by))
        return true;


    if (KingAttacks[sq] &
        pos.pieces(KING) &
        pos.pieces(by))
        return true;

    Bitboard occ = pos.pieces(WHITE) | pos.pieces(BLACK);

    Bitboard bq =
        (pos.pieces(BISHOP) | pos.pieces(QUEEN)) &
        pos.pieces(by);

    if (get_bishop_attacks(sq, occ) & bq)
        return true;

    Bitboard rq =
        (pos.pieces(ROOK) | pos.pieces(QUEEN)) &
        pos.pieces(by);

    if (get_rook_attacks(sq, occ) & rq)
        return true;

    return false;
}


Bitboard attackers_to(const Position& pos,
    Square sq,
    Bitboard occ,
    Color c)
{
    Bitboard result = BB_EMPTY;
    Bitboard colorBB = pos.pieces(c);


    if (c == WHITE)
        result |= PawnAttacks[BLACK][sq] &
        (colorBB & pos.pieces(PAWN));
    else
        result |= PawnAttacks[WHITE][sq] &
        (colorBB & pos.pieces(PAWN));


    result |= KnightAttacks[sq] &
        (colorBB & pos.pieces(KNIGHT));


    result |= KingAttacks[sq] &
        (colorBB & pos.pieces(KING));


    Bitboard bishopsQueens =
        (colorBB & pos.pieces(BISHOP)) |
        (colorBB & pos.pieces(QUEEN));

    result |= get_bishop_attacks(sq, occ) & bishopsQueens;

    Bitboard rooksQueens =
        (colorBB & pos.pieces(ROOK)) |
        (colorBB & pos.pieces(QUEEN));

    result |= get_rook_attacks(sq, occ) & rooksQueens;

    return result;
}

bool in_check(const Position& pos, Color c) {

    if (c == pos.side_to_move())
        return pos.checkers() != BB_EMPTY;

    Bitboard kbb = pos.pieces(KING) & pos.pieces(c);

    if (!kbb)
        return false;

    Square ksq = lsb(kbb);
    return is_square_attacked(pos, ksq, ~c);
}

Bitboard attacks_from(const Position& pos, Square sq)
{
    Piece p = pos.piece_on(sq);
    if (p == NO_PIECE)
        return BB_EMPTY;

    Color c = piece_color(p);
    PieceType pt = piece_type(p);

    Bitboard occ = pos.pieces(WHITE) | pos.pieces(BLACK);

    switch (pt)
    {
    case PAWN:
        return PawnAttacks[c][sq];

    case KNIGHT:
        return KnightAttacks[sq];

    case KING:
        return KingAttacks[sq];

    case BISHOP:
        return get_bishop_attacks(sq, occ);

    case ROOK:
        return get_rook_attacks(sq, occ);

    case QUEEN:
        return get_bishop_attacks(sq, occ) |
            get_rook_attacks(sq, occ);

    default:
        return BB_EMPTY;
    }
}




