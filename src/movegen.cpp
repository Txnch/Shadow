#include "movegen.h"
#include <cstdlib>
#include "bitboard.h"

static inline bool more_than_one(Bitboard b) {
    return (b & (b - 1)) != 0;
}

static Bitboard between_bb(Square a, Square b) {
    if (a == SQ_NONE || b == SQ_NONE || a == b)
        return BB_EMPTY;

    const int af = int(file_of(a));
    const int ar = int(rank_of(a));
    const int bf = int(file_of(b));
    const int br = int(rank_of(b));

    int df = 0;
    int dr = 0;

    if (af == bf)
        dr = br > ar ? 1 : -1;
    else if (ar == br)
        df = bf > af ? 1 : -1;
    else if (std::abs(af - bf) == std::abs(ar - br)) {
        df = bf > af ? 1 : -1;
        dr = br > ar ? 1 : -1;
    }
    else
        return BB_EMPTY;

    Bitboard out = BB_EMPTY;
    int f = af + df;
    int r = ar + dr;

    while (f != bf || r != br) {
        out |= square_bb(Square(r * 8 + f));
        f += df;
        r += dr;
    }

    return out;
}

static Bitboard line_bb(Square a, Square b) {
    if (a == SQ_NONE || b == SQ_NONE)
        return BB_EMPTY;

    const int af = int(file_of(a));
    const int ar = int(rank_of(a));
    const int bf = int(file_of(b));
    const int br = int(rank_of(b));

    Bitboard out = BB_EMPTY;

    if (af == bf) {
        for (int r = 0; r < 8; ++r)
            out |= square_bb(Square(r * 8 + af));
        return out;
    }

    if (ar == br) {
        for (int f = 0; f < 8; ++f)
            out |= square_bb(Square(ar * 8 + f));
        return out;
    }

    if (std::abs(af - bf) != std::abs(ar - br))
        return BB_EMPTY;

    const int df = bf > af ? 1 : -1;
    const int dr = br > ar ? 1 : -1;

    int f = af;
    int r = ar;
    while (f - df >= 0 && f - df < 8 && r - dr >= 0 && r - dr < 8) {
        f -= df;
        r -= dr;
    }

    while (f >= 0 && f < 8 && r >= 0 && r < 8) {
        out |= square_bb(Square(r * 8 + f));
        f += df;
        r += dr;
    }

    return out;
}

struct MoveGenContext {
    Color us = WHITE;
    Color them = BLACK;
    Square king = SQ_NONE;
    Bitboard own = BB_EMPTY;
    Bitboard enemy = BB_EMPTY;
    Bitboard occ = BB_EMPTY;
    Bitboard checkers = BB_EMPTY;
    Bitboard pinned = BB_EMPTY;
    Bitboard check_mask = BB_FULL;
};

static MoveGenContext make_context(const Position& pos) {
    MoveGenContext ctx;
    ctx.us = pos.side_to_move();
    ctx.them = ~ctx.us;
    ctx.king = pos.king_square(ctx.us);
    ctx.own = pos.pieces(ctx.us);
    ctx.enemy = pos.pieces(ctx.them);
    ctx.occ = pos.all_pieces();
    ctx.checkers = pos.checkers();
    ctx.pinned = pos.blockers_for_king(ctx.us) & ctx.own;

    if (ctx.checkers && !more_than_one(ctx.checkers) && ctx.king != SQ_NONE) {
        const Square checker = lsb(ctx.checkers);
        ctx.check_mask = ctx.checkers | between_bb(ctx.king, checker);
    }

    return ctx;
}

static inline Bitboard pin_mask(const MoveGenContext& ctx, Square from) {
    return (ctx.pinned & square_bb(from)) ? line_bb(ctx.king, from) : BB_FULL;
}

static bool king_target_legal(const Position& pos, const MoveGenContext& ctx, Square from, Square to) {
    const Bitboard from_bb = square_bb(from);
    const Bitboard to_bb = square_bb(to);
    const Bitboard enemy = ctx.enemy & ~to_bb;
    const Bitboard occ = (ctx.occ & ~from_bb) | to_bb;

    if (PawnAttacks[ctx.us][to] & (enemy & pos.pieces(PAWN)))
        return false;

    if (KnightAttacks[to] & (enemy & pos.pieces(KNIGHT)))
        return false;

    if (KingAttacks[to] & (enemy & pos.pieces(KING)))
        return false;

    if (get_bishop_attacks(to, occ) & (enemy & (pos.pieces(BISHOP) | pos.pieces(QUEEN))))
        return false;

    if (get_rook_attacks(to, occ) & (enemy & (pos.pieces(ROOK) | pos.pieces(QUEEN))))
        return false;

    return true;
}

