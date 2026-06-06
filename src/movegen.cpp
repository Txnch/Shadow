#include "movegen.h"
#include <cstdlib>
#include "bitboard.h"


static inline void push_targets(Square from, Bitboard targets, Bitboard enemy, MoveList& list) {
    while (targets) {
        const Square to = pop_lsb(targets);
        if (square_bb(to) & enemy)
            list.push(make_move(from, to, MOVE_CAPTURE));
        else
            list.push(make_move(from, to));
    }
}

static void gen_pawn_moves(const Position& pos, MoveList& list) {

    Color us = pos.side_to_move();
    Color them = ~us;

    Bitboard pawns = pos.pieces(PAWN) & pos.pieces(us);
    const Bitboard occ = pos.all_pieces();

    int dir = (us == WHITE) ? 8 : -8;
    int start_rank = (us == WHITE) ? RANK_2 : RANK_7;
    int promo_rank = (us == WHITE) ? RANK_8 : RANK_1;

    while (pawns) {

        Square from = pop_lsb(pawns);
        int to_int = int(from) + dir;

        if (to_int >= 0 && to_int < 64) {

            Square to = Square(to_int);

            if (!(occ & square_bb(to))) {

                if (rank_of(to) == promo_rank) {
                    list.push(make_promotion(from, to, QUEEN));
                    list.push(make_promotion(from, to, ROOK));
                    list.push(make_promotion(from, to, BISHOP));
                    list.push(make_promotion(from, to, KNIGHT));
                }
                else {
                    list.push(make_move(from, to));

                    if (rank_of(from) == start_rank) {
                        int to2_int = to_int + dir;
                        if (to2_int >= 0 && to2_int < 64) {
                            Square to2 = Square(to2_int);
                            if (!(occ & square_bb(to2)))
                                list.push(make_move(from, to2, MOVE_DOUBLE_PUSH));
                        }
                    }
                }
            }
        }

        Bitboard caps = PawnAttacks[us][from] & pos.pieces(them);
        while (caps) {
            Square to = pop_lsb(caps);

            if (rank_of(to) == promo_rank) {
                list.push(make_promotion(from, to, QUEEN, true));
                list.push(make_promotion(from, to, ROOK, true));
                list.push(make_promotion(from, to, BISHOP, true));
                list.push(make_promotion(from, to, KNIGHT, true));
            }
            else {
                list.push(make_move(from, to, MOVE_CAPTURE));
            }
        }

        Square ep = pos.ep_square();
        if (ep != SQ_NONE) {
            if (PawnAttacks[us][from] & square_bb(ep))
                list.push(make_en_passant(from, ep));
        }
    }
}

static void gen_pawn_quiets(const Position& pos, MoveList& list) {

    const Color us = pos.side_to_move();
    const Bitboard pawns = pos.pieces(PAWN) & pos.pieces(us);
    const Bitboard occ = pos.all_pieces();
    const int dir = (us == WHITE) ? 8 : -8;
    const int start_rank = (us == WHITE) ? RANK_2 : RANK_7;
    const int promo_rank = (us == WHITE) ? RANK_8 : RANK_1;

    Bitboard bb = pawns;
    while (bb) {
        const Square from = pop_lsb(bb);
        const int to_int = int(from) + dir;
        if (to_int < 0 || to_int >= 64)
            continue;

        const Square to = Square(to_int);
        if (occ & square_bb(to))
            continue;

        if (rank_of(to) == promo_rank)
            continue;

        list.push(make_move(from, to));

        if (rank_of(from) == start_rank) {
            const int to2_int = to_int + dir;
            if (to2_int >= 0 && to2_int < 64) {
                const Square to2 = Square(to2_int);
                if (!(occ & square_bb(to2)))
                    list.push(make_move(from, to2, MOVE_DOUBLE_PUSH));
            }
        }
    }
}

static void gen_knight_moves(const Position& pos, MoveList& list) {

    Color us = pos.side_to_move();
    const Bitboard own = pos.pieces(us);
    const Bitboard enemy = pos.pieces(~us);
    Bitboard bb = pos.pieces(KNIGHT) & pos.pieces(us);

    while (bb) {
        Square from = pop_lsb(bb);
        Bitboard targets = KnightAttacks[from] & ~own;
        push_targets(from, targets, enemy, list);
    }
}

static void gen_knight_quiets(const Position& pos, MoveList& list) {

    const Color us = pos.side_to_move();
    const Bitboard occ = pos.all_pieces();
    Bitboard bb = pos.pieces(KNIGHT) & pos.pieces(us);

    while (bb) {
        const Square from = pop_lsb(bb);
        Bitboard targets = KnightAttacks[from] & ~occ;
        while (targets)
            list.push(make_move(from, pop_lsb(targets)));
    }
}

