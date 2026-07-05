#include "lookup.h"
#include "bitboard.h"
#include "move.h"
#include "position.h"


#include <iostream>
#include <cstring>
#include <algorithm>
#include <cstdlib>


extern uint64_t zobrist_piece[PIECE_NB][64];
extern uint64_t zobrist_side;

namespace Cuckoo {

    Key keys[8192];
    Move moves[8192];


    Bitboard pseudo_attacks(PieceType pt, Square sq) {
        switch (pt) {
        case KNIGHT: return KnightAttacks[sq];
        case BISHOP: return get_bishop_attacks(sq, BB_EMPTY);
        case ROOK:   return get_rook_attacks(sq, BB_EMPTY);
        case QUEEN:  return get_bishop_attacks(sq, BB_EMPTY) | get_rook_attacks(sq, BB_EMPTY);
        case KING:   return KingAttacks[sq];
        default:     return BB_EMPTY;
        }
    }

    void init() {

        std::memset(keys, 0, sizeof(keys));
        std::memset(moves, 0, sizeof(moves));


        int count = 0;

        for (PieceType pt : {KNIGHT, BISHOP, ROOK, QUEEN, KING}) {
            for (Color color : {WHITE, BLACK}) {

                Piece piece = make_piece(color, pt);


                for (int s1_idx = 0; s1_idx < 64; ++s1_idx) {
                    Square s1 = static_cast<Square>(s1_idx);

                    for (int s2_idx = s1_idx + 1; s2_idx < 64; ++s2_idx) {
                        Square s2 = static_cast<Square>(s2_idx);


                        if (pseudo_attacks(pt, s1) & square_bb(s2)) {

                            Move move = make_move(s1, s2);


                            Key key = zobrist_piece[piece][s1] ^ zobrist_piece[piece][s2] ^ zobrist_side;

                            int slot = h1(key);

                            while (true) {
                                std::swap(keys[slot], key);
                                std::swap(moves[slot], move);

                                if (!move)
                                    break;


                                slot = (slot == h1(key)) ? h2(key) : h1(key);
                            }

                            count++;
                        }
                    }
                }
            }
        }

        if (count != 3668) {
            std::cout << "oops! cuckoo table is broken." << std::endl;
            std::exit(-1);
        }
    }
}