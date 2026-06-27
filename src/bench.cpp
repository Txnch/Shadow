#include "bench.h"
#include "move.h"
#include "position.h"
#include "search.h"
#include "tt.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <fstream>   
#include <vector>    
#include <streambuf>
#include <string>
#include <thread>
#include <utility>


int evaluate(const Position& pos);
int raw_eval = 0;

namespace Shadow::Bench {
    namespace {

        constexpr int DEFAULT_BENCH_DEPTH = 10;
        constexpr int MIN_BENCH_DEPTH = 1;
        constexpr int MAX_BENCH_DEPTH = 64;

        constexpr const char* BENCH_FENS[] = {
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            "r3k2r/p1ppqpb1/bn2pnp1/2pPN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
            "4rrk1/1pp1qppp/p1np1n2/4p3/2B1P3/2NP1N2/PPP2PPP/R2QR1K1 w - - 0 12",
            "2rq1rk1/pb3ppp/1pnpbn2/3Np3/2P1P3/2N1B3/PP2BPPP/R2Q1RK1 b - - 3 11",
            "r2q1rk1/pp1nbppp/2p1pn2/3p4/3P4/2NBPN2/PPQ2PPP/R1B2RK1 w - - 0 10",
            "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
            "8/5pk1/6p1/1p1Pp2p/pP2P2P/P5P1/5PK1/8 w - - 0 40",
            "r4rk1/1pp1qppp/p1npbn2/4p3/2B1P3/2NP1N2/PPP2PPP/R2QR1K1 b - - 4 12",
            "2r2rk1/1bqnbppp/p2ppn2/1p6/3NP3/1BN1B3/PPP2PPP/2RQ1RK1 w - - 4 13",
            "6k1/5ppp/8/8/8/8/5PPP/6K1 w - - 0 1",
            "r1bq1rk1/ppp2ppp/2nbpn2/3p4/3P4/2PBPN2/PP3PPP/RNBQ1RK1 w - - 2 7",
            "3r2k1/pp3ppp/2n1pn2/2qp4/8/2P1PN2/PPQ2PPP/3R1RK1 w - - 0 18",
        };

        static_assert(sizeof(BENCH_FENS) / sizeof(BENCH_FENS[0]) > 0);

        int clamp_depth(int depth)
        {
            if (depth <= 0)
                depth = DEFAULT_BENCH_DEPTH;

            return std::clamp(depth, MIN_BENCH_DEPTH, MAX_BENCH_DEPTH);
        }

        uint64_t mix_signature(uint64_t signature, const SearchResult& result)
        {
            constexpr uint64_t prime = 1099511628211ULL;

            signature ^= result.nodes + 0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2);
            signature *= prime;
            signature ^= uint64_t(uint32_t(result.best_move));
            signature *= prime;
            signature ^= uint64_t(uint32_t(result.score));
            signature *= prime;
            signature ^= uint64_t(uint32_t(result.depth));
            signature *= prime;

            return signature;
        }

        void print_move(std::ostream& out, Move move)
        {
            if (move == 0) {
                out << "0000";
                return;
            }

            const Square from = from_sq(move);
            const Square to = to_sq(move);

            out << char('a' + file_of(from))
                << char('1' + rank_of(from))
                << char('a' + file_of(to))
                << char('1' + rank_of(to));

            if (is_promotion(move)) {
                const PieceType pt = promotion_type(move);
                if (pt == QUEEN) out << 'q';
                else if (pt == ROOK) out << 'r';
                else if (pt == BISHOP) out << 'b';
                else if (pt == KNIGHT) out << 'n';
            }
        }

        class SearchInfoGuard {
        public:
            SearchInfoGuard()
                : previous_(std::exchange(g_silent_search, false))
            {
            }

            ~SearchInfoGuard()
            {
                g_silent_search = previous_;
            }

            SearchInfoGuard(const SearchInfoGuard&) = delete;
            SearchInfoGuard& operator=(const SearchInfoGuard&) = delete;

        private:
            bool previous_;
        };

        class NullBuffer : public std::streambuf {
        public:
            int_type overflow(int_type ch) override
            {
                return ch;
            }
        };

        class CoutSilencer {
        public:
            CoutSilencer()
                : previous_(std::cout.rdbuf(&null_buffer_))
            {
            }

            ~CoutSilencer()
            {
                std::cout.rdbuf(previous_);
            }

            CoutSilencer(const CoutSilencer&) = delete;
            CoutSilencer& operator=(const CoutSilencer&) = delete;

        private:
            NullBuffer null_buffer_;
            std::streambuf* previous_;
        };


