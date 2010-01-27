#include "daydreamer.h"
#include <string.h>
#include <math.h>

static const bool nullmove_enabled = true;
static const bool verification_enabled = true;
static const bool iid_enabled = true;
static const bool razoring_enabled = true;
static const bool futility_enabled = true;
static const bool history_prune_enabled = true;
static const bool value_prune_enabled = true;
static const bool qfutility_enabled = true;
static const bool lmr_enabled = true;

static const bool enable_pv_iid = true;
static const bool enable_non_pv_iid = false;
static const int iid_pv_depth_reduction = 2;
static const int iid_non_pv_depth_reduction = 2;
static const int iid_pv_depth_cutoff = 5;
static const int iid_non_pv_depth_cutoff = 8;

static const bool obvious_move_enabled = true;
static const int obvious_move_margin = 200;

static const int qfutility_margin = 80;
static const int futility_margin[FUTILITY_DEPTH_LIMIT] = { 100, 300, 500 };
static const int razor_margin[RAZOR_DEPTH_LIMIT] = { 300 };

static search_result_t root_search(search_data_t* search_data,
        int alpha,
        int beta);
static int search(position_t* pos,
        search_node_t* search_node,
        int ply,
        int alpha,
        int beta,
        int depth);
static int quiesce(position_t* pos,
        search_node_t* search_node,
        int ply,
        int alpha,
        int beta,
        int depth);

/*
 * Zero out all search variables prior to starting a search. Leaves the
 * position and search options untouched.
 */
void init_search_data(search_data_t* data)
{
    position_t root_pos_copy;
    copy_position(&root_pos_copy, &data->root_pos);
    memset(data, 0, sizeof(search_data_t));
    copy_position(&data->root_pos, &root_pos_copy);
    data->engine_status = ENGINE_IDLE;
    init_timer(&data->timer);
}

/*
 * Copy pv from a deeper search node, adding a new move at the current ply.
 */
static void update_pv(move_t* dst, move_t* src, int ply, move_t move)
{
    dst[ply] = move;
    int i=ply;
    do {
        ++i;
        dst[i] = src[i];
    } while (src[i] != NO_MOVE);
}

/*
 * Every time a node is expanded, this function increments the node counter.
 * Every POLL_INTERVAL nodes, user input, is checked.
 */
static void open_node(search_data_t* data, int ply)
{
    if ((++data->nodes_searched & POLL_INTERVAL) == 0) {
        if (should_stop_searching(data)) data->engine_status = ENGINE_ABORTED;
        uci_check_for_command();
        int so_far = elapsed_time(&data->timer);
        static int last_info = 0;
        if (so_far < 1000) {
            last_info = 0;
        } else if (so_far - last_info > 1000) {
            last_info = so_far;
            uint64_t nps = data->nodes_searched/so_far*1000;
            printf("info time %d nodes %"PRIu64, so_far, data->nodes_searched);
            if (options.verbose) printf(" qnodes %"PRIu64" pvnodes %"PRIu64,
                    data->qnodes_searched, data->pvnodes_searched);
            printf(" nps %"PRIu64" hashfull %d\n", nps, get_hashfull());
        }
    }
    data->search_stack[ply].killers[0] = NO_MOVE;
    data->search_stack[ply].killers[1] = NO_MOVE;
    data->search_stack[ply].mate_killer = NO_MOVE;
}

/*
 * Open a node in quiescent search.
 */
static void open_qnode(search_data_t* data, int ply)
{
    ++data->qnodes_searched;
    open_node(data, ply);
}

/*
 * Should we terminate the search? This considers time and node limits, as
 * well as user input. This function is checked periodically during search.
 */
bool should_stop_searching(search_data_t* data)
{
    if (data->engine_status == ENGINE_ABORTED) return true;
    if (data->engine_status == ENGINE_PONDERING || data->infinite) return false;
    int so_far = elapsed_time(&data->timer);

    // If we've passed our hard limit, we're done.
    if (data->time_limit && so_far >= data->time_limit) return true;

    // If we've passed our soft limit and just started a new iteration, stop.
    int real_target = data->time_target + data->time_bonus;
    if (data->time_target && so_far >= real_target &&
            data->current_move_index == 1) return true;

    // If we've passed our soft limit but we're in the middle of resolving a
    // fail high, try to resolve it before hitting the hard limit.
    if (data->time_target && !data->resolving_fail_high &&
            so_far > 4 * real_target) return true;

    // Respect node limits, if you're into that kind of thing.
    if (data->node_limit &&
            data->nodes_searched >= data->node_limit) return true;
    return false;
}

