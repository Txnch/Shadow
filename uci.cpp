#include "uci.h"
#include "search.h"
#include "position.h"
#include "movegen.h"
#include "move.h"
#include "tt.h"
#include "evaluate.h"

#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstdint>

static const char* STARTPOS_FEN =
"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

static std::thread search_thread;
static std::atomic<bool> searching(false);

static constexpr int DEFAULT_MOVE_OVERHEAD_MS = 10;
static constexpr int MIN_MOVE_OVERHEAD_MS = 0;
static constexpr int MAX_MOVE_OVERHEAD_MS = 5000;
static int move_overhead_ms = DEFAULT_MOVE_OVERHEAD_MS;



static Move parse_move(Position& pos, const std::string& s)
{
    if (s.size() < 4)
        return 0;

    Square from = Square((s[1] - '1') * 8 + (s[0] - 'a'));
    Square to = Square((s[3] - '1') * 8 + (s[2] - 'a'));

    MoveList list;
    generate_moves(pos, list);

    for (int i = 0; i < list.size; ++i)
    {
        Move m = list.moves[i];

        if (from_sq(m) == from && to_sq(m) == to)
        {
            if (is_promotion(m))
            {
                if (s.size() < 5) continue;

                char promo = s[4];
                PieceType pt = promotion_type(m);

                if ((promo == 'q' && pt == QUEEN) ||
                    (promo == 'r' && pt == ROOK) ||
                    (promo == 'b' && pt == BISHOP) ||
                    (promo == 'n' && pt == KNIGHT))
                    return m;

                continue;
            }

            return m;
        }
    }

    return 0;
}




