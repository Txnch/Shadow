#include "datagen.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "tt.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <atomic>
#include <csignal>

inline constexpr int MATE_THRESHOLD = 28000;
inline constexpr int SCORE_CLAMP = 3000;
inline constexpr int WIN_ADJ_SCORE = 2000;
inline constexpr int WIN_ADJ_PLIES = 6;
inline constexpr int DRAW_ADJ_SCORE = 15;
inline constexpr int DRAW_ADJ_MIN_PLY = 64;
inline constexpr int DRAW_ADJ_PLIES = 7;
inline constexpr int OPENING_FILTER_NODES = 1000;
inline constexpr int OPENING_FILTER_MAX_ABS_SCORE = 1000;

inline bool is_mate_score(int s) {
    return std::abs(s) >= MATE_THRESHOLD;
}

extern bool has_insufficient_material(const Position& pos);

namespace Shadow {
    namespace Datagen {

        static std::atomic<bool> g_stop_datagen{ false };

        static void request_datagen_stop()
        {
            g_stop_datagen.store(true, std::memory_order_seq_cst);
            stop_search_now();
        }

        static void signal_handler(int)
        {
            request_datagen_stop();
        }

        static void install_stop_handlers()
        {
            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);
        }

        namespace {

            static constexpr const char* STARTPOS_FEN =
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

#pragma pack(push, 1)
            struct ViriPackedBoard {
                uint64_t occupancy = 0;
                uint8_t pieces[16]{};
                uint8_t stm_ep = 64;
                uint8_t halfmove_clock = 0;
                uint16_t fullmove_number = 0;
                int16_t score = 0;
                uint8_t result = 1;
                uint8_t extra = 0;
            };

            struct ViriMoveScore {
                uint16_t move = 0;
                int16_t score = 0;
            };
#pragma pack(pop)

            static_assert(sizeof(ViriPackedBoard) == 32);
            static_assert(sizeof(ViriMoveScore) == 4);

            static int16_t clamp_score_to_i16(int score)
            {
                return static_cast<int16_t>(std::clamp(score, -32767, 32767));
            }

            static uint8_t piece_code(Piece p)
            {
                uint8_t type = 0;

                switch (piece_type(p)) {
                case PAWN:   type = 0; break;
                case KNIGHT: type = 1; break;
                case BISHOP: type = 2; break;
                case ROOK:   type = 3; break;
                case QUEEN:  type = 4; break;
                case KING:   type = 5; break;
                default:     type = 0; break;
                }

                if (piece_color(p) == BLACK)
                    type |= 0x8;

                return type;
            }

            static bool is_castling_rook_square(const Position& pos, Square sq, Piece p)
            {
                if (piece_type(p) != ROOK)
                    return false;

                const Color c = piece_color(p);
                const int rights = pos.castling();

                if (c == WHITE) {
                    return ((sq == SQ_H1) && (rights & WHITE_OO))
                        || ((sq == SQ_A1) && (rights & WHITE_OOO));
                }

                if (c == BLACK) {
                    return ((sq == SQ_H8) && (rights & BLACK_OO))
                        || ((sq == SQ_A8) && (rights & BLACK_OOO));
                }

                return false;
            }

            static ViriPackedBoard pack_board(const Position& pos, int white_relative_score, uint8_t result)
            {
                ViriPackedBoard out{};
                out.occupancy = pos.all_pieces();

                Bitboard occ = out.occupancy;
                int piece_idx = 0;
                while (occ) {
                    const Square sq = pop_lsb(occ);
                    const Piece p = pos.piece_on(sq);
                    if (p == NO_PIECE)
                        continue;

                    uint8_t code = piece_code(p);
                    if (is_castling_rook_square(pos, sq, p))
                        code = static_cast<uint8_t>((code & 0x8) | 6);

                    if (piece_idx < 32) {
                        const int byte_idx = piece_idx / 2;
                        if ((piece_idx & 1) == 0)
                            out.pieces[byte_idx] |= code;
                        else
                            out.pieces[byte_idx] |= static_cast<uint8_t>(code << 4);
                    }

                    ++piece_idx;
                }

                const Square ep = pos.ep_square();
                const uint8_t ep_field = (ep == SQ_NONE) ? uint8_t(64) : uint8_t(ep);
                out.stm_ep = static_cast<uint8_t>(ep_field | (pos.side_to_move() == BLACK ? 0x80 : 0));
                out.halfmove_clock = static_cast<uint8_t>(std::clamp(pos.halfmove_clock(), 0, 255));
                out.fullmove_number = 0;
                out.score = clamp_score_to_i16(white_relative_score);
                out.result = result;
                out.extra = 0;

                return out;
            }

