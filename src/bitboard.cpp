#include "bitboard.h"
#include <cassert>
#include <cstring>

#if defined(SHADOW_USE_BMI2)
#include <immintrin.h>
#endif

#if defined(SHADOW_USE_BMI2)
#pragma message("Shadow bitboard: BMI2 enabled")
#else
#pragma message("Shadow bitboard: magic bitboards enabled")
#endif

static U64 bishop_masks[64];
static U64 rook_masks[64];

static U64 bishop_attacks[64][512];
static U64 rook_attacks[64][4096];

int rook_relevant_bits[64] = {
    12, 11, 11, 11, 11, 11, 11, 12,
    11, 10, 10, 10, 10, 10, 10, 11,
    11, 10, 10, 10, 10, 10, 10, 11,
    11, 10, 10, 10, 10, 10, 10, 11,
    11, 10, 10, 10, 10, 10, 10, 11,
    11, 10, 10, 10, 10, 10, 10, 11,
    11, 10, 10, 10, 10, 10, 10, 11,
    12, 11, 11, 11, 11, 11, 11, 12
};

int bishop_relevant_bits[64] = {
    6, 5, 5, 5, 5, 5, 5, 6,
    5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 7, 7, 7, 7, 5, 5,
    5, 5, 7, 9, 9, 7, 5, 5,
    5, 5, 7, 9, 9, 7, 5, 5,
    5, 5, 7, 7, 7, 7, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5,
    6, 5, 5, 5, 5, 5, 5, 6
};

static U64 mask_bishop(int sq);
static U64 mask_rook(int sq);
static U64 set_occupancy(int index, int bits, U64 mask);

static inline U64 bishop_attack_index(int sq, U64 occ) {
    occ &= bishop_masks[sq];
#if defined(SHADOW_USE_BMI2)
    return _pext_u64(occ, bishop_masks[sq]);
#else
    return (occ * bishop_magics[sq]) >> (64 - bishop_relevant_bits[sq]);
#endif
}

static inline U64 rook_attack_index(int sq, U64 occ) {
    occ &= rook_masks[sq];
#if defined(SHADOW_USE_BMI2)
    return _pext_u64(occ, rook_masks[sq]);
#else
    return (occ * rook_magics[sq]) >> (64 - rook_relevant_bits[sq]);
#endif
}

U64 bishop_attacks_occ(int sq, U64 occ) {
    U64 attacks = 0ULL;
    int r = sq / 8;
    int f = sq % 8;

    for (int r1 = r + 1, f1 = f + 1; r1 <= 7 && f1 <= 7; r1++, f1++) {
        int s = r1 * 8 + f1;
        attacks |= 1ULL << s;
        if (occ & (1ULL << s)) break;
    }
    for (int r1 = r + 1, f1 = f - 1; r1 <= 7 && f1 >= 0; r1++, f1--) {
        int s = r1 * 8 + f1;
        attacks |= 1ULL << s;
        if (occ & (1ULL << s)) break;
    }
    for (int r1 = r - 1, f1 = f + 1; r1 >= 0 && f1 <= 7; r1--, f1++) {
        int s = r1 * 8 + f1;
        attacks |= 1ULL << s;
        if (occ & (1ULL << s)) break;
    }
    for (int r1 = r - 1, f1 = f - 1; r1 >= 0 && f1 >= 0; r1--, f1--) {
        int s = r1 * 8 + f1;
        attacks |= 1ULL << s;
        if (occ & (1ULL << s)) break;
    }
    return attacks;
}

