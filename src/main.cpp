#include <exception>
#include <iostream>
#include <string>

#include "attacks.h"
#include "bitboard.h"
#include "position.h"
#include "nnue.h"
#include "uci.h"
#include "timeman.h"
#include "datagen.h"


int main(int argc, char* argv[])
{
    try
    {
        init_attacks();
        init_magic();
        init_zobrist();

        const bool nnueLoaded = nnue::init("quantised.bin");
        if (nnueLoaded)
            std::cout << "info string NNUE load success: quantised.bin\n";
        else
            std::cout << "info string NNUE load failed\n";


        if (argc >= 2 && std::string(argv[1]) == "datagen") {
            int games = (argc >= 3) ? std::stoi(argv[2]) : 1000;
            std::string output = (argc >= 4) ? argv[3] : "train.data";

            std::string epd_file = (argc >= 5) ? argv[4] : "none";
            int nodes = (argc >= 6) ? std::stoi(argv[5]) : 5000;
            int random_opening_plies = (argc >= 7) ? std::stoi(argv[6]) : -1;


            Shadow::Datagen::run(output, epd_file, nodes, games, random_opening_plies);
            return 0;
        }


        uci_loop();

        return 0;
    }
    catch (...)
    {
        return 1;
    }
}