void uci_loop()
{
    Position pos;
    pos.set_fen(STARTPOS_FEN);

    std::string line;

    while (std::getline(std::cin, line))
    {

        if (line == "uci")
        {
            std::cout << "id name Shadow\n";
            std::cout << "id author TunCH\n";
            std::cout << "option name Hash type spin default " << tt_hash_mb() << " min 1 max 65536\n";
            std::cout << "option name Move Overhead type spin default "
                << DEFAULT_MOVE_OVERHEAD_MS << " min " << MIN_MOVE_OVERHEAD_MS
                << " max " << MAX_MOVE_OVERHEAD_MS << "\n";
            std::cout << "uciok\n" << std::flush;
        }


        else if (line == "isready")
        {
            std::cout << "readyok\n" << std::flush;
        }

        else if (line == "eval")
        {
            int raw_eval = evaluate(pos);

            std::cout << "--------------------------------\n";
            std::cout << "info string Pure NNUE Eval: " << raw_eval << " cp\n";
            std::cout << "info string Side to move: " << (pos.side_to_move() == WHITE ? "White" : "Black") << "\n";
            std::cout << "--------------------------------\n";
            std::cout << std::flush;
        }

        else if (line.rfind("setoption", 0) == 0)
        {
            std::istringstream ss(line);
            std::string token;
            std::string name;
            std::string value;

            ss >> token;
            if (!(ss >> token) || token != "name")
                continue;

            while (ss >> token && token != "value")
            {
                if (!name.empty())
                    name += " ";
                name += token;
            }

            if (token == "value")
                std::getline(ss, value);

            if (!value.empty() && value[0] == ' ')
                value.erase(0, 1);

            if (name == "Hash")
            {
                int mb = tt_hash_mb();
                if (!value.empty())
                {
                    try { mb = std::stoi(value); }
                    catch (...) {}
                }

                if (searching)
                {
                    stop_search_now();
                    if (search_thread.joinable())
                        search_thread.join();
                    searching = false;
                }

                if (tt_resize_mb(mb))
                    std::cout << "info string Hash set to " << tt_hash_mb() << " MB\n";
                else
                    std::cout << "info string Hash resize failed\n";
            }
            else if (name == "Move Overhead")
            {
                int overhead = move_overhead_ms;
                if (!value.empty())
                {
                    try { overhead = std::stoi(value); }
                    catch (...) {}
                }

                move_overhead_ms = std::clamp(overhead, MIN_MOVE_OVERHEAD_MS, MAX_MOVE_OVERHEAD_MS);
                std::cout << "info string Move Overhead set to " << move_overhead_ms << " ms\n";
            }
        }


        else if (line == "ucinewgame")
        {
            tt_clear();
            stop_search_now();


            if (search_thread.joinable())
                search_thread.join();

            searching = false;

            clear_search_state_for_new_game();
            pos.set_fen(STARTPOS_FEN);
        }


        else if (line.rfind("position", 0) == 0)
        {
            stop_search_now();

            if (search_thread.joinable())
                search_thread.join();

            searching = false;

            std::istringstream ss(line);
            std::string token;

            ss >> token;
            ss >> token;

            if (token == "startpos")
            {
                pos.set_fen(STARTPOS_FEN);
            }
            else if (token == "fen")
            {
                std::string fen, part;

                while (ss >> part)
                {
                    if (part == "moves")
                        break;

                    fen += part + " ";
                }

                pos.set_fen(fen);
                token = part;
            }

            if (token == "moves" || (ss >> token && token == "moves"))
            {
                while (ss >> token)
                {
                    Move m = parse_move(pos, token);

                    if (m == 0)
                    {
                        std::cout << "info string PARSE FAILED: "
                            << token << "\n";

                        MoveList list;
                        generate_moves(pos, list);

                        std::cout << "info string Legal moves were:\n";

                        for (int i = 0; i < list.size; ++i)
                        {
                            Move lm = list.moves[i];
                            Square f = from_sq(lm);
                            Square t = to_sq(lm);

                            std::cout << "info string   "
                                << char('a' + file_of(f))
                                << char('1' + rank_of(f))
                                << char('a' + file_of(t))
                                << char('1' + rank_of(t))
                                << "\n";
                        }

                        break;
                    }
                    if (!pos.make_move(m))
                    {
                        std::cout << "info string ILLEGAL MOVE IN UCI: "
                            << token << "\n";
                        break;
                    }


                }
            }
        }


        else if (line.rfind("go", 0) == 0)
        {
            if (searching)
                continue;

            int depth = 64;
            int movetime = -1;
            int wtime = -1, btime = -1;
            int winc = 0, binc = 0;
            int movestogo = 0;
            uint64_t nodes = 0;
            bool infinite = false;
            bool depth_only = false;

            std::istringstream ss(line);
            std::string token;
            ss >> token;

            while (ss >> token)
            {
                if (token == "depth") { ss >> depth; depth_only = true; }
                else if (token == "movetime") ss >> movetime;
                else if (token == "wtime")    ss >> wtime;
                else if (token == "btime")    ss >> btime;
                else if (token == "winc")     ss >> winc;
                else if (token == "binc")     ss >> binc;
                else if (token == "movestogo") ss >> movestogo;
                else if (token == "nodes")    ss >> nodes;
                else if (token == "infinite") infinite = true;
            }

            if (nodes > 0)
                infinite = false;


            int timeLeft = 0;
            int inc = 0;


            if (pos.side_to_move() == WHITE) {
                timeLeft = wtime;
                inc = winc;
            }
            else {
                timeLeft = btime;
                inc = binc;
            }

            if (depth_only && movetime <= 0 && wtime <= 0 && btime <= 0 && nodes == 0)
                infinite = true;

            if (infinite || (movetime <= 0 && wtime <= 0 && btime <= 0 && depth <= 0 && nodes == 0))
                infinite = true;

            stop_search_now();
            if (search_thread.joinable())
                search_thread.join();

            searching = true;
            Position search_pos = pos;
            int move_overhead = move_overhead_ms;

            search_thread = std::thread([search_pos, depth, movetime, timeLeft, inc, movestogo, infinite, nodes, move_overhead]() mutable
                {
                    SearchResult result = search(search_pos, depth, movetime, timeLeft, inc, movestogo, infinite, nodes, move_overhead);

                    if (result.best_move == 0)
                    {
                        std::cout << "bestmove 0000\n" << std::flush;
                    }
                    else
                    {
                        Square f = from_sq(result.best_move);
                        Square t = to_sq(result.best_move);

                        std::cout << "bestmove "
                            << char('a' + file_of(f))
                            << char('1' + rank_of(f))
                            << char('a' + file_of(t))
                            << char('1' + rank_of(t));

                        if (is_promotion(result.best_move))
                        {
                            PieceType pt = promotion_type(result.best_move);
                            if (pt == QUEEN)  std::cout << "q";
                            if (pt == ROOK)   std::cout << "r";
                            if (pt == BISHOP) std::cout << "b";
                            if (pt == KNIGHT) std::cout << "n";
                        }

                        std::cout << "\n" << std::flush;
                    }

                    searching = false;
                });
        }


        else if (line == "stop")
        {
            stop_search_now();

            if (search_thread.joinable())
                search_thread.join();

            searching = false;
        }


        else if (line == "quit")
        {
            stop_search_now();

            if (search_thread.joinable())
                search_thread.join();

            break;
        }
    }
}