            static uint8_t promotion_code(PieceType pt)
            {
                switch (pt) {
                case KNIGHT: return 0;
                case BISHOP: return 1;
                case ROOK:   return 2;
                case QUEEN:  return 3;
                default:     return 0;
                }
            }

            static uint16_t encode_viri_move(Move m, Color us)
            {
                Square from = from_sq(m);
                Square to = to_sq(m);
                uint16_t move_type = 0;
                uint16_t promo = 0;

                if (is_en_passant(m)) {
                    move_type = 1;
                }
                else if (is_castling(m)) {
                    move_type = 2;

                    if (us == WHITE)
                        to = (to == SQ_G1) ? SQ_H1 : SQ_A1;
                    else
                        to = (to == SQ_G8) ? SQ_H8 : SQ_A8;
                }
                else if (is_promotion(m)) {
                    move_type = 3;
                    promo = promotion_code(promotion_type(m));
                }

                return static_cast<uint16_t>(
                    (uint16_t(from) & 0x3F)
                    | ((uint16_t(to) & 0x3F) << 6)
                    | ((promo & 0x3) << 12)
                    | ((move_type & 0x3) << 14));
            }

            static MoveList get_legal_moves(Position& pos)
            {
                MoveList pseudo;
                MoveList legal;
                generate_moves(pos, pseudo);

                for (int i = 0; i < pseudo.size; ++i) {
                    const Move m = pseudo.moves[i];
                    if (pos.make_move(m)) {
                        legal.push(m);
                        pos.undo_move();
                    }
                }

                return legal;
            }

            static bool write_game(std::ofstream& out,
                ViriPackedBoard start_board,
                const std::vector<ViriMoveScore>& moves,
                uint8_t result)
            {
                start_board.result = result;
                const ViriMoveScore terminator{};

                out.write(reinterpret_cast<const char*>(&start_board), sizeof(start_board));

                if (!moves.empty())
                    out.write(reinterpret_cast<const char*>(moves.data()),
                        static_cast<std::streamsize>(moves.size() * sizeof(ViriMoveScore)));

                out.write(reinterpret_cast<const char*>(&terminator), sizeof(terminator));
                return static_cast<bool>(out);
            }

            static bool passes_opening_filter(Position& pos)
            {
                clear_search_state_for_new_game();
                tt_clear();

                const SearchResult res = search(pos, 64, -1, 0, 0, 0, true, OPENING_FILTER_NODES, 10, true);
                if (g_stop_datagen.load(std::memory_order_relaxed))
                    return false;

                return res.best_move != 0
                    && !is_mate_score(res.score)
                    && std::abs(res.score) <= OPENING_FILTER_MAX_ABS_SCORE;
            }

        } // namespace

