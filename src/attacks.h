#pragma once
#include "move.h"
#include "bitboard.h"



extern Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
extern Bitboard KnightAttacks[SQUARE_NB];
extern Bitboard KingAttacks[SQUARE_NB];



void init_attacks();



class Position;

bool is_square_attacked(const Position& pos, Square sq, Color by);
bool in_check(const Position& pos, Color c);
Bitboard attacks_from(const Position& pos, Square sq);
Bitboard attackers_to(const Position& pos, Square sq, Bitboard occ, Color c);