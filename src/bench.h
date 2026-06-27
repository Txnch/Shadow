#pragma once

#include <cstdint>
#include <iosfwd>

namespace Shadow::Bench {

    struct Result {
        int depth = 0;
        int positions = 0;
        int elapsed_ms = 0;
        uint64_t nodes = 0;
        uint64_t nps = 0;
        uint64_t signature = 0;
    };

    Result run(std::ostream& out, int depth);
    Result run_eval(std::ostream& out);

} // namespace Shadow::Bench
