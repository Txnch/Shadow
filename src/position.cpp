#include "position.h"
#include "attacks.h"
#include "bitboard.h"
#include "nnue.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <random>
#include <sstream>
#include <cstring>

uint64_t zobrist_piece[PIECE_NB][64];
uint64_t zobrist_side;
uint64_t zobrist_castling[16];
uint64_t zobrist_ep[64];

static inline bool ep_capturable(Color stm, Square epSq, Bitboard pawns)
{
    if (epSq == SQ_NONE) return false;

    int ep = int(epSq);
    int f = ep % 8;

    if (stm == WHITE)
    {

        if (f != 7)
        {
            int from = ep - 7;
            if (from >= 0 && (pawns & square_bb(Square(from))))
                return true;
        }
        if (f != 0)
        {
            int from = ep - 9;
            if (from >= 0 && (pawns & square_bb(Square(from))))
                return true;
        }
    }
    else
    {

        if (f != 7)
        {
            int from = ep + 9;
            if (from < 64 && (pawns & square_bb(Square(from))))
                return true;
        }
        if (f != 0)
        {
            int from = ep + 7;
            if (from < 64 && (pawns & square_bb(Square(from))))
                return true;
        }
    }
    return false;
}

static inline bool ep_should_be_hashed(const Position& pos, Color stm, Square epSq)
{
    Bitboard pawns = (stm == WHITE) ? pos.pieces(WHITE) & pos.pieces(PAWN)
        : pos.pieces(BLACK) & pos.pieces(PAWN);
    return ep_capturable(stm, epSq, pawns);
}

static inline uint64_t key_at_distance(const Position& pos, int dist)
{
    if (dist <= 0)
        return pos.hash();

    const int idx = pos.history_size() - dist;
    if (idx < 0)
        return 0;

    return pos.history_key(idx);
}

static inline int plies_since_last_null(const Position& pos, int limit)
{
    const int maxPlies = std::min(limit, pos.history_size());
    int d = 0;
    for (; d < maxPlies; ++d)
    {
        const int idx = pos.history_size() - 1 - d;
        if (pos.history_is_null(idx))
            break;
    }
    return d;
}

static inline bool squares_aligned(Square a, Square b, Square c)
{
    const int af = int(file_of(a));
    const int ar = int(rank_of(a));
    const int bf = int(file_of(b));
    const int br = int(rank_of(b));
    const int cf = int(file_of(c));
    const int cr = int(rank_of(c));

    if (af == bf && bf == cf)
        return true;
    if (ar == br && br == cr)
        return true;
    if ((af - bf == ar - br) && (bf - cf == br - cr))
        return true;
    if ((af - bf == br - ar) && (bf - cf == cr - br))
        return true;
    return false;
}