/*
 * Should the search depth be extended? Note that our move has
 * already been played in |pos|. For now, just extend one ply on checks
 * and pawn pushes to the 7th (relative) rank.
 * Note: |move| has already been made in |pos|. We need both anyway for
 * efficiency.
 * TODO: recapture extensions might be good. Also, fractional extensions,
 * and fractional plies in general.
 * TODO: test the value of pawn push extensions. Maybe limit the situations
 * in which the pushes are extended to pv?
 */
static int extend(position_t* pos, move_t move, bool single_reply)
{
    if (is_check(pos) || single_reply) return 1;
    square_t sq = get_move_to(move);
    if (piece_type(pos->board[sq]) == PAWN &&
            (square_rank(sq) == RANK_7 || square_rank(sq) == RANK_2)) return 1;
    return 0;
}

/*
 * Should we go on to the next level of iterative deepening in our root
 * search? This considers regular stopping conditions and also
 * tries to decide when we should stop early.
 */
static bool should_deepen(search_data_t* data)
{
    if (should_stop_searching(data)) return false;
    if (data->infinite || data->engine_status == ENGINE_PONDERING) return true;
    int so_far = elapsed_time(&data->timer);
    int real_target = data->time_target + data->time_bonus;
    
    // Allocate more search time when the root position is unclear.
    if (data->current_depth < 6) data->time_bonus = 0;
    else data->time_bonus = MAX(data->time_bonus,
            data->time_target * data->root_indecisiveness / 2);

    // If we're much more than halfway through our time, we won't make it
    // through the first move of the next iteration anyway.
    if (data->time_target &&
            real_target - so_far < real_target * 60 / 100) return false;

    // Go ahead and quit if we have a mate.
    int* scores = data->scores_by_iteration;
    int depth = data->current_depth;
    if (depth >= 4 && is_mate_score(abs(scores[depth--])) &&
            is_mate_score(abs(scores[depth--])) &&
            is_mate_score(abs(scores[depth--]))) return false;

    // We can stop early if our best move is obvious.
    if (!data->depth_limit && !data->node_limit && obvious_move_enabled &&
            data->current_depth >= 6 && data->obvious_move) return false;

    // Allocate some extra time when the root score drops.
    if (so_far < real_target / 3 || data->current_depth < 5) return true;
    int it_score = data->scores_by_iteration[data->current_depth];
    int last_it_score = data->scores_by_iteration[data->current_depth-1];
    if (it_score >= last_it_score) return true;
    else if (it_score >= last_it_score - 25) {
        data->time_bonus = MAX(data->time_bonus, data->time_target);
    } else if (it_score >= last_it_score - 50) {
        data->time_bonus = MAX(data->time_bonus, data->time_target * 3);
    } else {
        data->time_bonus = MAX(data->time_bonus, data->time_target * 7);
    }
    return true;
}

/*
 * Should we look up the current position in an endgame database?
 */
static bool should_probe_egbb(position_t* pos,
        int depth,
        int ply,
        int m50,
        int alpha,
        int beta)
{
    if (!options.use_egbb) return false;
    //TODO: evaluate 5 man bases
    if (pos->num_pieces[WHITE] + pos->num_pieces[BLACK] +
            pos->num_pawns[WHITE] + pos->num_pawns[BLACK] > 4) return false;
    if (is_mate_score(alpha) || is_mated_score(beta)) return false;
    return (m50 == 0 || (ply > 2*(depth + ply)/3));
}

/*
 * In the given position, is the nullmove heuristic valid? We avoid nullmoves
 * in cases where we're down to king and pawns because of zugzwang.
 */
static bool is_nullmove_allowed(position_t* pos)
{
    // don't allow nullmove if either side is in check
    if (is_check(pos)) return false;
    // allow nullmove if we're not down to king/pawns
    return !(pos->num_pieces[WHITE] == 1 && pos->num_pieces[BLACK] == 1);
}

/*
 * Point |data->current_root_move| at the structure representing |move|.
 */
static void set_current_root_move(search_data_t* data, move_t move)
{
    int i;
    for (i=0; data->root_moves[i].move != move &&
            data->root_moves[i].move != NO_MOVE; ++i) {}
    assert(data->root_moves[i].move == move);
    data->current_root_move = &data->root_moves[i];
}

/*
 * Record the number of nodes searched for a particular root move.
 */