static bool ep_legal(const Position& pos, const MoveGenContext& ctx, Square from) {
    const Square ep = pos.ep_square();
    if (ep == SQ_NONE)
        return false;

    if (!(PawnAttacks[ctx.us][from] & square_bb(ep)))
        return false;

    return pos.is_legal(make_en_passant(from, ep));
}


static inline void push_targets(Square from, Bitboard targets, Bitboard enemy, MoveList& list) {
    while (targets) {
        const Square to = pop_lsb(targets);
        if (square_bb(to) & enemy)
            list.push(make_move(from, to, MOVE_CAPTURE));
        else
            list.push(make_move(from, to));
    }
}

static void gen_pawn_moves(const Position& pos, MoveList& list, const MoveGenContext& ctx) {

    Color us = ctx.us;

    Bitboard pawns = pos.pieces(PAWN) & ctx.own;
    const Bitboard occ = ctx.occ;

    int dir = (us == WHITE) ? 8 : -8;
    int start_rank = (us == WHITE) ? RANK_2 : RANK_7;
    int promo_rank = (us == WHITE) ? RANK_8 : RANK_1;

    while (pawns) {

        Square from = pop_lsb(pawns);
        const Bitboard legal_targets = ctx.check_mask & pin_mask(ctx, from);
        int to_int = int(from) + dir;

        if (to_int >= 0 && to_int < 64) {

            Square to = Square(to_int);
            const Bitboard to_bb = square_bb(to);

            if (!(occ & to_bb)) {

                if (rank_of(to) == promo_rank && (to_bb & legal_targets)) {
                    list.push(make_promotion(from, to, QUEEN));
                    list.push(make_promotion(from, to, ROOK));
                    list.push(make_promotion(from, to, BISHOP));
                    list.push(make_promotion(from, to, KNIGHT));
                }
                else {
                    if (to_bb & legal_targets)
                        list.push(make_move(from, to));

                    if (rank_of(from) == start_rank) {
                        int to2_int = to_int + dir;
                        if (to2_int >= 0 && to2_int < 64) {
                            Square to2 = Square(to2_int);
                            const Bitboard to2_bb = square_bb(to2);
                            if (!(occ & to2_bb) && (to2_bb & legal_targets))
                                list.push(make_move(from, to2, MOVE_DOUBLE_PUSH));
                        }
                    }
                }
            }
        }

        Bitboard caps = PawnAttacks[us][from] & ctx.enemy & legal_targets;
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

        if (ep_legal(pos, ctx, from))
            list.push(make_en_passant(from, pos.ep_square()));
    }
}

static void gen_pawn_quiets(const Position& pos, MoveList& list, const MoveGenContext& ctx) {

    const Color us = ctx.us;
    const Bitboard pawns = pos.pieces(PAWN) & ctx.own;
    const Bitboard occ = ctx.occ;
    const int dir = (us == WHITE) ? 8 : -8;
    const int start_rank = (us == WHITE) ? RANK_2 : RANK_7;
    const int promo_rank = (us == WHITE) ? RANK_8 : RANK_1;

    Bitboard bb = pawns;
    while (bb) {
        const Square from = pop_lsb(bb);
        const Bitboard legal_targets = ctx.check_mask & pin_mask(ctx, from);
        const int to_int = int(from) + dir;
        if (to_int < 0 || to_int >= 64)
            continue;

        const Square to = Square(to_int);
        const Bitboard to_bb = square_bb(to);
        if (occ & to_bb)
            continue;

        if (rank_of(to) != promo_rank && (to_bb & legal_targets))
            list.push(make_move(from, to));

        if (rank_of(from) == start_rank) {
            const int to2_int = to_int + dir;
            if (to2_int >= 0 && to2_int < 64) {
                const Square to2 = Square(to2_int);
                const Bitboard to2_bb = square_bb(to2);
                if (!(occ & to2_bb) && (to2_bb & legal_targets))
                    list.push(make_move(from, to2, MOVE_DOUBLE_PUSH));
            }
        }
    }
}

