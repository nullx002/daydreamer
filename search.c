
#include <stdio.h>
#include <strings.h>
#include "daydreamer.h"

#define should_output(s)    \
    (elapsed_time(&((s)->timer)) > (s)->options.output_delay)


search_data_t root_data;

static bool should_stop_searching(search_data_t* data);
static bool root_search(search_data_t* search_data);
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

void init_search_data(search_data_t* data)
{
    memset(data->root_moves, 0, sizeof(move_t) * 256);
    memset(data->root_move_scores, 0, sizeof(int) * 256);
    memset(data->root_move_depths, 0, sizeof(int) * 256);
    memset(data->root_move_types, 0, sizeof(score_type_t) * 256);
    data->best_move = NO_MOVE;
    data->best_score = 0;
    memset(data->pv, 0, sizeof(move_t) * MAX_SEARCH_DEPTH);
    memset(data->search_stack, 0, sizeof(search_node_t) * MAX_SEARCH_DEPTH);
    data->nodes_searched = 0;
    data->current_depth = 0;
    data->engine_status = ENGINE_IDLE;
    init_timer(&data->timer);
    data->node_limit = 0;
    data->depth_limit = 0;
    data->time_limit = 0;
    data->time_target = 0;
    data->mate_search = 0;
    data->infinite = 0;
    data->ponder = 0;
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
static void open_node(search_data_t* data)
{
    if ((++data->nodes_searched & POLL_INTERVAL) == 0) {
        if (should_stop_searching(data)) data->engine_status = ENGINE_ABORTED;
        check_for_input(data);
    }
}

/*
 * Should we terminate the search? This considers time and node limits, as
 * well as user input. This function is checked periodically during search.
 */
static bool should_stop_searching(search_data_t* data)
{
    if (data->engine_status == ENGINE_ABORTED) return true;
    if (!data->infinite &&
            data->time_target &&
            elapsed_time(&data->timer) >= data->time_target) return true;
    // TODO: take time_limit and search difficulty into account
    if (data->node_limit &&
            data->nodes_searched >= data->node_limit) return true;
    // TODO: we need a heuristic for when the current result is "good enough",
    // regardless of search params.
    return false;
}

/*
 * Should the search depth be extended? Note that our move has
 * already been played in |pos|. For now, just extend one ply on checks
 * and pawn pushes to the 7th rank.
 * Note: |move| has already been made in |pos|. We need both anyway for
 * efficiency.
 */
static int extend(position_t* pos, move_t move)
{
    if (is_check(pos)) return 1;
    square_t sq = get_move_to(move);
    if (piece_type(pos->board[sq].piece) == PAWN &&
            square_rank(sq) == RANK_7) return 1;
    return 0;
}

/*
 * Should we go on to the next level of iterative deepening in our root
 * search? This considers regular stopping conditions and also guesses whether
 * or not we can finish the next depth in the remaining time.
 */
static bool should_deepen(search_data_t* data)
{
    if (should_stop_searching(data)) return false;
    // If we're more than halfway through our time, we won't make it through
    // the next iteration anyway. TODO: this margin could be tightened up.
    if (!data->infinite && data->time_target &&
        data->time_target-elapsed_time(&data->timer) <
        data->time_target/2) return false;
    return true;
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
    int piece_value = pos->material_eval[pos->side_to_move] -
        material_value(WK) -
        material_value(WP)*pos->piece_count[pos->side_to_move][PAWN];
    return piece_value != 0;
}

/*
 * Iterative deepening search of the root position. This is the external
 * function that is called by the console interface. For each depth,
 * |root_search| performs the actual search.
 */
void deepening_search(search_data_t* search_data)
{
    search_data->engine_status = ENGINE_THINKING;
    init_timer(&search_data->timer);
    start_timer(&search_data->timer);
    // If |search_data| already has a list of root moves, we search only
    // those moves. Otherwise, search everything. This allows support for the
    // uci searchmoves command.
    if (!*search_data->root_moves) {
        generate_legal_moves(&search_data->root_pos, search_data->root_moves);
    }

    // iterative deepening loop
    root_data.best_score = -MATE_VALUE-1;
    for (search_data->current_depth=1;
            !search_data->depth_limit ||
            search_data->current_depth <= search_data->depth_limit;
            ++search_data->current_depth) {
        if (should_output(search_data)) {
            print_transposition_stats();
            printf("info depth %d\n", search_data->current_depth);
        }
        bool no_abort = root_search(search_data);
        if (!no_abort || !should_deepen(search_data)) {
            ++search_data->current_depth;
            break;
        }
    }
    stop_timer(&search_data->timer);
    
    --search_data->current_depth;
    print_pv(search_data);
    printf("info string targettime %d elapsedtime %d\n",
            search_data->time_target, elapsed_time(&search_data->timer));
    print_transposition_stats();
    char la_move[6];
    move_to_la_str(search_data->best_move, la_move);
    printf("bestmove %s\n", la_move);
    root_data.engine_status = ENGINE_IDLE;
}

/*
 * Perform search at the root position. |search_data| contains all relevant
 * search information, which is set in |deepening_search|.
 */
static bool root_search(search_data_t* search_data)
{
    int alpha = -MATE_VALUE-1, beta = MATE_VALUE+1;
    int best_depth_score = -MATE_VALUE-1;
    bool pv = true;
    int best_index = 0, move_index = 0;
    position_t* pos = &search_data->root_pos;
    // TODO: proper move scoring/ordering
    for (move_t* move=search_data->root_moves; *move;
            ++move, ++move_index) {
        if (should_output(search_data)) {
            char la_move[6];
            move_to_la_str(*move, la_move);
            printf("info currmove %s currmovenumber %d\n", la_move, move_index);
        }
        undo_info_t undo;
        do_move(pos, *move, &undo);
        int ext = extend(pos, *move);
        int score;
        if (pv) {
            score = -search(pos, search_data->search_stack,
                    1, -beta, -alpha, search_data->current_depth+ext-1);
        } else {
            score = -search(pos, search_data->search_stack,
                    1, -alpha-1, -alpha, search_data->current_depth+ext-1);
            if (score > alpha) {
                score = -search(pos, search_data->search_stack,
                    1, -beta, -alpha, search_data->current_depth+ext-1);
            }
        }
        undo_move(pos, *move, &undo);
        if (search_data->engine_status == ENGINE_ABORTED) return false;
        // update score
        if (score > alpha) {
            alpha = score;
            pv = false;
            if (score > best_depth_score) {
                best_depth_score = score;
                if (score > search_data->best_score ||
                        *move == search_data->best_move) {
                    search_data->best_score = score;
                    search_data->best_move = *move;
                    best_index = move_index;
                }
            }
            update_pv(search_data->pv, search_data->search_stack->pv, 0, *move);
            if (should_output(search_data)) {
                print_pv(search_data);
            }
        }
    }
    if (alpha != -MATE_VALUE-1) {
        // swap the pv move to the front of the list
        search_data->root_moves[best_index] = search_data->root_moves[0];
        search_data->root_moves[0] = search_data->best_move;
        search_data->best_score = best_depth_score;
        put_transposition(pos, search_data->best_move,
                search_data->current_depth,
                search_data->best_score, SCORE_EXACT);
    }
    return true;
}

static int search(position_t* pos,
        search_node_t* search_node,
        int ply,
        int alpha,
        int beta,
        int depth)
{
    if (root_data.engine_status == ENGINE_ABORTED) return 0;
    if (alpha > MATE_VALUE - ply - 1) return alpha; // can't beat this
    if (depth <= 0) {
        search_node->pv[ply] = NO_MOVE;
        return quiesce(pos, search_node, ply, alpha, beta, depth);
    }
    if (is_draw(pos)) return DRAW_VALUE;
    open_node(&root_data);
    bool full_window = (beta-alpha > 1);

    // check transposition table
    // TODO: maybe factor into its own function
    if (!full_window) { // TODO: use hash for move ordering
        move_t hash_move;
        bool hash_hit = get_transposition(
                pos, depth, &alpha, &beta, &hash_move);
        if (hash_hit && alpha >= beta) {
            search_node->pv[ply] = hash_move;
            search_node->pv[ply+1] = 0;
            return alpha;
        }
    }

    bool pv = true;
    int score = -MATE_VALUE-1;
    move_t moves[256];
    // nullmove reduction, just check for beta cutoff
    // TODO: factor into its own function
    if (is_nullmove_allowed(pos)) {
        undo_info_t undo;
        do_nullmove(pos, &undo);
        score = -search(pos, search_node+1, ply+1,
                -beta, -beta+1, depth-NULL_R);
        undo_nullmove(pos, &undo);
        if (score >= beta) {
            depth -= NULLMOVE_DEPTH_REDUCTION;
            if (depth <= 0) {
                return quiesce(pos, search_node, ply, alpha, beta, depth);
            } else {
                return beta;
            }
        }
    }
    generate_pseudo_moves(pos, moves);
    int num_legal_moves = 0;
    int orig_alpha = alpha;
    // TODO: proper move scoring/ordering
    // TODO: extensions
    // TODO: late move reductions
    for (move_t* move = moves; *move; ++move) {
        if (!is_move_legal(pos, *move)) continue;
        ++num_legal_moves;
        undo_info_t undo;
        do_move(pos, *move, &undo);
        int ext = extend(pos, *move);
        if (pv) score = -search(pos, search_node+1, ply+1,
                -beta, -alpha, depth+ext-1);
        else {
            score = -search(pos, search_node+1, ply+1,
                    -alpha-1, -alpha, depth+ext-1);
            if (score > alpha && score < beta) {
                score = -search(pos, search_node+1, ply+1,
                        -beta, -alpha, depth+ext-1);
            }
        }
        undo_move(pos, *move, &undo);
        if (score >= beta) {
            // TODO: killer move heuristic
            put_transposition(pos, *move, depth, beta, SCORE_LOWERBOUND);
            return beta;
        }
        if (score > alpha) {
            alpha = score;
            pv = false;
            update_pv(search_node->pv, (search_node+1)->pv, ply, *move);
        }
    }
    if (!num_legal_moves) {
        // No legal moves, this is either stalemate or checkmate.
        search_node->pv[ply] = NO_MOVE;
        if (is_check(pos)) {
            // note: adjust MATE_VALUE by ply so that we favor shorter mates
            return -(MATE_VALUE-ply);
        }
        return DRAW_VALUE;
    }
    score_type_t score_type = (alpha == orig_alpha) ?
        SCORE_UPPERBOUND : SCORE_EXACT;
    put_transposition(pos, search_node->pv[ply], depth, alpha, score_type);
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
    if (root_data.engine_status == ENGINE_ABORTED) return 0;
    open_node(&root_data);
    if (alpha > MATE_VALUE - ply - 1) return alpha; // can't beat this
    int eval = simple_eval(pos);
    int score = eval;
    if (score >= beta) return beta;
    if (alpha < score) alpha = score;
    
    move_t moves[256];
    generate_pseudo_captures(pos, moves);
    int num_legal_captures = 0;
    for (move_t* move = moves; *move; ++move) {
        if (!is_move_legal(pos, *move)) continue;
        ++num_legal_captures;
        if (static_exchange_eval(pos, *move) < 0) continue;
        undo_info_t undo;
        do_move(pos, *move, &undo);
        score = -quiesce(pos, search_node+1, ply+1, -beta, -alpha, depth-1);
        undo_move(pos, *move, &undo);
        if (score >= beta) return beta;
        if (score > alpha) {
            alpha = score;
            update_pv(search_node->pv, (search_node+1)->pv, ply, *move);
        }
    }
    if (!num_legal_captures) {
        search_node->pv[ply] = NO_MOVE;
        int num_legal_noncaptures = generate_legal_noncaptures(pos, moves);
        if (num_legal_noncaptures) {
            // TODO: if we're in check, we haven't quiesced yet--handle this
            // we've reached quiescence
            return eval;
        }
        // No legal moves, this is either stalemate or checkmate.
        // note: adjust MATE_VALUE by ply so that we favor shorter mates
        if (is_check(pos)) {
            return -(MATE_VALUE-ply);
        }
        return DRAW_VALUE;
    }
    return alpha;
}

/*
 * Basic minimax search, no pruning or cleverness of any kind. Used strictly
 * for debugging.
 */
static int minimax(position_t* pos,
        search_node_t* search_node,
        int ply,
        int depth)
{
    if (root_data.engine_status == ENGINE_ABORTED) return 0;
    if (!depth) {
        search_node->pv[ply] = NO_MOVE;
        return simple_eval(pos);
    }
    open_node(&root_data);
    int score, best_score = -MATE_VALUE-1;
    move_t moves[256];
    int num_moves = generate_legal_moves(pos, moves);
    for (move_t* move = moves; *move; ++move) {
        undo_info_t undo;
        do_move(pos, *move, &undo);
        score = -minimax(pos, search_node+1, ply+1, depth-1);
        undo_move(pos, *move, &undo);
        if (score > best_score) {
            best_score = score;
            update_pv(search_node->pv, (search_node+1)->pv, ply, *move);
        }
    }
    if (!num_moves) {
        // No legal moves, this is either stalemate or checkmate.
        search_node->pv[ply] = NO_MOVE;
        // note: adjust MATE_VALUE by ply so that we favor shorter mates
        if (is_check(pos)) {
            return -(MATE_VALUE-ply);
        }
        return DRAW_VALUE;
    }
    return best_score;
}

/*
 * Full minimax tree search. As slow and accurate as possible. Used to debug
 * more sophisticated search strategies.
 */
void root_search_minimax(void)
{
    position_t* pos = &root_data.root_pos;
    int depth = root_data.depth_limit;
    root_data.engine_status = ENGINE_THINKING;
    init_timer(&root_data.timer);
    start_timer(&root_data.timer);
    if (!*root_data.root_moves) generate_legal_moves(pos, root_data.root_moves);

    root_data.best_score = -MATE_VALUE-1;
    for (move_t* move=root_data.root_moves; *move; ++move) {
        undo_info_t undo;
        do_move(pos, *move, &undo);
        int score = -minimax(pos, root_data.search_stack, 1, depth-1);
        undo_move(pos, *move, &undo);
        // update score
        if (score > root_data.best_score) {
            root_data.best_score = score;
            root_data.best_move = *move;
            update_pv(root_data.pv, root_data.search_stack->pv, 0, *move);
            print_pv(&root_data);
        }
    }
        
    stop_timer(&root_data.timer);
    printf("info string targettime %d elapsedtime %d\n",
            root_data.time_target, elapsed_time(&root_data.timer));
    print_pv(&root_data);
    char la_move[6];
    move_to_la_str(root_data.best_move, la_move);
    printf("bestmove %s\n", la_move);
    root_data.engine_status = ENGINE_IDLE;
}