        void run(const std::string& output_path,
            const std::string& epd_path,
            int nodes_per_move,
            int target_games,
            int random_opening_plies)
        {
            if (target_games <= 0) {
                std::cerr << "Datagen: target games must be greater than zero\n";
                return;
            }

            if (nodes_per_move <= 0) {
                std::cerr << "Datagen: nodes per move must be greater than zero\n";
                return;
            }

            if (random_opening_plies < -1) {
                std::cerr << "Datagen: random opening plies must be -1, zero, or greater\n";
                return;
            }

            g_silent_search = true;
            std::mt19937 rng(std::random_device{}());

            install_stop_handlers();
            g_stop_datagen.store(false);

            std::vector<std::string> opening_fens;
            bool use_epd = false;

            if (epd_path != "none") {
                std::ifstream epd_file(epd_path);
                if (epd_file) {
                    std::string line;
                    while (std::getline(epd_file, line)) {
                        if (!line.empty())
                            opening_fens.push_back(line);
                    }
                    epd_file.close();

                    if (!opening_fens.empty()) {
                        use_epd = true;
                        std::cerr << "Loaded " << opening_fens.size() << " openings from " << epd_path << "\n";
                        if (random_opening_plies >= 0) {
                            std::cerr << "Applying " << random_opening_plies
                                << " random legal plies after each opening.\n";
                        }
                    }
                }
                else {
                    std::cerr << "Warning: Could not open EPD file (" << epd_path << "). Falling back to random moves.\n";
                }
            }
            else {
                if (random_opening_plies >= 0) {
                    std::cerr << "EPD set to 'none'. Using "
                        << random_opening_plies << " random legal plies.\n";
                }
                else {
                    std::cerr << "EPD set to 'none'. Using standard random opening (8-9 plies).\n";
                }
            }

            std::ofstream out(output_path, std::ios::binary | std::ios::app);
            if (!out) {
                std::cerr << "Datagen: failed to open " << output_path << "\n";
                return;
            }

            tt_resize_mb(16);

            uint64_t games_played = 0;
            uint64_t positions_saved = 0;
            uint64_t opening_filter_rejected = 0;

            std::cerr << "Starting ViriFormat Datagen: " << target_games
                << " games at " << nodes_per_move << " nodes/move\n";
            std::cerr << "Opening filter: " << OPENING_FILTER_NODES
                << " nodes, max |score| " << OPENING_FILTER_MAX_ABS_SCORE << "\n";

            const auto total_start_time = std::chrono::steady_clock::now();
            auto batch_start_time = std::chrono::steady_clock::now();

            std::uniform_int_distribution<size_t> epd_dist(0, opening_fens.empty() ? 0 : opening_fens.size() - 1);

            while (games_played < target_games && !g_stop_datagen.load(std::memory_order_relaxed)) {
                Position pos;
                bool valid = true;
                int open_plies = 0;

                if (use_epd) {
                    pos.set_fen(opening_fens[epd_dist(rng)]);
                    if (random_opening_plies >= 0)
                        open_plies = random_opening_plies;
                }
                else {
                    pos.set_fen(STARTPOS_FEN);
                    open_plies = random_opening_plies;
                    if (open_plies < 0) {
                        std::uniform_int_distribution<int> open_dist(8, 9);
                        open_plies = open_dist(rng);
                    }
                }

                for (int i = 0; i < open_plies; ++i) {
                    MoveList moves = get_legal_moves(pos);
                    if (moves.size == 0) {
                        valid = false;
                        break;
                    }

                    std::uniform_int_distribution<int> move_dist(0, moves.size - 1);
                    if (!pos.make_move(moves.moves[move_dist(rng)])) {
                        valid = false;
                        break;
                    }
                }

                if (!valid)
                    continue;

                if (!passes_opening_filter(pos)) {
                    if (g_stop_datagen.load(std::memory_order_relaxed))
                        break;
                    ++opening_filter_rejected;
                    continue;
                }

                uint8_t result = 1;

                clear_search_state_for_new_game();
                tt_clear();

                ViriPackedBoard start_board = pack_board(pos, 0, result);
                std::vector<ViriMoveScore> game_moves;
                game_moves.reserve(128);
                bool completed_game = false;

                int win_plies = 0;
                int loss_plies = 0;
                int draw_plies = 0;

                for (int ply = 0; ply < 400; ++ply) {
                    if (g_stop_datagen.load(std::memory_order_relaxed)) {
                        game_moves.clear();
                        break;
                    }

                    SearchResult res = search(pos, 64, -1, 0, 0, 0, true, nodes_per_move, 10, true);

                    if (g_stop_datagen.load(std::memory_order_relaxed)) {
                        game_moves.clear();
                        break;
                    }

                    if (res.best_move == 0) {
                        MoveList moves = get_legal_moves(pos);
                        if (moves.size == 0) {
                            if (in_check(pos, pos.side_to_move()))
                                result = (pos.side_to_move() == WHITE) ? 0 : 2;
                            else
                                result = 1;
                            completed_game = true;
                        }
                        else {
                            game_moves.clear();
                        }
                        break;
                    }

                    if (pos.halfmove_clock() >= 100 || pos.is_repetition_draw(0) || has_insufficient_material(pos)) {
                        result = 1;
                        completed_game = true;
                        break;
                    }

                    const Color stm = pos.side_to_move();
                    const int white_relative_score = (stm == WHITE) ? res.score : -res.score;
                    const Piece moved_piece = pos.piece_on(from_sq(res.best_move));
                    const bool resets_draw_counter = is_capture(res.best_move) || piece_type(moved_piece) == PAWN;

                    ViriMoveScore move_score{};
                    move_score.move = encode_viri_move(res.best_move, stm);

                    if (is_mate_score(res.score)) {
                        result = (res.score > 0)
                            ? ((stm == WHITE) ? 2 : 0)
                            : ((stm == WHITE) ? 0 : 2);

                        int mate_cp = (res.score > 0) ? SCORE_CLAMP : -SCORE_CLAMP;
                        if (stm == BLACK) mate_cp = -mate_cp;
                        move_score.score = clamp_score_to_i16(mate_cp);

                        game_moves.push_back(move_score);
                        completed_game = true;
                        break;
                    }

                    move_score.score = clamp_score_to_i16(
                        std::clamp(white_relative_score, -SCORE_CLAMP, SCORE_CLAMP));
                    game_moves.push_back(move_score);

                    if (white_relative_score > WIN_ADJ_SCORE) {
                        win_plies++;
                        loss_plies = draw_plies = 0;
                    }
                    else if (white_relative_score < -WIN_ADJ_SCORE) {
                        loss_plies++;
                        win_plies = draw_plies = 0;
                    }
                    else {
                        win_plies = loss_plies = 0;

                        if (resets_draw_counter)
                            draw_plies = 0;
                        else if (ply >= DRAW_ADJ_MIN_PLY && std::abs(white_relative_score) < DRAW_ADJ_SCORE)
                            draw_plies++;
                        else
                            draw_plies = 0;
                    }

                    if (win_plies >= WIN_ADJ_PLIES) {
                        result = 2;
                        completed_game = true;
                        break;
                    }
                    if (loss_plies >= WIN_ADJ_PLIES) {
                        result = 0;
                        completed_game = true;
                        break;
                    }
                    if (draw_plies >= DRAW_ADJ_PLIES) {
                        result = 1;
                        completed_game = true;
                        break;
                    }

                    if (!pos.make_move(res.best_move)) {
                        game_moves.clear();
                        break;
                    }
                }

                if (completed_game && !g_stop_datagen.load(std::memory_order_relaxed) && !game_moves.empty()) {
                    if (!write_game(out, start_board, game_moves, result)) {
                        std::cerr << "Datagen: failed while writing to " << output_path << "\n";
                        break;
                    }

                    positions_saved += static_cast<uint64_t>(game_moves.size());
                    ++games_played;

                    if (games_played % 10 == 0) {
                        const auto batch_end_time = std::chrono::steady_clock::now();
                        const std::chrono::duration<double> batch_elapsed = batch_end_time - batch_start_time;

                        std::cerr << "Played: " << games_played
                            << " games | Positions: " << positions_saved
                            << " | Filter rejects: " << opening_filter_rejected
                            << " | Time: " << batch_elapsed.count() << " s\n";

                        batch_start_time = std::chrono::steady_clock::now();
                    }
                }
            }

            const auto total_end_time = std::chrono::steady_clock::now();
            const std::chrono::duration<double> total_elapsed = total_end_time - total_start_time;

            out.close();
            std::cerr << "Datagen complete! Saved " << positions_saved
                << " ViriFormat positions to " << output_path << "\n";
            std::cerr << "Opening filter rejected: " << opening_filter_rejected << " candidate games.\n";
            std::cerr << "Total time elapsed: " << total_elapsed.count() << " seconds.\n";
        }

    } // namespace Datagen
} // namespace Shadow