static void gen_king_moves(const Position& pos, MoveList& list) {

    Color us = pos.side_to_move();
    Color them = ~us;
    const Bitboard own = pos.pieces(us);
    const Bitboard enemy = pos.pieces(them);

    Bitboard kbb = pos.pieces(KING) & pos.pieces(us);
    if (!kbb) return;

    Square from = lsb(kbb);

    Bitboard targets = KingAttacks[from] & ~own;
    push_targets(from, targets, enemy, list);

    if (in_check(pos, us))
        return;

    if (us == WHITE) {

        if ((pos.castling() & WHITE_OO) &&
            pos.piece_on(SQ_F1) == NO_PIECE &&
            pos.piece_on(SQ_G1) == NO_PIECE &&
            !is_square_attacked(pos, SQ_F1, them) &&
            !is_square_attacked(pos, SQ_G1, them))
        {
            list.push(make_castling(SQ_E1, SQ_G1));
        }

        if ((pos.castling() & WHITE_OOO) &&
            pos.piece_on(SQ_D1) == NO_PIECE &&
            pos.piece_on(SQ_C1) == NO_PIECE &&
            pos.piece_on(SQ_B1) == NO_PIECE &&
            !is_square_attacked(pos, SQ_D1, them) &&
            !is_square_attacked(pos, SQ_C1, them))
        {
            list.push(make_castling(SQ_E1, SQ_C1));
        }
    }
    else {

        if ((pos.castling() & BLACK_OO) &&
            pos.piece_on(SQ_F8) == NO_PIECE &&
            pos.piece_on(SQ_G8) == NO_PIECE &&
            !is_square_attacked(pos, SQ_F8, them) &&
            !is_square_attacked(pos, SQ_G8, them))
        {
            list.push(make_castling(SQ_E8, SQ_G8));
        }

        if ((pos.castling() & BLACK_OOO) &&
            pos.piece_on(SQ_D8) == NO_PIECE &&
            pos.piece_on(SQ_C8) == NO_PIECE &&
            pos.piece_on(SQ_B8) == NO_PIECE &&
            !is_square_attacked(pos, SQ_D8, them) &&
            !is_square_attacked(pos, SQ_C8, them))
        {
            list.push(make_castling(SQ_E8, SQ_C8));
        }
    }
}

static void gen_king_quiets(const Position& pos, MoveList& list) {

    const Color us = pos.side_to_move();
    const Color them = ~us;
    const Bitboard occ = pos.all_pieces();

    Bitboard kbb = pos.pieces(KING) & pos.pieces(us);
    if (!kbb) return;

    const Square from = lsb(kbb);
    Bitboard targets = KingAttacks[from] & ~occ;
    while (targets)
        list.push(make_move(from, pop_lsb(targets)));

    if (in_check(pos, us))
        return;

    if (us == WHITE) {
        if ((pos.castling() & WHITE_OO) &&
            pos.piece_on(SQ_F1) == NO_PIECE &&
            pos.piece_on(SQ_G1) == NO_PIECE &&
            !is_square_attacked(pos, SQ_F1, them) &&
            !is_square_attacked(pos, SQ_G1, them))
        {
            list.push(make_castling(SQ_E1, SQ_G1));
        }

        if ((pos.castling() & WHITE_OOO) &&
            pos.piece_on(SQ_D1) == NO_PIECE &&
            pos.piece_on(SQ_C1) == NO_PIECE &&
            pos.piece_on(SQ_B1) == NO_PIECE &&
            !is_square_attacked(pos, SQ_D1, them) &&
            !is_square_attacked(pos, SQ_C1, them))
        {
            list.push(make_castling(SQ_E1, SQ_C1));
        }
    }
    else {
        if ((pos.castling() & BLACK_OO) &&
            pos.piece_on(SQ_F8) == NO_PIECE &&
            pos.piece_on(SQ_G8) == NO_PIECE &&
            !is_square_attacked(pos, SQ_F8, them) &&
            !is_square_attacked(pos, SQ_G8, them))
        {
            list.push(make_castling(SQ_E8, SQ_G8));
        }

        if ((pos.castling() & BLACK_OOO) &&
            pos.piece_on(SQ_D8) == NO_PIECE &&
            pos.piece_on(SQ_C8) == NO_PIECE &&
            pos.piece_on(SQ_B8) == NO_PIECE &&
            !is_square_attacked(pos, SQ_D8, them) &&
            !is_square_attacked(pos, SQ_C8, them))
        {
            list.push(make_castling(SQ_E8, SQ_C8));
        }
    }
}

void generate_moves(const Position& pos, MoveList& list) {

    list.clear();

    gen_pawn_moves(pos, list);
    gen_knight_moves(pos, list);

    Color us = pos.side_to_move();
    Bitboard occ = pos.pieces(WHITE) | pos.pieces(BLACK);
    Bitboard own = pos.pieces(us);
    Bitboard enemy = pos.pieces(~us);

    {
        Bitboard bb = pos.pieces(BISHOP) & pos.pieces(us);

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard targets = get_bishop_attacks(from, occ) & ~own;
            push_targets(from, targets, enemy, list);
        }
    }

    {
        Bitboard bb = pos.pieces(ROOK) & pos.pieces(us);

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard targets = get_rook_attacks(from, occ) & ~own;
            push_targets(from, targets, enemy, list);
        }
    }

    {
        Bitboard bb = pos.pieces(QUEEN) & pos.pieces(us);

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard targets = (get_bishop_attacks(from, occ) | get_rook_attacks(from, occ)) & ~own;
            push_targets(from, targets, enemy, list);
        }
    }

    gen_king_moves(pos, list);
}