static void store_root_data(search_data_t* data,
        move_t move,
        int score,
        uint64_t nodes_before)
{
    int i;
    for (i=0; data->root_moves[i].move != move &&
            data->root_moves[i].move != NO_MOVE; ++i) {}
    assert(data->root_moves[i].move == move);
    data->root_moves[i].nodes = data->nodes_searched - nodes_before;
    data->root_moves[i].score = score;
    data->root_moves[i].pv[0] = move;
    update_pv(data->root_moves[i].pv, data->search_stack->pv, 0, move);
}

/*
 * Get number of nodes searched for a root move in the last iteration.
 */
static uint64_t get_root_node_count(move_t move)
{
    int i;
    for (i=0; root_data.root_moves[i].move != move &&
            root_data.root_moves[i].move != NO_MOVE; ++i) {}
    assert(root_data.root_moves[i].move == move);
    return root_data.root_moves[i].nodes;
}

/*
 * Record quiet moves that cause fail-highs in the history table.
 */
static void record_success(history_t* h, move_t move, int depth)
{
    int index = history_index(move);
    h->history[index] += depth_to_history(depth);
    h->success[index]++;

    // Keep history values inside the correct range.
    if (h->history[index] > MAX_HISTORY) {
        for (int i=0; i<MAX_HISTORY_INDEX; ++i) h->history[i] /= 2;
    }
}

/*
 * Record quiet moves that cause failed to cause a fail-high on a fail-high
 * node in the history table.
 */
static void record_failure(history_t* h, move_t move)
{
    h->failure[history_index(move)]++;
}

/*
 * History heuristic for forward pruning.
 */
static bool is_history_prune_allowed(history_t* h, move_t move, int depth)
{
    int index = history_index(move);
    return depth * h->success[index] < h->failure[index];
}

/*
 * History heuristic for depth reduction.
 * TODO: try actually using this
 */
static bool is_history_reduction_allowed(history_t* h, move_t move)
{
    int index = history_index(move);
    return h->success[index] / 8 < h->failure[index];
}

/*
 * Can we do internal iterative deepening?
 */
static bool is_iid_allowed(bool full_window, int depth)
{
    if (full_window && (!enable_pv_iid ||
                iid_pv_depth_cutoff >= depth)) return false;
    else if (!enable_non_pv_iid ||
            iid_non_pv_depth_cutoff >= depth) return false;
    return true;
}

/*
 * Does the transposition table entry we found cause a cutoff?
 */
static bool is_trans_cutoff_allowed(transposition_entry_t* entry,
        int depth,
        int* alpha,
        int* beta)
{
    if (depth > entry->depth) return false;
    if (entry->flags & SCORE_LOWERBOUND && entry->score > *alpha) {
        *alpha = entry->score;
    }
    if (entry->flags & SCORE_UPPERBOUND && entry->score < *beta) {
        *beta = entry->score;
    }
    return *alpha >= *beta;
}

/*
 * Initialize a move at the root with the score of its depth-1 search.
 */
void init_root_move(root_move_t* root_move, move_t move)
{
    memset(root_move, 0, sizeof(root_move_t));
    root_move->move = move;
    undo_info_t undo;
    do_move(&root_data.root_pos, move, &undo);
    root_move->qsearch_score = -quiesce(&root_data.root_pos,
            root_data.search_stack, 1, mated_in(-1), mate_in(-1), 0);
    undo_move(&root_data.root_pos, move, &undo);
    root_move->pv[0] = move;
}

/*
 * Look for a root move that's better than its competitors by at least
 * |obvious_move_margin|. If there is one, and it consistently remains the
 * best move for the first several iterations, we just stop and return
 * the obvious move.
 */
void find_obvious_move(search_data_t* data)
{
    root_move_t* r = data->root_moves;
    int best_score = INT_MIN;
    for (int i=0; r[i].move; ++i) {
        if (r[i].qsearch_score > best_score) {
            best_score = r[i].qsearch_score;
            data->obvious_move = r[i].move;
        }
    }
    for (int i=0; r[i].move; ++i) {
        if (r[i].move == data->obvious_move) continue;
        if (r[i].qsearch_score + obvious_move_margin > best_score) {
            if (options.verbose && data->engine_status != ENGINE_PONDERING) {
                printf("info string no obvious move\n");
            }
            data->obvious_move = NO_MOVE;
            return;
        }
    }
    if (options.verbose && data->engine_status != ENGINE_PONDERING) {
        printf("info string candidate obvious move ");
        print_coord_move(data->obvious_move);
        printf("\n");
    }
}