static Bitboard between_squares(Square a, Square b)
{
    const int af = int(file_of(a));
    const int ar = int(rank_of(a));
    const int bf = int(file_of(b));
    const int br = int(rank_of(b));

    int df = 0;
    int dr = 0;

    if (af == bf)
        dr = (br > ar) ? 1 : -1;
    else if (ar == br)
        df = (bf > af) ? 1 : -1;
    else if (std::abs(af - bf) == std::abs(ar - br)) {
        df = (bf > af) ? 1 : -1;
        dr = (br > ar) ? 1 : -1;
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

    return out & ~square_bb(a) & ~square_bb(b);
}

static inline void aux_xor_piece_keys(Piece p, uint64_t z, uint64_t& pawnKey, std::array<uint64_t, COLOR_NB>& nonPawnKeys) {
    if (piece_type(p) == PAWN) {
        pawnKey ^= z;
    }
    else {
        nonPawnKeys[piece_color(p)] ^= z;
    }
}

static void compute_blockers_for_king(const Position& pos,
    Color kingColor,
    Bitboard& blockers,
    Bitboard& pinnersOfEnemy)
{
    blockers = BB_EMPTY;
    pinnersOfEnemy = BB_EMPTY;

    const Square ksq = pos.king_square(kingColor);
    if (ksq == SQ_NONE)
        return;

    const Color enemy = ~kingColor;
    const Bitboard enemyRQ = (pos.pieces(enemy) & (pos.pieces(ROOK) | pos.pieces(QUEEN)));
    const Bitboard enemyBQ = (pos.pieces(enemy) & (pos.pieces(BISHOP) | pos.pieces(QUEEN)));

    Bitboard snipers = (get_rook_attacks(ksq, BB_EMPTY) & enemyRQ)
        | (get_bishop_attacks(ksq, BB_EMPTY) & enemyBQ);
    const Bitboard occupied = pos.all_pieces() & ~snipers;

    while (snipers) {
        const Square sniperSq = pop_lsb(snipers);
        const Bitboard between = between_squares(ksq, sniperSq) & occupied;
        if (popcount(between) == 1) {
            blockers |= between;
            if (between & pos.pieces(kingColor))
                pinnersOfEnemy |= square_bb(sniperSq);
        }
    }
}

void init_zobrist() {
    std::mt19937_64 rng(123456789);
    std::uniform_int_distribution<uint64_t> dist;

    for (int p = 0; p < PIECE_NB; ++p)
        for (int s = 0; s < 64; ++s)
            zobrist_piece[p][s] = dist(rng);

    zobrist_side = dist(rng);

    for (int i = 0; i < 16; ++i)
        zobrist_castling[i] = dist(rng);

    for (int i = 0; i < 64; ++i)
        zobrist_ep[i] = dist(rng);
}

Position::Position() { clear(); }

Position::Position(const Position& other) = default;

void Position::clear() {
    piece_bb.fill(BB_EMPTY);
    board.fill(NO_PIECE);

    occ[WHITE] = occ[BLACK] = occ_all = BB_EMPTY;

    side = WHITE;
    ep = SQ_NONE;
    castling_right = NO_CASTLING;
    ply = 0;
    hash_key = 0;
    pawn_hash_key = 0;
    non_pawn_hash_key = { 0, 0 };
    halfmove_clock_state = 0;
    fullmove_number_state = 1;

    refresh_check_info();
}

void Position::set_fen(const std::string& fen) {
    clear();

    std::istringstream ss(fen);
    std::string board_part, stm, castling_part, ep_part;
    int halfmove = 0, fullmove = 1;
    ss >> board_part >> stm >> castling_part >> ep_part >> halfmove >> fullmove;

    int rank = 7;
    int file = 0;

    for (char c : board_part) {
        if (c == '/') {
            rank--;
            file = 0;
        }
        else if (c >= '1' && c <= '8') {
            file += c - '0';
        }
        else {
            Square sq = Square(rank * 8 + file);

            Color col = (c >= 'a') ? BLACK : WHITE;
            PieceType pt = NO_PIECE_TYPE;

            switch (std::tolower(static_cast<unsigned char>(c))) {
            case 'p': pt = PAWN; break;
            case 'n': pt = KNIGHT; break;
            case 'b': pt = BISHOP; break;
            case 'r': pt = ROOK; break;
            case 'q': pt = QUEEN; break;
            case 'k': pt = KING; break;
            default: pt = NO_PIECE_TYPE; break;
            }

            Piece p = make_piece(col, pt);

            board[sq] = p;
            piece_bb[p] |= square_bb(sq);
            occ[col] |= square_bb(sq);
            occ_all |= square_bb(sq);

            file++;
        }
    }

    side = (stm == "w") ? WHITE : BLACK;

    castling_right = NO_CASTLING;
    if (castling_part.find('K') != std::string::npos) castling_right |= WHITE_OO;
    if (castling_part.find('Q') != std::string::npos) castling_right |= WHITE_OOO;
    if (castling_part.find('k') != std::string::npos) castling_right |= BLACK_OO;
    if (castling_part.find('q') != std::string::npos) castling_right |= BLACK_OOO;

    if (ep_part != "-" && ep_part.size() == 2) {
        int f = ep_part[0] - 'a';
        int r = ep_part[1] - '1';
        ep = Square(r * 8 + f);
    }
    else {
        ep = SQ_NONE;
    }

    halfmove_clock_state = (halfmove > 0) ? halfmove : 0;
    fullmove_number_state = std::max(1, fullmove);

    hash_key = 0;
    pawn_hash_key = 0;
    non_pawn_hash_key = { 0, 0 };
    for (int s = 0; s < 64; ++s) {
        Piece p = board[s];
        if (p != NO_PIECE) {
            uint64_t z = zobrist_piece[p][s];
            hash_key ^= z;
            aux_xor_piece_keys(p, z, pawn_hash_key, non_pawn_hash_key);
        }
    }

    if (side == BLACK)
        hash_key ^= zobrist_side;

    hash_key ^= zobrist_castling[castling_right];

    if (ep != SQ_NONE && ep_should_be_hashed(*this, side, ep))
        hash_key ^= zobrist_ep[ep];

    refresh_check_info();
}

Piece Position::piece_on(Square s) const {
    assert(s >= SQ_A1 && s <= SQ_H8);
    return board[s];
}

Bitboard Position::pieces(Color c) const { return occ[c]; }

Bitboard Position::pieces(PieceType pt) const {
    switch (pt) {
    case PAWN:   return piece_bb[W_PAWN] | piece_bb[B_PAWN];
    case KNIGHT: return piece_bb[W_KNIGHT] | piece_bb[B_KNIGHT];
    case BISHOP: return piece_bb[W_BISHOP] | piece_bb[B_BISHOP];
    case ROOK:   return piece_bb[W_ROOK] | piece_bb[B_ROOK];
    case QUEEN:  return piece_bb[W_QUEEN] | piece_bb[B_QUEEN];
    case KING:   return piece_bb[W_KING] | piece_bb[B_KING];
    default:     return BB_EMPTY;
    }
}

Square Position::king_square(Color c) const {
    Piece king = (c == WHITE) ? W_KING : B_KING;
    Bitboard bb = piece_bb[king];
    if (!bb) return SQ_NONE;
    return lsb(bb);
}

bool Position::is_pseudo_legal(Move m) const {
    if (!m)
        return false;

    const Square from = from_sq(m);
    const Square to = to_sq(m);
    if (from < SQ_A1 || from > SQ_H8 || to < SQ_A1 || to > SQ_H8)
        return false;
    if (from == to)
        return false;

    const Piece mover = board[from];
    if (mover == NO_PIECE || piece_color(mover) != side)
        return false;

    const Piece target = board[to];
    const Color us = side;

    const bool capture = is_capture(m);
    const bool epMove = is_en_passant(m);
    const bool castleMove = is_castling(m);
    const bool promoMove = is_promotion(m);
    const bool doublePush = is_double_push(m);

    if (!promoMove && promotion_type(m) != NO_PIECE_TYPE)
        return false;

    if (castleMove && (capture || epMove || promoMove || doublePush))
        return false;

    if (epMove && (!capture || castleMove || promoMove || doublePush))
        return false;

    if (!epMove && target != NO_PIECE && piece_color(target) == us)
        return false;

    const PieceType pt = piece_type(mover);

    if (castleMove) {
        if (pt != KING)
            return false;

        Square rookFrom = SQ_NONE;
        int needRight = NO_CASTLING;

        if (us == WHITE) {
            if (from == SQ_E1 && to == SQ_G1) {
                needRight = WHITE_OO;
                rookFrom = SQ_H1;
                if (board[SQ_F1] != NO_PIECE || board[SQ_G1] != NO_PIECE)
                    return false;
                if (in_check(*this, WHITE)
                    || is_square_attacked(*this, SQ_F1, BLACK)
                    || is_square_attacked(*this, SQ_G1, BLACK))
                    return false;
            }
            else if (from == SQ_E1 && to == SQ_C1) {
                needRight = WHITE_OOO;
                rookFrom = SQ_A1;
                if (board[SQ_D1] != NO_PIECE || board[SQ_C1] != NO_PIECE || board[SQ_B1] != NO_PIECE)
                    return false;
                if (in_check(*this, WHITE)
                    || is_square_attacked(*this, SQ_D1, BLACK)
                    || is_square_attacked(*this, SQ_C1, BLACK))
                    return false;
            }
            else {
                return false;
            }
        }
        else {
            if (from == SQ_E8 && to == SQ_G8) {
                needRight = BLACK_OO;
                rookFrom = SQ_H8;
                if (board[SQ_F8] != NO_PIECE || board[SQ_G8] != NO_PIECE)
                    return false;
                if (in_check(*this, BLACK)
                    || is_square_attacked(*this, SQ_F8, WHITE)
                    || is_square_attacked(*this, SQ_G8, WHITE))
                    return false;
            }
            else if (from == SQ_E8 && to == SQ_C8) {
                needRight = BLACK_OOO;
                rookFrom = SQ_A8;
                if (board[SQ_D8] != NO_PIECE || board[SQ_C8] != NO_PIECE || board[SQ_B8] != NO_PIECE)
                    return false;
                if (in_check(*this, BLACK)
                    || is_square_attacked(*this, SQ_D8, WHITE)
                    || is_square_attacked(*this, SQ_C8, WHITE))
                    return false;
            }
            else {
                return false;
            }
        }

        if (!(castling_right & needRight))
            return false;

        const Piece rook = make_piece(us, ROOK);
        if (rookFrom == SQ_NONE || board[rookFrom] != rook)
            return false;

        return true;
    }

    if (epMove) {
        if (pt != PAWN)
            return false;
        if (ep == SQ_NONE || to != ep)
            return false;
        if (target != NO_PIECE)
            return false;

        const int df = int(file_of(to)) - int(file_of(from));
        const int dr = int(rank_of(to)) - int(rank_of(from));

        if (us == WHITE) {
            if (dr != 1 || std::abs(df) != 1)
                return false;
            const Square capSq = Square(int(to) - 8);
            if (board[capSq] != B_PAWN)
                return false;
        }
        else {
            if (dr != -1 || std::abs(df) != 1)
                return false;
            const Square capSq = Square(int(to) + 8);
            if (board[capSq] != W_PAWN)
                return false;
        }

        return true;
    }

    const bool targetOccupied = (target != NO_PIECE);
    if (capture != targetOccupied)
        return false;

    const int ff = int(file_of(from));
    const int fr = int(rank_of(from));
    const int tf = int(file_of(to));
    const int tr = int(rank_of(to));
    const int df = tf - ff;
    const int dr = tr - fr;

    auto absi = [](int v) { return v < 0 ? -v : v; };

    switch (pt) {
    case PAWN: {
        if (promoMove) {
            const PieceType pr = promotion_type(m);
            if (pr != QUEEN && pr != ROOK && pr != BISHOP && pr != KNIGHT)
                return false;
        }

        const int step = (us == WHITE) ? 1 : -1;
        const Rank startRank = (us == WHITE) ? RANK_2 : RANK_7;
        const Rank promoRank = (us == WHITE) ? RANK_8 : RANK_1;

        if (capture) {
            if (dr != step || absi(df) != 1)
                return false;
        }
        else {
            if (df != 0)
                return false;

            if (dr == step) {
                if (doublePush)
                    return false;
            }
            else if (dr == 2 * step) {
                if (!doublePush || rank_of(from) != startRank)
                    return false;
                const Square mid = Square(int(from) + (us == WHITE ? 8 : -8));
                if (board[mid] != NO_PIECE)
                    return false;
            }
            else {
                return false;
            }
        }

        const bool reachesPromo = (rank_of(to) == promoRank);
        if (reachesPromo != promoMove)
            return false;

        return true;
    }

    case KNIGHT:
        if (promoMove || doublePush)
            return false;
        return (KnightAttacks[from] & square_bb(to)) != 0;

    case BISHOP:
        if (promoMove || doublePush)
            return false;
        return (get_bishop_attacks(from, occ_all) & square_bb(to)) != 0;

    case ROOK:
        if (promoMove || doublePush)
            return false;
        return (get_rook_attacks(from, occ_all) & square_bb(to)) != 0;

    case QUEEN:
        if (promoMove || doublePush)
            return false;
        return ((get_bishop_attacks(from, occ_all) | get_rook_attacks(from, occ_all)) & square_bb(to)) != 0;

    case KING:
        if (promoMove || doublePush)
            return false;
        return (KingAttacks[from] & square_bb(to)) != 0;

    default:
        return false;
    }
}
bool Position::gives_check(Move m) const {
    if (!m)
        return false;

    const Color us = side;
    const Color them = ~us;
    const Square enemyKing = king_square(them);
    if (enemyKing == SQ_NONE)
        return false;

    const Square from = from_sq(m);
    const Square to = to_sq(m);
    const Piece mover = board[from];
    if (mover == NO_PIECE || piece_color(mover) != us)
        return false;

    const PieceType movedType = piece_type(mover);
    const PieceType placedType = is_promotion(m) ? promotion_type(m) : movedType;
    if (check_squares(placedType) & square_bb(to))
        return true;

    if ((blockers_for_king(them) & square_bb(from))
        && (!squares_aligned(from, to, enemyKing) || is_castling(m)))
        return true;

    if (is_en_passant(m)) {
        const Square capSq = (us == WHITE) ? Square(int(to) - 8) : Square(int(to) + 8);
        const Bitboard occ = (occ_all ^ square_bb(from) ^ square_bb(capSq)) | square_bb(to);
        return (get_bishop_attacks(enemyKing, occ) & (piece_bb[make_piece(us, BISHOP)] | piece_bb[make_piece(us, QUEEN)]))
            || (get_rook_attacks(enemyKing, occ) & (piece_bb[make_piece(us, ROOK)] | piece_bb[make_piece(us, QUEEN)]));
    }

    if (is_castling(m)) {
        const Square rookTo = (us == WHITE)
            ? ((to == SQ_G1) ? SQ_F1 : SQ_D1)
            : ((to == SQ_G8) ? SQ_F8 : SQ_D8);
        return (check_squares(ROOK) & square_bb(rookTo)) != 0;
    }

    return false;
}

bool Position::is_repetition_draw(int ply_from_root) const
{
    const int maxDist = std::min(halfmove_clock(),
        plies_since_last_null(*this, history_size()));

    bool hitBeforeRoot = false;
    for (int i = 4; i <= maxDist; i += 2)
    {
        if (hash() == key_at_distance(*this, i))
        {
            if (ply_from_root >= i)
                return true;
            if (hitBeforeRoot)
                return true;
            hitBeforeRoot = true;
        }
    }

    return false;
}



void Position::refresh_check_info() {
    checkers_bb = BB_EMPTY;
    blockers_for_king_bb[WHITE] = BB_EMPTY;
    blockers_for_king_bb[BLACK] = BB_EMPTY;
    pinners_bb[WHITE] = BB_EMPTY;
    pinners_bb[BLACK] = BB_EMPTY;
    check_squares_bb.fill(BB_EMPTY);

    const Square stmKing = king_square(side);
    if (stmKing != SQ_NONE)
        checkers_bb = attackers_to(*this, stmKing, occ_all, ~side);

    compute_blockers_for_king(*this, WHITE, blockers_for_king_bb[WHITE], pinners_bb[BLACK]);
    compute_blockers_for_king(*this, BLACK, blockers_for_king_bb[BLACK], pinners_bb[WHITE]);

    const Square enemyKing = king_square(~side);
    if (enemyKing == SQ_NONE)
        return;

    check_squares_bb[PAWN] = PawnAttacks[~side][enemyKing];
    check_squares_bb[KNIGHT] = KnightAttacks[enemyKing];
    check_squares_bb[BISHOP] = get_bishop_attacks(enemyKing, occ_all);
    check_squares_bb[ROOK] = get_rook_attacks(enemyKing, occ_all);
    check_squares_bb[QUEEN] = check_squares_bb[BISHOP] | check_squares_bb[ROOK];
    check_squares_bb[KING] = BB_EMPTY;
}

bool Position::make_move(Move m) {
    if (ply >= MAX_GAME_PLY - 1)
        return false;

    if (!is_pseudo_legal(m))
        return false;

    Square from = from_sq(m);
    Square to = to_sq(m);

    Piece p = board[from];

    assert(p != NO_PIECE);
    assert(piece_color(p) == side);

    Undo& u = history[ply++];
    u.is_null = false;
    u.move = m;
    u.hash_key = hash_key;
    u.pawn_key = pawn_hash_key;
    u.non_pawn_key[WHITE] = non_pawn_hash_key[WHITE];
    u.non_pawn_key[BLACK] = non_pawn_hash_key[BLACK];
    u.halfmove_clock = halfmove_clock_state;
    u.fullmove_number = fullmove_number_state;

    u.ep_square = ep;
    u.castling_rights = castling_right;
    u.captured = NO_PIECE;
    u.captured_sq = SQ_NONE;
    u.dp = {};

    Color us = side;

    hash_key ^= zobrist_side;
    if (ep != SQ_NONE && ep_should_be_hashed(*this, us, ep)) hash_key ^= zobrist_ep[ep];
    hash_key ^= zobrist_castling[castling_right];

    ep = SQ_NONE;

    if (is_en_passant(m)) {
        Square cap = (us == WHITE) ? Square(int(to) - 8) : Square(int(to) + 8);
        u.captured = board[cap];
        u.captured_sq = cap;
    }
    else if (is_capture(m)) {
        u.captured = board[to];
        u.captured_sq = to;
    }

    halfmove_clock_state = (piece_type(p) == PAWN || u.captured != NO_PIECE) ? 0 : (halfmove_clock_state + 1);


    if (u.captured != NO_PIECE) {
        Square csq = u.captured_sq;
        board[csq] = NO_PIECE;
        piece_bb[u.captured] &= ~square_bb(csq);
        occ[piece_color(u.captured)] &= ~square_bb(csq);
        occ_all &= ~square_bb(csq);
        uint64_t z_cap = zobrist_piece[u.captured][csq];
        hash_key ^= z_cap;
        aux_xor_piece_keys(u.captured, z_cap, pawn_hash_key, non_pawn_hash_key);
    }

    board[from] = NO_PIECE;
    piece_bb[p] &= ~square_bb(from);
    occ[piece_color(p)] &= ~square_bb(from);
    occ_all &= ~square_bb(from);
    uint64_t z_from = zobrist_piece[p][from];
    hash_key ^= z_from;
    aux_xor_piece_keys(p, z_from, pawn_hash_key, non_pawn_hash_key);

    Piece placed = is_promotion(m) ? make_piece(us, promotion_type(m)) : p;

    board[to] = placed;
    piece_bb[placed] |= square_bb(to);
    occ[us] |= square_bb(to);
    occ_all |= square_bb(to);
    uint64_t z_to = zobrist_piece[placed][to];
    hash_key ^= z_to;
    aux_xor_piece_keys(placed, z_to, pawn_hash_key, non_pawn_hash_key);

    Square rook_from = SQ_NONE;
    Square rook_to = SQ_NONE;
    Piece rook = NO_PIECE;
    if (is_castling(m)) {
        if (us == WHITE) {
            if (to == SQ_G1) { rook_from = SQ_H1; rook_to = SQ_F1; }
            else { rook_from = SQ_A1; rook_to = SQ_D1; }
        }
        else {
            if (to == SQ_G8) { rook_from = SQ_H8; rook_to = SQ_F8; }
            else { rook_from = SQ_A8; rook_to = SQ_D8; }
        }

        rook = board[rook_from];
        assert(rook != NO_PIECE);

        board[rook_from] = NO_PIECE;
        piece_bb[rook] &= ~square_bb(rook_from);
        occ[piece_color(rook)] &= ~square_bb(rook_from);
        occ_all &= ~square_bb(rook_from);
        uint64_t z_rook_from = zobrist_piece[rook][rook_from];
        hash_key ^= z_rook_from;
        aux_xor_piece_keys(rook, z_rook_from, pawn_hash_key, non_pawn_hash_key);

        board[rook_to] = rook;
        piece_bb[rook] |= square_bb(rook_to);
        occ[piece_color(rook)] |= square_bb(rook_to);
        occ_all |= square_bb(rook_to);
        uint64_t z_rook_to = zobrist_piece[rook][rook_to];
        hash_key ^= z_rook_to;
        aux_xor_piece_keys(rook, z_rook_to, pawn_hash_key, non_pawn_hash_key);
    }

    nnue::DirtyPieces dp{};
    dp.sub0 = { p, from };
    dp.add0 = { placed, to };
    if (is_castling(m)) {
        dp.sub1 = { rook, rook_from };
        dp.add1 = { rook, rook_to };
    }
    else if (u.captured != NO_PIECE) {
        dp.sub1 = { u.captured, u.captured_sq };
    }
    u.dp = dp;

    if (is_double_push(m)) {
        ep = (us == WHITE) ? Square(int(from) + 8) : Square(int(from) - 8);
    }

    if (p == W_KING) castling_right &= ~(WHITE_OO | WHITE_OOO);
    if (p == B_KING) castling_right &= ~(BLACK_OO | BLACK_OOO);

    if (from == SQ_A1 || to == SQ_A1) castling_right &= ~WHITE_OOO;
    if (from == SQ_H1 || to == SQ_H1) castling_right &= ~WHITE_OO;
    if (from == SQ_A8 || to == SQ_A8) castling_right &= ~BLACK_OOO;
    if (from == SQ_H8 || to == SQ_H8) castling_right &= ~BLACK_OO;

    hash_key ^= zobrist_castling[castling_right];
    if (ep != SQ_NONE && ep_should_be_hashed(*this, ~us, ep))
        hash_key ^= zobrist_ep[ep];

    side = ~side;
    if (us == BLACK)
        ++fullmove_number_state;

    const Square ourKing = king_square(us);
    if (ourKing != SQ_NONE && is_square_attacked(*this, ourKing, ~us)) {
        undo_move();
        return false;
    }

    refresh_check_info();
    return true;
}
void Position::undo_move() {
    assert(ply > 0);

    Undo& u = history[--ply];
    Move m = u.move;
    halfmove_clock_state = u.halfmove_clock;
    fullmove_number_state = u.fullmove_number;

    Square from = from_sq(m);
    Square to = to_sq(m);

    side = ~side;
    Color us = side;

    hash_key = u.hash_key;
    pawn_hash_key = u.pawn_key;
    non_pawn_hash_key[WHITE] = u.non_pawn_key[WHITE];
    non_pawn_hash_key[BLACK] = u.non_pawn_key[BLACK];
    ep = u.ep_square;
    castling_right = u.castling_rights;

    Piece p = board[to];

    if (is_promotion(m)) {
        piece_bb[p] &= ~square_bb(to);
        occ[piece_color(p)] &= ~square_bb(to);
        occ_all &= ~square_bb(to);
        p = make_piece(us, PAWN);
    }

    board[to] = NO_PIECE;
    piece_bb[p] &= ~square_bb(to);
    occ[piece_color(p)] &= ~square_bb(to);
    occ_all &= ~square_bb(to);

    board[from] = p;
    piece_bb[p] |= square_bb(from);
    occ[piece_color(p)] |= square_bb(from);
    occ_all |= square_bb(from);

    if (is_castling(m)) {
        Square rook_from, rook_to;
        if (us == WHITE) {
            if (to == SQ_G1) { rook_from = SQ_H1; rook_to = SQ_F1; }
            else { rook_from = SQ_A1; rook_to = SQ_D1; }
        }
        else {
            if (to == SQ_G8) { rook_from = SQ_H8; rook_to = SQ_F8; }
            else { rook_from = SQ_A8; rook_to = SQ_D8; }
        }

        Piece rook = board[rook_to];

        board[rook_to] = NO_PIECE;
        piece_bb[rook] &= ~square_bb(rook_to);
        occ[piece_color(rook)] &= ~square_bb(rook_to);
        occ_all &= ~square_bb(rook_to);

        board[rook_from] = rook;
        piece_bb[rook] |= square_bb(rook_from);
        occ[piece_color(rook)] |= square_bb(rook_from);
        occ_all |= square_bb(rook_from);
    }

    if (u.captured != NO_PIECE) {
        Square sq = u.captured_sq;
        board[sq] = u.captured;
        piece_bb[u.captured] |= square_bb(sq);
        occ[piece_color(u.captured)] |= square_bb(sq);
        occ_all |= square_bb(sq);
    }

    refresh_check_info();
}
void Position::do_null_move() {
    assert(ply < MAX_GAME_PLY);

    Undo& u = history[ply++];

    u.is_null = true;
    u.move = 0;
    u.captured = NO_PIECE;
    u.captured_sq = SQ_NONE;
    u.ep_square = ep;
    u.castling_rights = castling_right;
    u.hash_key = hash_key;
    u.pawn_key = pawn_hash_key;
    u.non_pawn_key[WHITE] = non_pawn_hash_key[WHITE];
    u.non_pawn_key[BLACK] = non_pawn_hash_key[BLACK];
    u.halfmove_clock = halfmove_clock_state;
    u.fullmove_number = fullmove_number_state;
    u.dp = {};


    if (ep != SQ_NONE && ep_should_be_hashed(*this, side, ep))
        hash_key ^= zobrist_ep[ep];

    ep = SQ_NONE;

    side = ~side;
    hash_key ^= zobrist_side;
    refresh_check_info();

}
void Position::undo_null_move() {
    assert(ply > 0);

    Undo& u = history[--ply];
    assert(u.is_null);

    side = ~side;
    ep = u.ep_square;
    castling_right = u.castling_rights;
    hash_key = u.hash_key;
    pawn_hash_key = u.pawn_key;
    non_pawn_hash_key[WHITE] = u.non_pawn_key[WHITE];
    non_pawn_hash_key[BLACK] = u.non_pawn_key[BLACK];
    halfmove_clock_state = u.halfmove_clock;
    fullmove_number_state = u.fullmove_number;
    refresh_check_info();
}

