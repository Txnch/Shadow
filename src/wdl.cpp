#include "wdl.h"

#include "bitboard.h"

#include <algorithm>
#include <cmath>

namespace wdl {
    namespace {

        struct Params {
            double a;
            double b;
        };

        Params params(const Position& pos)
        {
            const int mat = std::clamp(material(pos), 17, 78);
            const double m = static_cast<double>(mat) / 58.0;

            constexpr double as[] = { 72.61734468, -120.19665423, -43.28430432, 284.44370533 };
            constexpr double bs[] = { 110.00526088, -272.83420461, 281.23732815, 25.95302509 };

            const double a = ((as[0] * m + as[1]) * m + as[2]) * m + as[3];
            const double b = ((bs[0] * m + bs[1]) * m + bs[2]) * m + bs[3];

            return { a, b };
        }

        int win_rate(int score, const Position& pos)
        {
            const Params p = params(pos);
            return static_cast<int>(std::round(1000.0 / (1.0 + std::exp((p.a - score) / p.b))));
        }

    }

    int material(const Position& pos)
    {
        return popcount(pos.pieces(PAWN))
            + 3 * popcount(pos.pieces(KNIGHT))
            + 3 * popcount(pos.pieces(BISHOP))
            + 5 * popcount(pos.pieces(ROOK))
            + 9 * popcount(pos.pieces(QUEEN));
    }

    int normalize_score(int score, const Position& pos)
    {
        if (score == 0)
            return 0;

        const Params p = params(pos);
        return static_cast<int>(std::round(100.0 * static_cast<double>(score) / p.a));
    }

    WDL model(int score, const Position& pos)
    {
        const int win = win_rate(score, pos);
        const int loss = win_rate(-score, pos);
        const int draw = std::clamp(1000 - win - loss, 0, 1000);

        return { win, draw, loss };
    }

}