        std::vector<std::string> load_fens(std::ostream& out) {
            std::vector<std::string> fens;
            std::ifstream file("fen.txt");

            if (file.is_open()) {
                std::string line;
                while (std::getline(file, line)) {
                    if (!line.empty()) {
                        fens.push_back(line);

                        if (fens.size() >= 3000000) {
                            break;
                        }
                    }
                }
            }

            if (fens.empty()) {
                for (const char* fen : BENCH_FENS) {
                    fens.push_back(fen);
                }
                out << "info string loaded default FENs\n" << std::flush;
            }
            else {
                out << "info string loaded " << fens.size() << " FENs from fen.txt\n" << std::flush;
            }
            return fens;
        }

    } // namespace


    Result run_fens(std::ostream& out, int requested_depth, const std::vector<std::string>& fens)
    {
        const int depth = clamp_depth(requested_depth);
        const int positions = int(fens.size());

        Result result{};
        result.depth = depth;
        result.positions = positions;
        result.signature = 1469598103934665603ULL;

        SearchInfoGuard search_info_guard;

        long long total_search_score = 0;
        double total_absolute_eval = 0.0;

        out << "info string bench depth " << depth
            << " positions " << positions << "\n" << std::flush;

        int successful_positions = 0;

        for (int i = 0; i < positions; ++i) {
            SearchResult search_result{};
            int elapsed_ms = 0;
            std::string error;

            std::thread worker([&, i] {
                try {
                    Position pos;
                    pos.set_fen(fens[i].c_str());

                    clear_search_state_for_new_game();
                    tt_clear();

                    raw_eval = evaluate(pos);
                    const auto start = std::chrono::steady_clock::now();
                    {
                        CoutSilencer silence_search_info;
                        search_result = search(
                            pos,
                            depth,
                            -1,
                            0,
                            0,
                            0,
                            true);
                    }
                    const auto end = std::chrono::steady_clock::now();
                    elapsed_ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
                }
                catch (const std::exception& e) {
                    error = e.what();
                }
                catch (...) {
                    error = "unknown exception";
                }
                });
            worker.join();

            if (!error.empty()) {
                out << "info string bench error position " << (i + 1) << " (skipping)\n" << std::flush;
                continue;
            }

            if (search_result.depth <= 0 && search_result.best_move == 0 && search_result.nodes == 0) {
                out << "info string bench warning position " << (i + 1) << " no search result (skipping)\n" << std::flush;
                continue;
            }

            successful_positions++;

            const uint64_t nps = elapsed_ms > 0 ? search_result.nodes * 1000ULL / uint64_t(elapsed_ms) : 0ULL;

            total_search_score += search_result.score;
            total_absolute_eval += std::abs(raw_eval);

            result.elapsed_ms += elapsed_ms;
            result.nodes += search_result.nodes;
            result.signature = mix_signature(result.signature, search_result);

            out << "info string bench " << (i + 1) << "/" << positions
                << " depth " << search_result.depth
                << " nodes " << search_result.nodes
                << " time " << elapsed_ms
                << " nps " << nps
                << " score " << search_result.score
                << " bestmove ";
            print_move(out, search_result.best_move);
            out << "\n" << std::flush;
        }

        result.nps = result.elapsed_ms > 0 ? result.nodes * 1000ULL / uint64_t(result.elapsed_ms) : 0ULL;
        int average_search_score = successful_positions > 0 ? (total_search_score / successful_positions) : 0;
        double average_abs_eval = successful_positions > 0 ? total_absolute_eval / successful_positions : 0.0;
        result.positions = successful_positions;

        out << "info string bench total"
            << " depth " << result.depth
            << " positions " << result.positions
            << " nodes " << result.nodes
            << " time " << result.elapsed_ms
            << " nps " << result.nps
            << " avg_score " << average_search_score
            << " avg_abs_eval " << average_abs_eval
            << " signature " << result.signature
            << "\n" << std::flush;

        return result;
    }

    Result run(std::ostream& out, int requested_depth)
    {
        std::vector<std::string> fens = load_fens(out);
        return run_fens(out, requested_depth, fens);
    }


    Result run_eval_fens(std::ostream& out, const std::vector<std::string>& fens)
    {
        const int positions = int(fens.size());
        Result result{};

        double total_absolute_eval = 0.0;
        int successful_positions = 0;

        out << "info string bench eval starting for " << positions << " positions\n" << std::flush;

        const auto start = std::chrono::steady_clock::now();


        for (int i = 0; i < positions; ++i) {
            std::string error;
            try {
                Position pos;
                pos.set_fen(fens[i].c_str());

                int eval = evaluate(pos);
                total_absolute_eval += std::abs(eval);
                successful_positions++;
            }
            catch (const std::exception& e) {
                error = e.what();
            }
            catch (...) {
                error = "unknown exception";
            }

            if (!error.empty()) {
                out << "info string bench eval error position " << (i + 1)
                    << " exception " << error
                    << " (skipping)\n" << std::flush;
                continue;
            }


            if ((i + 1) % 100000 == 0 || (i + 1) == positions) {
                out << "info string bench eval progress " << (i + 1) << "/" << positions << "\n" << std::flush;
            }
        }

        const auto end = std::chrono::steady_clock::now();
        result.elapsed_ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        result.positions = successful_positions;

        double average_abs_eval = successful_positions > 0 ? total_absolute_eval / successful_positions : 0.0;


        out << "info string bench eval total"
            << " positions " << result.positions
            << " time " << result.elapsed_ms << "ms"
            << " avg_abs_eval " << average_abs_eval
            << "\n" << std::flush;

        return result;
    }

    Result run_eval(std::ostream& out)
    {
        std::vector<std::string> fens = load_fens(out);
        return run_eval_fens(out, fens);
    }

} // namespace Shadow::Bench