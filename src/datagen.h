#pragma once
#include <string>

namespace Shadow {
    namespace Datagen {
        void run(const std::string& output_path,
            const std::string& epd_path,
            int nodes_per_move,
            int target_games,
            int random_opening_plies = -1,
            int thread_count = 1);
    }
}