/*
 * Iterative deepening search of the root position. This is the external
 * function that is called by the console interface. For each depth,
 * |root_search| performs the actual search.
 */
void deepening_search(search_data_t* search_data, bool ponder)
{
    search_data->engine_status = ponder ? ENGINE_PONDERING : ENGINE_THINKING;
    increment_transposition_age();
    init_timer(&search_data->timer);
    start_timer(&search_data->timer);

    // Get a move out of the opening book if we can.
    if (options.use_book && !search_data->infinite &&
            !search_data->depth_limit && !search_data->node_limit &&
            search_data->engine_status != ENGINE_PONDERING) {
        move_t book_move = get_book_move(&search_data->root_pos);
        if (book_move) {
            if (options.verbose) {
                printf("info string Found book move.\n");
            }
            char move_str[7];
            move_to_coord_str(book_move, move_str);
            printf("bestmove %s\n", move_str);
            search_data->engine_status = ENGINE_IDLE;
            return;
        }
    }

    // If |search_data| already has a list of root moves, we search only
    // those moves. Otherwise, search everything. This allows support for the
    // uci searchmoves command.
    if (search_data->root_moves[0].move == NO_MOVE) {
        move_t moves[256];
        generate_legal_moves(&search_data->root_pos, moves);
        for (int i=0; moves[i]; ++i) {
            init_root_move(&search_data->root_moves[i], moves[i]);
        }
    }
    find_obvious_move(search_data);

    int id_score = root_data.best_score = mated_in(-1);
    int consecutive_fail_highs = 0;
    int consecutive_fail_lows = 0;
    if (!search_data->depth_limit) search_data->depth_limit = MAX_SEARCH_DEPTH;
    for (search_data->current_depth=2;
            search_data->current_depth <= search_data->depth_limit;
            ++search_data->current_depth) {
        int depth = search_data->current_depth;
        if (should_output(search_data)) {
            if (options.verbose) print_transposition_stats();
            printf("info depth %d\n", depth);
        }

        // Calculate aspiration search window.
        int alpha = mated_in(-1);
        int beta = mate_in(-1);
        int last_score = search_data->scores_by_iteration[depth-1];
        if (depth > 5 && options.multi_pv == 1) {
            alpha = consecutive_fail_lows > 1 ? mated_in(-1) : last_score - 40;
            beta = consecutive_fail_highs > 1 ? mate_in(-1) : last_score + 40;
            if (options.verbose) {
                printf("info string root window is (%d, %d)\n", alpha, beta);
            }
        }
        search_data->root_indecisiveness = 0;

        search_result_t result = root_search(search_data, alpha, beta);
        if (result == SEARCH_ABORTED) break;

        // Replace any displaced pv entries in the hash table.
        score_type_t score_type = SCORE_EXACT;
        if (result == SEARCH_FAIL_LOW) score_type = SCORE_UPPERBOUND;
        else if (result == SEARCH_FAIL_HIGH) score_type = SCORE_LOWERBOUND;
        put_transposition_line(&search_data->root_pos,
                search_data->pv,
                depth,
                search_data->best_score,
                score_type);

        // Check the obvious move, if any.
        if (search_data->pv[0] != search_data->obvious_move) {
            search_data->obvious_move = NO_MOVE;
        }

        // Record scores.
        id_score = search_data->best_score;
        search_data->scores_by_iteration[depth] = id_score;
        if (id_score <= alpha) {
            consecutive_fail_lows++;
            consecutive_fail_highs = 0;
            search_data->root_indecisiveness += 3;
        } else if (id_score >= beta) {
            consecutive_fail_lows = 0;
            consecutive_fail_highs++;
            search_data->root_indecisiveness += 3;
        } else {
            consecutive_fail_lows = 0;
            consecutive_fail_highs = 0;
        }

        if (!should_deepen(search_data)) {
            ++search_data->current_depth;
            break;
        }
    }
    stop_timer(&search_data->timer);
    if (search_data->engine_status == ENGINE_PONDERING) uci_wait_for_command();

    --search_data->current_depth;
    search_data->best_score = id_score;
    if (options.verbose) {
        print_search_stats(search_data);
        printf("info string time target %d time limit %d elapsed time %d\n",
                search_data->time_target,
                search_data->time_limit,
                elapsed_time(&search_data->timer));
        print_transposition_stats();
        print_pawn_stats();
        print_pv_cache_stats();
        print_multipv(search_data);
    }
    char best_move[7], ponder_move[7];
    move_to_coord_str(search_data->pv[0], best_move);
    move_to_coord_str(search_data->pv[1], ponder_move);
    assert(search_data->pv[0] != NO_MOVE);
    printf("bestmove %s", best_move);
    if (search_data->pv[1]) printf(" ponder %s", ponder_move);
    printf("\n");
    search_data->engine_status = ENGINE_IDLE;
}