U64 rook_attacks_occ(int sq, U64 occ) {
    U64 attacks = 0ULL;
    int r = sq / 8;
    int f = sq % 8;

    for (int r1 = r + 1; r1 <= 7; r1++) {
        int s = r1 * 8 + f;
        attacks |= 1ULL << s;
        if (occ & (1ULL << s)) break;
    }
    for (int r1 = r - 1; r1 >= 0; r1--) {
        int s = r1 * 8 + f;
        attacks |= 1ULL << s;
        if (occ & (1ULL << s)) break;
    }
    for (int f1 = f + 1; f1 <= 7; f1++) {
        int s = r * 8 + f1;
        attacks |= 1ULL << s;
        if (occ & (1ULL << s)) break;
    }
    for (int f1 = f - 1; f1 >= 0; f1--) {
        int s = r * 8 + f1;
        attacks |= 1ULL << s;
        if (occ & (1ULL << s)) break;
    }
    return attacks;
}

void init_magic() {
    memset(bishop_attacks, 0, sizeof(bishop_attacks));
    memset(rook_attacks, 0, sizeof(rook_attacks));
    for (int sq = 0; sq < 64; sq++) {
        bishop_masks[sq] = mask_bishop(sq);
        rook_masks[sq] = mask_rook(sq);

        int bishop_occ = 1 << bishop_relevant_bits[sq];
        for (int i = 0; i < bishop_occ; i++) {
            U64 occ = set_occupancy(i, bishop_relevant_bits[sq], bishop_masks[sq]);
            U64 attack = bishop_attacks_occ(sq, occ);
            U64 index = bishop_attack_index(sq, occ);
            bishop_attacks[sq][index] = attack;
        }

        int rook_occ = 1 << rook_relevant_bits[sq];
        for (int i = 0; i < rook_occ; i++) {
            U64 occ = set_occupancy(i, rook_relevant_bits[sq], rook_masks[sq]);
            U64 attack = rook_attacks_occ(sq, occ);
            U64 index = rook_attack_index(sq, occ);
            rook_attacks[sq][index] = attack;
        }
    }
}

U64 get_bishop_attacks(int sq, U64 occ) {
    return bishop_attacks[sq][bishop_attack_index(sq, occ)];
}

U64 get_rook_attacks(int sq, U64 occ) {
    return rook_attacks[sq][rook_attack_index(sq, occ)];
}

static U64 mask_bishop(int sq) {
    U64 attacks = 0ULL;
    int r = sq / 8;
    int f = sq % 8;

    for (int r1 = r + 1, f1 = f + 1; r1 <= 6 && f1 <= 6; r1++, f1++)
        attacks |= 1ULL << (r1 * 8 + f1);
    for (int r1 = r + 1, f1 = f - 1; r1 <= 6 && f1 >= 1; r1++, f1--)
        attacks |= 1ULL << (r1 * 8 + f1);
    for (int r1 = r - 1, f1 = f + 1; r1 >= 1 && f1 <= 6; r1--, f1++)
        attacks |= 1ULL << (r1 * 8 + f1);
    for (int r1 = r - 1, f1 = f - 1; r1 >= 1 && f1 >= 1; r1--, f1--)
        attacks |= 1ULL << (r1 * 8 + f1);

    return attacks;
}

static U64 mask_rook(int sq) {
    U64 attacks = 0ULL;
    int r = sq / 8;
    int f = sq % 8;

    for (int r1 = r + 1; r1 <= 6; r1++) attacks |= 1ULL << (r1 * 8 + f);
    for (int r1 = r - 1; r1 >= 1; r1--) attacks |= 1ULL << (r1 * 8 + f);
    for (int f1 = f + 1; f1 <= 6; f1++) attacks |= 1ULL << (r * 8 + f1);
    for (int f1 = f - 1; f1 >= 1; f1--) attacks |= 1ULL << (r * 8 + f1);

    return attacks;
}

static U64 set_occupancy(int index, int bits, U64 mask) {
    U64 occupancy = 0ULL;
    for (int i = 0; i < bits; i++) {
#if defined(_MSC_VER)
        unsigned long sq;
        _BitScanForward64(&sq, mask);
#else
        int sq = __builtin_ctzll(mask);
#endif
        mask &= mask - 1;
        if (index & (1 << i))
            occupancy |= (1ULL << sq);
    }
    return occupancy;
}
