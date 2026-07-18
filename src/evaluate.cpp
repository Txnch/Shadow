#include "nnue.h"
#include "position.h"

static int material_phase(const Position& pos)
{
	return
		2 * popcount(pos.pieces(PAWN)) +
		3 * popcount(pos.pieces(KNIGHT)) +
		3 * popcount(pos.pieces(BISHOP)) +
		5 * popcount(pos.pieces(ROOK)) +
		12 * popcount(pos.pieces(QUEEN));
}


int evaluate(const Position& pos)
{
	int score = nnue::is_ready() ? nnue::evaluate(pos) : 0;
	score = score * (material_phase(pos) + 180) / 280;
	return score;
}