static void gen_knight_moves(const Position& pos, MoveList& list, const MoveGenContext& ctx) {

    Bitboard bb = pos.pieces(KNIGHT) & ctx.own & ~ctx.pinned;

    while (bb) {
        Square from = pop_lsb(bb);
        Bitboard targets = KnightAttacks[from] & ~ctx.own & ctx.check_mask;
        push_targets(from, targets, ctx.enemy, list);
    }
}

static void gen_knight_quiets(const Position& pos, MoveList& list, const MoveGenContext& ctx) {

    Bitboard bb = pos.pieces(KNIGHT) & ctx.own & ~ctx.pinned;

    while (bb) {
        const Square from = pop_lsb(bb);
        Bitboard targets = KnightAttacks[from] & ~ctx.occ & ctx.check_mask;
        while (targets)
            list.push(make_move(from, pop_lsb(targets)));
    }
}

static void gen_king_moves(const Position& pos, MoveList& list, const MoveGenContext& ctx) {

    Color us = ctx.us;
    Color them = ctx.them;

    Bitboard kbb = pos.pieces(KING) & ctx.own;
    if (!kbb) return;

    Square from = lsb(kbb);

    Bitboard targets = KingAttacks[from] & ~ctx.own;
    while (targets) {
        const Square to = pop_lsb(targets);
        if (king_target_legal(pos, ctx, from, to)) {
            if (square_bb(to) & ctx.enemy)
                list.push(make_move(from, to, MOVE_CAPTURE));
            else
                list.push(make_move(from, to));
        }
    }

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

static void gen_king_quiets(const Position& pos, MoveList& list, const MoveGenContext& ctx) {

    const Color us = ctx.us;
    const Color them = ctx.them;

    Bitboard kbb = pos.pieces(KING) & ctx.own;
    if (!kbb) return;

    const Square from = lsb(kbb);
    Bitboard targets = KingAttacks[from] & ~ctx.occ;
    while (targets) {
        const Square to = pop_lsb(targets);
        if (king_target_legal(pos, ctx, from, to))
            list.push(make_move(from, to));
    }

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

    const MoveGenContext ctx = make_context(pos);
    if (more_than_one(ctx.checkers)) {
        gen_king_moves(pos, list, ctx);
        return;
    }

    gen_pawn_moves(pos, list, ctx);
    gen_knight_moves(pos, list, ctx);

    Color us = ctx.us;
    Bitboard occ = ctx.occ;
    Bitboard own = ctx.own;
    Bitboard enemy = ctx.enemy;

    {
        Bitboard bb = pos.pieces(BISHOP) & own;

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard targets = get_bishop_attacks(from, occ) & ~own & ctx.check_mask & pin_mask(ctx, from);
            push_targets(from, targets, enemy, list);
        }
    }

    {
        Bitboard bb = pos.pieces(ROOK) & own;

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard targets = get_rook_attacks(from, occ) & ~own & ctx.check_mask & pin_mask(ctx, from);
            push_targets(from, targets, enemy, list);
        }
    }

    {
        Bitboard bb = pos.pieces(QUEEN) & own;

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard targets = (get_bishop_attacks(from, occ) | get_rook_attacks(from, occ))
                & ~own & ctx.check_mask & pin_mask(ctx, from);
            push_targets(from, targets, enemy, list);
        }
    }

    gen_king_moves(pos, list, ctx);
}

void generate_quiets(const Position& pos, MoveList& list) {

    list.clear();

    const MoveGenContext ctx = make_context(pos);
    if (more_than_one(ctx.checkers)) {
        gen_king_quiets(pos, list, ctx);
        return;
    }

    gen_pawn_quiets(pos, list, ctx);
    gen_knight_quiets(pos, list, ctx);

    const Color us = ctx.us;
    const Bitboard occ = ctx.occ;

    {
        Bitboard bb = pos.pieces(BISHOP) & pos.pieces(us);
        while (bb) {
            const Square from = pop_lsb(bb);
            Bitboard targets = get_bishop_attacks(from, occ) & ~occ & ctx.check_mask & pin_mask(ctx, from);
            while (targets)
                list.push(make_move(from, pop_lsb(targets)));
        }
    }

    {
        Bitboard bb = pos.pieces(ROOK) & pos.pieces(us);
        while (bb) {
            const Square from = pop_lsb(bb);
            Bitboard targets = get_rook_attacks(from, occ) & ~occ & ctx.check_mask & pin_mask(ctx, from);
            while (targets)
                list.push(make_move(from, pop_lsb(targets)));
        }
    }

    {
        Bitboard bb = pos.pieces(QUEEN) & pos.pieces(us);
        while (bb) {
            const Square from = pop_lsb(bb);
            Bitboard targets = (get_bishop_attacks(from, occ) | get_rook_attacks(from, occ))
                & ~occ & ctx.check_mask & pin_mask(ctx, from);
            while (targets)
                list.push(make_move(from, pop_lsb(targets)));
        }
    }

    gen_king_quiets(pos, list, ctx);
}

