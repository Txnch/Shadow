#pragma once

#include "move.h"
#include <cstdint>

namespace Cuckoo {


    using Key = uint64_t;

    inline int h1(Key key) {
        return key & 0x1fff;
    }

    inline int h2(Key key) {
        return (key >> 16) & 0x1fff;
    }

    extern Key keys[8192];
    extern Move moves[8192];

    inline Move lookup(Key k) {
        int s1 = h1(k);
        if (keys[s1] == k)
            return moves[s1];

        int s2 = h2(k);
        if (keys[s2] == k)
            return moves[s2];

        return Move(0);
    }

    void init();
}