/*
 * Perform search at the root position. |search_data| contains all relevant
 * search information, which is set in |deepening_search|.
 * TODO: aspiration window?
 */
static search_result_t root_search(search_data_t* search_data,
        int alpha,
        int beta)
{
    int orig_alpha = alpha;
    search_data->best_score = alpha;
    position_t* pos = &search_data->root_pos;
    transposition_entry_t* trans_entry = get_transposition(pos);
    move_t hash_move = trans_entry ? trans_entry->move : NO_MOVE;

    move_selector_t selector;
    init_move_selector(&selector, pos, ROOT_GEN,
            NULL, hash_move, search_data->current_depth, 0);
    search_data->current_move_index = 0;
    search_data->resolving_fail_high = false;
    for (move_t move = select_move(&selector); move != NO_MOVE;
            move = select_move(&selector), ++search_data->current_move_index) {
        set_current_root_move(search_data, move);
        if (alpha >= beta) {
            // Fail high, bail out and try a bigger window.
            search_data->current_root_move->score = mated_in(-1);
            continue;
        }
        if (should_output(search_data)) {
            char coord_move[7];
            move_to_coord_str(move, coord_move);
            printf("info currmove %s currmovenumber %d\n",
                    coord_move, search_data->current_move_index);
        }
        uint64_t nodes_before = search_data->nodes_searched;
        undo_info_t undo;
        do_move(pos, move, &undo);
        int ext = extend(pos, move, false);
        int score;
        int depth = search_data->current_depth;
        int num_moves = search_data->current_move_index;

        if (search_data->current_move_index < options.multi_pv) {
            // Use full window search.
            alpha = mated_in(-1);
            score = -search(pos, search_data->search_stack,
                    1, -beta, -alpha, search_data->current_depth+ext-1);
        } else {
            const bool try_lmr = lmr_enabled &&
                num_moves > 10 &&
                !ext &&
                depth > LMR_DEPTH_LIMIT &&
                !is_check(pos);
            int lmr_red = try_lmr ? lmr_reduction(&selector, move) : 0;
            if (lmr_red) {
                score = -search(pos, search_data->search_stack,
                        1, -alpha-1, -alpha, depth-lmr_red-1);
            } else {
                score = -search(pos, search_data->search_stack,
                    1, -alpha-1, -alpha, search_data->current_depth+ext-1);
            }
            if (score > alpha) {
                if (score > alpha) {
                    if (options.verbose && should_output(search_data)) {
                        char coord_move[7];
                        move_to_coord_str(move, coord_move);
                        printf("info string fail high, research %s\n",
                                coord_move);
                    }
                    search_data->resolving_fail_high = true;
                    score = -search(pos, search_data->search_stack,
                            1, -beta, -alpha, search_data->current_depth+ext-1);
                }
            }
        }
        if (score <= alpha) {
            score = mated_in(-1);
        } else if (search_data->current_move_index >= options.multi_pv) {
            search_data->root_indecisiveness++;
        }
        store_root_data(search_data, move, score, nodes_before);
        undo_move(pos, move, &undo);
        if (search_data->engine_status == ENGINE_ABORTED) return SEARCH_ABORTED;
        if (score > alpha) {
            alpha = score;
            if (score > search_data->best_score) {
                search_data->best_score = score;
            }
            update_pv(search_data->pv, search_data->search_stack->pv, 0, move);
            check_line(pos, search_data->pv);
            print_multipv(search_data);
        }
        search_data->resolving_fail_high = false;
    }
    if (alpha == orig_alpha) {
        if (options.verbose && should_output(search_data)) {
            printf("info string Root search failed low, window was (%d, %d)\n",
                    alpha, beta);
        }
        search_data->stats.root_fail_lows++;
        return SEARCH_FAIL_LOW;
    } else if (alpha >= beta) {
        if (options.verbose && should_output(search_data)) {
            printf("info string Root search failed high, window was (%d, %d)\n",
                    orig_alpha, beta);
        }
        search_data->stats.root_fail_highs++;
        return SEARCH_FAIL_HIGH;
    }
    return SEARCH_EXACT;
}

/*
 * Search an interior, non-quiescent node.
 */