void generate_captures(const Position& pos, MoveList& list)
{
    list.clear();

    const MoveGenContext ctx = make_context(pos);
    Color us = ctx.us;

    if (more_than_one(ctx.checkers)) {
        Bitboard king = pos.pieces(KING) & ctx.own;
        if (king) {
            Square from = lsb(king);
            Bitboard attacks = KingAttacks[from] & ctx.enemy;
            while (attacks) {
                const Square to = pop_lsb(attacks);
                if (king_target_legal(pos, ctx, from, to))
                    list.push(make_move(from, to, MOVE_CAPTURE));
            }
        }
        return;
    }

    Bitboard pawns = pos.pieces(PAWN) & ctx.own;

    int promo_rank = (us == WHITE) ? RANK_8 : RANK_1;

    while (pawns)
    {
        Square from = pop_lsb(pawns);
        const Bitboard legal_targets = ctx.check_mask & pin_mask(ctx, from);

        Bitboard caps = PawnAttacks[us][from] & ctx.enemy & legal_targets;

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
            const Bitboard to_bb = square_bb(to);
            if (rank_of(to) == promo_rank && !(ctx.occ & to_bb) && (to_bb & legal_targets))
            {
                list.push(make_promotion(from, to, QUEEN));
                list.push(make_promotion(from, to, ROOK));
                list.push(make_promotion(from, to, BISHOP));
                list.push(make_promotion(from, to, KNIGHT));
            }
        }

        if (ep_legal(pos, ctx, from))
            list.push(make_en_passant(from, pos.ep_square()));
    }

    Bitboard knights = pos.pieces(KNIGHT) & ctx.own & ~ctx.pinned;
    while (knights)
    {
        Square from = pop_lsb(knights);
        Bitboard attacks = KnightAttacks[from] & ctx.enemy & ctx.check_mask;

        while (attacks)
        {
            Square to = pop_lsb(attacks);
            list.push(make_move(from, to, MOVE_CAPTURE));
        }
    }

    Bitboard occ = ctx.occ;

    {
        Bitboard bb = pos.pieces(BISHOP) & ctx.own;

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard attacks = get_bishop_attacks(from, occ) & ctx.enemy & ctx.check_mask & pin_mask(ctx, from);

            while (attacks) {
                Square to = pop_lsb(attacks);
                list.push(make_move(from, to, MOVE_CAPTURE));
            }
        }
    }

    {
        Bitboard bb = pos.pieces(ROOK) & ctx.own;

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard attacks = get_rook_attacks(from, occ) & ctx.enemy & ctx.check_mask & pin_mask(ctx, from);

            while (attacks) {
                Square to = pop_lsb(attacks);
                list.push(make_move(from, to, MOVE_CAPTURE));
            }
        }
    }

    {
        Bitboard bb = pos.pieces(QUEEN) & ctx.own;

        while (bb) {
            Square from = pop_lsb(bb);
            Bitboard attacks = (get_bishop_attacks(from, occ) | get_rook_attacks(from, occ))
                & ctx.enemy & ctx.check_mask & pin_mask(ctx, from);

            while (attacks) {
                Square to = pop_lsb(attacks);
                list.push(make_move(from, to, MOVE_CAPTURE));
            }
        }
    }

    Bitboard king = pos.pieces(KING) & ctx.own;
    if (king)
    {
        Square from = lsb(king);
        Bitboard attacks = KingAttacks[from] & ctx.enemy;

        while (attacks)
        {
            Square to = pop_lsb(attacks);
            if (king_target_legal(pos, ctx, from, to))
                list.push(make_move(from, to, MOVE_CAPTURE));
        }
    }
}