void generate_quiets(const Position& pos, MoveList& list) {

    list.clear();

    gen_pawn_quiets(pos, list);
    gen_knight_quiets(pos, list);

    const Color us = pos.side_to_move();
    const Bitboard occ = pos.pieces(WHITE) | pos.pieces(BLACK);

    {
        Bitboard bb = pos.pieces(BISHOP) & pos.pieces(us);
        while (bb) {
            const Square from = pop_lsb(bb);
            Bitboard targets = get_bishop_attacks(from, occ) & ~occ;
            while (targets)
                list.push(make_move(from, pop_lsb(targets)));
        }
    }

    {
        Bitboard bb = pos.pieces(ROOK) & pos.pieces(us);
        while (bb) {
            const Square from = pop_lsb(bb);
            Bitboard targets = get_rook_attacks(from, occ) & ~occ;
            while (targets)
                list.push(make_move(from, pop_lsb(targets)));
        }
    }

    {
        Bitboard bb = pos.pieces(QUEEN) & pos.pieces(us);
        while (bb) {
            const Square from = pop_lsb(bb);
            Bitboard targets = (get_bishop_attacks(from, occ) | get_rook_attacks(from, occ)) & ~occ;
            while (targets)
                list.push(make_move(from, pop_lsb(targets)));
        }
    }

    gen_king_quiets(pos, list);
}

void generate_captures(const Position& pos, MoveList& list)
{
    list.clear();

    Color us = pos.side_to_move();
    Color them = ~us;

    Bitboard pawns = pos.pieces(PAWN) & pos.pieces(us);

    int promo_rank = (us == WHITE) ? RANK_8 : RANK_1;

    while (pawns)
    {
        Square from = pop_lsb(pawns);

        Bitboard caps = PawnAttacks[us][from] & pos.pieces(them);

        while (caps)
        {
            Square to = pop_lsb(caps);

            if (rank_of(to) == promo_rank)
            {
                list.push(make_promotion(from, to, QUEEN, true));
                list.push(make_promotion(from, to, ROOK, true));
                list.push(make_promotion(from, to, BISHOP, true));
                list.push(make_promotion(from, to, KNIGHT, true));
            }
            else
            {
                list.push(make_move(from, to, MOVE_CAPTURE));
            }
        }

        const int push = (us == WHITE) ? 8 : -8;
        const int to_int = int(from) + push;
        if (to_int >= 0 && to_int < 64)
        {
            const Square to = Square(to_int);
            if (rank_of(to) == promo_rank && !(pos.all_pieces() & square_bb(to)))
            {
                list.push(make_promotion(from, to, QUEEN));
                list.push(make_promotion(from, to, ROOK));
                list.push(make_promotion(from, to, BISHOP));
                list.push(make_promotion(from, to, KNIGHT));
            }
        }

        Square ep = pos.ep_square();
        if (ep != SQ_NONE)
        {
            if (PawnAttacks[us][from] & square_bb(ep))
                list.push(make_en_passant(from, ep));
        }
    }

    Bitboard knights = pos.pieces(KNIGHT) & pos.pieces(us);
    while (knights)
    {
        Square from = pop_lsb(knights);
        Bitboard attacks = KnightAttacks[from] & pos.pieces(them);

        while (attacks)
        {
            Square to = pop_lsb(attacks);
            list.push(make_move(from, to, MOVE_CAPTURE));
        }
    }

    Bitboard occ = pos.pieces(WHITE) | pos.pieces(BLACK);

    {
        Bitboard bb = pos.pieces(BISHOP) & pos.pieces(us);

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard attacks = get_bishop_attacks(from, occ) & pos.pieces(them);

            while (attacks) {
                Square to = pop_lsb(attacks);
                list.push(make_move(from, to, MOVE_CAPTURE));
            }
        }
    }

    {
        Bitboard bb = pos.pieces(ROOK) & pos.pieces(us);

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard attacks = get_rook_attacks(from, occ) & pos.pieces(them);

            while (attacks) {
                Square to = pop_lsb(attacks);
                list.push(make_move(from, to, MOVE_CAPTURE));
            }
        }
    }

    {
        Bitboard bb = pos.pieces(QUEEN) & pos.pieces(us);

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard attacks = (get_bishop_attacks(from, occ) | get_rook_attacks(from, occ)) & pos.pieces(them);

            while (attacks) {
                Square to = pop_lsb(attacks);
                list.push(make_move(from, to, MOVE_CAPTURE));
            }
        }
    }

    Bitboard king = pos.pieces(KING) & pos.pieces(us);
    if (king)
    {
        Square from = lsb(king);
        Bitboard attacks = KingAttacks[from] & pos.pieces(them);

        while (attacks)
        {
            Square to = pop_lsb(attacks);
            list.push(make_move(from, to, MOVE_CAPTURE));
        }
    }
}