static int search(position_t* pos,
        search_node_t* search_node,
        int ply,
        int alpha,
        int beta,
        int depth)
{
    search_node->pv[ply] = NO_MOVE;
    if (root_data.engine_status == ENGINE_ABORTED) return 0;
    if (alpha > mate_in(ply)) return alpha; // Can't beat this...
    if (depth <= 0) {
        return quiesce(pos, search_node, ply, alpha, beta, depth);
    }
    if (is_draw(pos)) return DRAW_VALUE;
    bool full_window = (beta-alpha > 1);

    // Put some bounds on how good/bad this node could turn out to be.
    int orig_alpha = alpha;
    alpha = MAX(alpha, mated_in(ply));
    beta = MIN(beta, mate_in(ply));
    if (alpha >= beta) return alpha;

    // Get move from transposition table if possible.
    transposition_entry_t* trans_entry = get_transposition(pos);
    move_t hash_move = trans_entry ? trans_entry->move : NO_MOVE;
    bool mate_threat = trans_entry && trans_entry->flags & MATE_THREAT;
    if (!full_window && trans_entry &&
            is_trans_cutoff_allowed(trans_entry, depth, &alpha, &beta)) {
        search_node->pv[ply] = hash_move;
        search_node->pv[ply+1] = NO_MOVE;
        root_data.stats.transposition_cutoffs[root_data.current_depth]++;
        return MAX(alpha, trans_entry->score);
    }

    // Check endgame bitbases if appropriate
    int score;
    if (should_probe_egbb(pos, depth, ply,
                pos->fifty_move_counter, alpha, beta)) {
        if (probe_egbb(pos, &score, ply)) {
            ++root_data.stats.egbb_hits;
            return score;
        }
    }

    open_node(&root_data, ply);
    if (full_window) root_data.pvnodes_searched++;
    score = mated_in(-1);
    int lazy_score = simple_eval(pos);
    if (nullmove_enabled &&
            depth != 1 &&
            !mate_threat &&
            !full_window &&
            pos->prev_move != NULL_MOVE &&
            lazy_score + NULL_EVAL_MARGIN > beta &&
            !is_mate_score(beta) &&
            is_nullmove_allowed(pos)) {
        // Nullmove search.
        undo_info_t undo;
        do_nullmove(pos, &undo);
        int null_r = 2 + (depth + 2)/4;
        if (lazy_score - beta > PAWN_VAL) null_r++;
        int null_score = -search(pos, search_node+1, ply+1,
                -beta, -beta+1, depth - null_r);
        undo_nullmove(pos, &undo);
        if (is_mated_score(null_score)) mate_threat = true;
        if (null_score >= beta) {
            if (verification_enabled) {
                int rdepth = depth - NULLMOVE_VERIFICATION_REDUCTION;
                if (rdepth > 0) null_score = search(pos,
                        search_node, ply, alpha, beta, rdepth);
            }
            root_data.stats.nullmove_cutoffs[root_data.current_depth]++;
            if (null_score >= beta) return beta;
        }
    } else if (razoring_enabled &&
            pos->prev_move != NULL_MOVE &&
            !full_window &&
            depth <= RAZOR_DEPTH_LIMIT &&
            hash_move == NO_MOVE &&
            !is_mate_score(beta) &&
            lazy_score + razor_margin[depth-1] < beta) {
        // Razoring.
        root_data.stats.razor_attempts[depth-1]++;
        int qscore = quiesce(pos, search_node, ply, alpha, beta, 0);
        if (depth == 1 || qscore < beta) {
            root_data.stats.razor_prunes[depth-1]++;
            return qscore;
        }
    }

    // Internal iterative deepening.
    if (iid_enabled &&
            hash_move == NO_MOVE &&
            is_iid_allowed(full_window, depth)) {
        const int iid_depth = full_window ?
                depth - iid_pv_depth_reduction :
                MIN(depth / 2, depth - iid_non_pv_depth_reduction);
        assert(iid_depth > 0);
        search(pos, search_node, ply, alpha, beta, iid_depth);
        hash_move = search_node->pv[ply];
        search_node->pv[ply] = NO_MOVE;
    }

    move_t searched_moves[256];
    move_selector_t selector;
    init_move_selector(&selector, pos, full_window ? PV_GEN : NONPV_GEN,
            search_node, hash_move, depth, ply);
    // TODO: test extensions. Also try fractional extensions.
    bool single_reply = has_single_reply(&selector);
    int num_legal_moves = 0, num_futile_moves = 0, num_searched_moves = 0;
    int eval_score = lazy_score;
    for (move_t move = select_move(&selector); move != NO_MOVE;
            move = select_move(&selector)) {
        num_legal_moves = selector.moves_so_far;
        if (num_legal_moves == 2) eval_score = full_eval(pos);
        int64_t nodes_before = root_data.nodes_searched;

        undo_info_t undo;
        do_move(pos, move, &undo);
        int ext = extend(pos, move, single_reply);
        if (ext && defer_move(&selector, move)) {
            undo_move(pos, move, &undo);
            continue;
        }
        if (num_legal_moves == 1) {
            // First move, use full window search.
            score = -search(pos, search_node+1, ply+1,
                    -beta, -alpha, depth+ext-1);
        } else {
            // Futility pruning. Note: it would be nice to do extensions and
            // futility before calling do_move, but this would require more
            // efficient ways of identifying important moves without actually
            // making them.
            const bool prune_futile = futility_enabled &&
                !full_window &&
                !ext &&
                !mate_threat &&
                depth <= FUTILITY_DEPTH_LIMIT &&
                !is_check(pos) &&
                num_legal_moves >= depth + 2 &&
                should_try_prune(&selector, move);
            if (prune_futile) {
                // History pruning.
                if (history_prune_enabled && is_history_prune_allowed(
                            &root_data.history, move, depth)) {
                    num_futile_moves++;
                    undo_move(pos, move, &undo);
                    if (full_window) add_pv_move(&selector, move, 0);
                    continue;
                }
                // Value pruning.
                if (value_prune_enabled &&
                        eval_score + material_value(get_move_capture(move)) +
                        futility_margin[depth-1] < beta + 2*num_legal_moves) {
                    num_futile_moves++;
                    undo_move(pos, move, &undo);
                    if (full_window) add_pv_move(&selector, move, 0);
                    continue;
                }
            }
            // Late move reduction (LMR), as described by Tord Romstad at
            // http://www.glaurungchess.com/lmr.html
            const bool move_is_late = full_window ?
                num_legal_moves > LMR_PV_EARLY_MOVES :
                num_legal_moves > LMR_EARLY_MOVES;
            const bool try_lmr = lmr_enabled &&
                move_is_late &&
                !ext &&
                !mate_threat &&
                depth > LMR_DEPTH_LIMIT &&
                !is_check(pos);
            int lmr_red = try_lmr ? lmr_reduction(&selector, move) : 0;
            if (lmr_red) {
                score = -search(pos, search_node+1, ply+1,
                        -alpha-1, -alpha, depth-lmr_red-1);
            } else {
                score = alpha+1;
            }
            if (score > alpha) {
                score = -search(pos, search_node+1, ply+1,
                        -alpha-1, -alpha, depth+ext-1);
                if (score > alpha) {
                    score = -search(pos, search_node+1, ply+1,
                            -beta, -alpha, depth+ext-1);
                }
            }
        }
        searched_moves[num_searched_moves++] = move;
        undo_move(pos, move, &undo);
        if (full_window) add_pv_move(&selector, move,
                root_data.nodes_searched - nodes_before);
        if (score > alpha) {
            alpha = score;
            update_pv(search_node->pv, (search_node+1)->pv, ply, move);
            check_line(pos, search_node->pv+ply);
            if (score >= beta) {
                if (!get_move_capture(move) &&
                        !get_move_promote(move)) {
                    record_success(&root_data.history, move, depth);
                    for (int i=0; i<num_searched_moves-1; ++i) {
                        move_t m = searched_moves[i];
                        assert(m != move);
                        if (!get_move_capture(m) && !get_move_promote(m)) {
                            record_failure(&root_data.history, m);
                        }
                    }
                    if (move != search_node->killers[0]) {
                        search_node->killers[1] = search_node->killers[0];
                        search_node->killers[0] = move;
                    }
                }
                if (is_mate_score(score)) search_node->mate_killer = move;
                put_transposition(pos, move, depth, beta,
                        SCORE_LOWERBOUND, mate_threat);
                root_data.stats.move_selection[
                    MIN(num_legal_moves-1, HIST_BUCKETS)]++;
                if (full_window) {
                    root_data.stats.pv_move_selection[
                        MIN(num_legal_moves-1, HIST_BUCKETS)]++;
                    while ((move = select_move(&selector))) {
                        add_pv_move(&selector, move, 0);
                    }
                    commit_pv_moves(&selector);
                }
                search_node->pv[ply] = NO_MOVE;
                return beta;
            }
        }
    }
    if (full_window) commit_pv_moves(&selector);
    if (!num_legal_moves) {
        // No legal moves, this is either stalemate or checkmate.
        search_node->pv[ply] = NO_MOVE;
        if (is_check(pos)) return mated_in(ply);
        return DRAW_VALUE;
    }

    root_data.stats.move_selection[MIN(num_legal_moves-1, HIST_BUCKETS)]++;
    if (full_window) root_data.stats.pv_move_selection[
        MIN(num_legal_moves-1, HIST_BUCKETS)]++;
    if (alpha == orig_alpha) {
        put_transposition(pos, NO_MOVE, depth, alpha,
                SCORE_UPPERBOUND, mate_threat);
    } else {
        put_transposition(pos, search_node->pv[ply], depth, alpha,
                SCORE_EXACT, mate_threat);
    }
    return alpha;
}

/*
 * Search a position until it becomes "quiet". This is called at the leaves
 * of |search| to avoid using the static evaluator on positions that have
 * easy tactics on the board.
 */
static int quiesce(position_t* pos,
        search_node_t* search_node,
        int ply,
        int alpha,
        int beta,
        int depth)
{
    if (root_data.current_root_move &&
            ply > root_data.current_root_move->max_depth) {
        root_data.current_root_move->max_depth = ply;
    }
    search_node->pv[ply] = NO_MOVE;
    if (root_data.engine_status == ENGINE_ABORTED) return 0;
    if (alpha > mate_in(ply-1)) return alpha; // can't beat this
    if (is_draw(pos)) return DRAW_VALUE;
    bool full_window = (beta-alpha > 1);

    // Get move from transposition table if possible.
    int orig_alpha = alpha;
    transposition_entry_t* trans_entry = get_transposition(pos);
    move_t hash_move = trans_entry ? trans_entry->move : NO_MOVE;
    if (trans_entry && 
            is_trans_cutoff_allowed(trans_entry, depth, &alpha, &beta)) {
        search_node->pv[ply] = hash_move;
        search_node->pv[ply+1] = NO_MOVE;
        root_data.stats.transposition_cutoffs[root_data.current_depth]++;
        return MAX(alpha, trans_entry->score);
    }

    // Check endgame bitbases if appropriate
    int score;
    if (should_probe_egbb(pos, depth, ply,
                pos->fifty_move_counter, alpha, beta)) {
        if (probe_egbb(pos, &score, ply)) {
            ++root_data.stats.egbb_hits;
            return score;
        }
    }

    int eval = full_eval(pos);
    score = eval;
    if (ply >= MAX_SEARCH_DEPTH-1) return score;
    open_qnode(&root_data, ply);
    if (!is_check(pos)) {
        if (alpha < score) alpha = score;
        if (alpha >= beta) return beta;
    }

    bool allow_futility = qfutility_enabled &&
        !full_window &&
        !is_check(pos) &&
        pos->num_pieces[pos->side_to_move] > 2;
    int num_qmoves = 0;
    move_selector_t selector;
    generation_t gen_type = depth >= 0 && eval + 150 >= alpha ?
        Q_CHECK_GEN : Q_GEN;
    init_move_selector(&selector, pos, gen_type,
            search_node, hash_move, depth, ply);
    for (move_t move = select_move(&selector); move != NO_MOVE;
            move = select_move(&selector), ++num_qmoves) {
        // TODO: prevent futility for passed pawn moves and checks
        // TODO: no futility on early moves?
        if (allow_futility &&
                get_move_promote(move) != QUEEN &&
                eval + material_value(get_move_capture(move)) +
                qfutility_margin < alpha) continue;
        undo_info_t undo;
        do_move(pos, move, &undo);
        score = -quiesce(pos, search_node+1, ply+1, -beta, -alpha, depth-1);
        undo_move(pos, move, &undo);
        if (score > alpha) {
            alpha = score;
            update_pv(search_node->pv, (search_node+1)->pv, ply, move);
            check_line(pos, search_node->pv+ply);
            if (score >= beta) {
                put_transposition(pos, move, depth, beta,
                        SCORE_LOWERBOUND, false);
                return beta;
            }
        }
    }
    if (!num_qmoves && is_check(pos)) {
        return mated_in(ply);
    }
    if (alpha == orig_alpha) {
        put_transposition(pos, NO_MOVE, depth, alpha,
                SCORE_UPPERBOUND, false);
    } else {
        put_transposition(pos, search_node->pv[ply], depth, alpha,
                SCORE_EXACT, false);
    }
    return alpha;
}

