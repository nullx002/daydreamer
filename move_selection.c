
#include "daydreamer.h"
#include <string.h>

extern search_data_t root_data;
static const bool defer_enabled = false;
static bool pv_cache_enabled = true;

selection_phase_t phase_table[6][8] = {
    { PHASE_BEGIN, PHASE_ROOT, PHASE_END },
    { PHASE_BEGIN, PHASE_TRANS, PHASE_PV, PHASE_DEFERRED, PHASE_END },
    { PHASE_BEGIN, PHASE_TRANS, PHASE_NON_PV, PHASE_DEFERRED, PHASE_END },
    { PHASE_BEGIN, PHASE_EVASIONS, PHASE_DEFERRED, PHASE_END },
    { PHASE_BEGIN, PHASE_TRANS, PHASE_QSEARCH, PHASE_DEFERRED, PHASE_END },
    { PHASE_BEGIN, PHASE_TRANS, PHASE_QSEARCH_CH, PHASE_DEFERRED, PHASE_END },
};

typedef struct {
    hashkey_t key;
    move_t moves[256];
    int64_t nodes[256];
} move_cache_t;

// How many moves should be selected by scanning through the score list and
// picking the highest available, as opposed to picking them in order? Note
// that root selection is 0 because the moves are already sorted into the
// correct order.
static const int ordered_move_count[6] = { 0, 256, 16, 16, 4, 4 };

static void generate_moves(move_selector_t* sel);
static void score_moves(move_selector_t* sel);
static void sort_root_moves(move_selector_t* sel);
static int64_t score_tactical_move(position_t* pos, move_t move);
static move_t get_best_move(move_selector_t* sel, int64_t* score);
static move_cache_t* get_pv_move_list(const position_t* pos);

/*
 * Initialize the move selector data structure with the information needed to
 * determine what kind of moves to generate and how to order them.
 */
void init_move_selector(move_selector_t* sel,
        position_t* pos,
        generation_t gen_type,
        search_node_t* search_node,
        move_t hash_move,
        int depth,
        int ply)
{
    sel->pos = pos;
    if (is_check(pos) && gen_type != ROOT_GEN) {
        sel->generator = ESCAPE_GEN;
    } else {
        sel->generator = gen_type;
    }
    sel->phase = phase_table[sel->generator];
    sel->hash_move[0] = hash_move;
    sel->hash_move[1] = NO_MOVE;
    sel->depth = depth;
    sel->moves_so_far = 0;
    sel->quiet_moves_so_far = 0;
    sel->pv_index = 0;
    sel->ordered_moves = ordered_move_count[gen_type];
    if (search_node) {
        sel->mate_killer = search_node->mate_killer;
        sel->killers[0] = search_node->killers[0];
        sel->killers[1] = search_node->killers[1];
        if (ply >= 2) {
            sel->killers[2] = (search_node-2)->killers[0];
            sel->killers[3] = (search_node-2)->killers[1];
        } else {
            sel->killers[2] = sel->killers[3] = NO_MOVE;
        }
        sel->killers[4] = NO_MOVE;
    } else {
        sel->killers[0] = sel->killers[1] = NO_MOVE;
    }
    sel->deferred_moves[0] = NO_MOVE;
    sel->num_deferred_moves = 0;
    generate_moves(sel);
}

/*
 * Is there only one possible move in the current position? This may need to
 * be changed for phased move generation later.
 */
bool has_single_reply(move_selector_t* sel)
{
    return *sel->phase == PHASE_EVASIONS && sel->moves_end == 1;
}

bool should_try_prune(move_selector_t* sel, move_t move)
{
    // TODO: try excluding killers, underpromotes
    return !get_move_capture(move) &&
        !get_move_promote(move) &&
        !is_move_castle(move);
}

float lmr_reduction(move_selector_t* sel, move_t move)
{
    assert(sel->moves[sel->current_move_index-1] == move);
    bool do_lmr = sel->quiet_moves_so_far > 2 &&
        !get_move_capture(move) &&
        get_move_promote(move) != QUEEN &&
        !is_move_castle(move) &&
        move != sel->killers[0] &&
        move != sel->killers[1];
    return do_lmr ?
        (sel->scores[sel->current_move_index-1]<0 ? 2*PLY : PLY) : 0;
}

/*
 * Fill the list of candidate moves and score each move for later selection.
 */
static void generate_moves(move_selector_t* sel)
{
    sel->phase++;
    sel->moves_end = 0;
    sel->current_move_index = 0;
    sel->moves = sel->base_moves;
    sel->scores = sel->base_scores;
    move_cache_t* pv_cache;
    switch (*sel->phase) {
        case PHASE_BEGIN:
            assert(false);
        case PHASE_END:
            return;
        case PHASE_TRANS:
            sel->moves = sel->hash_move;
            sel->moves_end = 1;
            break;
        case PHASE_EVASIONS:
            sel->moves_end = generate_evasions(sel->pos, sel->moves);
            score_moves(sel);
            break;
        case PHASE_ROOT:
            sort_root_moves(sel);
            break;
        case PHASE_PV:
            pv_cache = get_pv_move_list(sel->pos);
            if (pv_cache_enabled && pv_cache->key == sel->pos->hash) {
                int i;
                for (i=0; pv_cache->moves[i]; ++i) {
                    sel->moves[i] = pv_cache->moves[i];
                    sel->scores[i] = pv_cache->nodes[i];
                    assert2(is_move_legal(sel->pos, sel->moves[i]));
                }
                sel->moves[i] = NO_MOVE;
                sel->moves_end = i;
                break;
            }
        case PHASE_NON_PV:
            sel->moves_end = generate_pseudo_moves(sel->pos, sel->moves);
            score_moves(sel);
            break;
        case PHASE_QSEARCH_CH:
            sel->moves_end = generate_quiescence_moves(
                    sel->pos, sel->moves, true);
            score_moves(sel);
            break;
        case PHASE_QSEARCH:
            sel->moves_end = generate_quiescence_moves(
                    sel->pos, sel->moves, false);
            score_moves(sel);
            break;
        case PHASE_DEFERRED:
            sel->moves = sel->deferred_moves;
            sel->moves_end = sel->num_deferred_moves;
            break;
        default: assert(false);
    }
    sel->single_reply = sel->generator == ESCAPE_GEN && sel->moves_end == 1;
    assert(sel->moves[sel->moves_end] == NO_MOVE);
    assert(sel->current_move_index == 0);
}

/*
 * Return the next move to be searched. The first n moves are returned in order
 * of their score, and the rest in the order they were generated. n depends on
 * the type of node we're at.
 */
move_t select_move(move_selector_t* sel)
{
    if (*sel->phase == PHASE_END) return NO_MOVE;

    move_t move;
    switch (*sel->phase) {
        case PHASE_TRANS:
            move = sel->hash_move[sel->current_move_index++];
            if (!move || !is_plausible_move_legal(sel->pos, move)) break;
            sel->moves_so_far++;
            return move;
        case PHASE_ROOT:
            move = sel->moves[sel->current_move_index++];
            if (!move) break;
            sel->moves_so_far++;
            if (!get_move_capture(move) && get_move_promote(move) != QUEEN) {
                sel->quiet_moves_so_far++;
            }
            return move;
        case PHASE_EVASIONS:
            if (sel->current_move_index >= sel->ordered_moves) {
                move = sel->moves[sel->current_move_index++];
                if (!move) break;
                sel->moves_so_far++;
                if (!get_move_capture(move) && get_move_promote(move)!=QUEEN) {
                    sel->quiet_moves_so_far++;
                }
                return move;
            } else {
                assert(sel->current_move_index <= sel->moves_end);
                move = get_best_move(sel, NULL);
                if (!move) break;
                check_pseudo_move_legality(sel->pos, move);
                sel->moves_so_far++;
                if (!get_move_capture(move) && get_move_promote(move)!=QUEEN) {
                    sel->quiet_moves_so_far++;
                }
                return move;
            }
            
        case PHASE_PV:
        case PHASE_NON_PV:
        case PHASE_QSEARCH:
        case PHASE_QSEARCH_CH:
            while (sel->current_move_index >= sel->ordered_moves) {
                move = sel->moves[sel->current_move_index++];
                if (move == NO_MOVE) break;
                if (move == sel->hash_move[0] ||
                        !is_pseudo_move_legal(sel->pos, move)) {
                    continue;
                }
                sel->moves_so_far++;
                if (!get_move_capture(move) && get_move_promote(move)!=QUEEN) {
                    sel->quiet_moves_so_far++;
                }
                return move;
            }
            if (sel->current_move_index >= sel->ordered_moves) break;

            while (true) {
                assert(sel->current_move_index <= sel->moves_end);
                int64_t best_score;
                move = get_best_move(sel, &best_score);
                if (!move) break;
                if ((sel->generator == Q_CHECK_GEN ||
                     sel->generator == Q_GEN) &&
                    (!get_move_promote(move) ||
                     get_move_promote(move) != QUEEN) &&
                    best_score < MAX_HISTORY) continue;
                if (move == sel->hash_move[0] ||
                        !is_pseudo_move_legal(sel->pos, move)) continue;
                check_pseudo_move_legality(sel->pos, move);
                sel->moves_so_far++;
                if (!get_move_capture(move) && get_move_promote(move)!=QUEEN) {
                    sel->quiet_moves_so_far++;
                }
                return move;
            }
            break;
        case PHASE_DEFERRED:
            assert(sel->current_move_index <= sel->moves_end);
            move = sel->moves[sel->current_move_index++];
            if (!move) break;
            sel->moves_so_far++;
            if (!get_move_capture(move) && get_move_promote(move)!=QUEEN) {
                sel->quiet_moves_so_far++;
            }
            return move;

        default: assert(false);
    }

    assert(*sel->phase != PHASE_END);
    generate_moves(sel);
    return select_move(sel);
}

bool defer_move(move_selector_t* sel, move_t move)
{
    if (!defer_enabled) return false;
    assert(move == sel->moves[sel->current_move_index]);
    if (*sel->phase == PHASE_DEFERRED ||
            *sel->phase == PHASE_TRANS ||
            sel->scores[sel->current_move_index] > MAX_HISTORY) return false;
    sel->deferred_moves[sel->num_deferred_moves++] = move;
    sel->deferred_moves[sel->num_deferred_moves] = NO_MOVE;
    sel->moves_so_far--;
    return true;
}

static move_t get_best_move(move_selector_t* sel, int64_t* score)
{
    int offset = sel->current_move_index;
    move_t move = NO_MOVE;
    int64_t best_score = INT64_MIN;
    int index = -1;
    for (int i=offset; sel->moves[i] != NO_MOVE; ++i) {
        if (sel->scores[i] > best_score) {
            best_score = sel->scores[i];
            index = i;
        }
    }
    if (index != -1) {
        assert(best_score == sel->scores[index]);
        move = sel->moves[index];
        sel->moves[index] = sel->moves[offset];
        sel->scores[index] = sel->scores[offset];
        sel->moves[offset] = move;
        sel->scores[offset] = best_score;
        sel->current_move_index++;
    }
    if (score) *score = best_score;
    return move;
}

/*
 * Take an unordered list of pseudo-legal moves and score them according
 * to how good we think they'll be. This just identifies a few key classes
 * of moves and applies scores appropriately. Moves are then selected
 * by |select_move|.
 * TODO: separate scoring functions for different phases.
 */
static void score_moves(move_selector_t* sel)
{
    move_t* moves = sel->moves;
    int64_t* scores = sel->scores;

    const int64_t grain = MAX_HISTORY;
    const int64_t hash_score = 1000 * grain;
    const int64_t killer_score = 700 * grain;
    for (int i=0; moves[i] != NO_MOVE; ++i) {
        const move_t move = moves[i];
        int64_t score = 0ull;
        if (move == sel->hash_move[0]) {
            score = hash_score;
        } else if (move == sel->mate_killer) {
            score = hash_score-1;
        } else if (get_move_capture(move) || get_move_promote(move)) {
            score = score_tactical_move(sel->pos, move);
        } else if (move == sel->killers[0]) {
            score = killer_score;
        } else if (move == sel->killers[1]) {
            score = killer_score-1;
        } else if (move == sel->killers[2]) {
            score = killer_score-2;
        } else if (move == sel->killers[3]) {
            score = killer_score-3;
        } else {
            score = (int64_t)root_data.history.history[history_index(move)];
        }
        scores[i] = score;
    }
}

/*
 * Determine a score for a capturing or promoting move.
 */
static int64_t score_tactical_move(position_t* pos, move_t move)
{
    const int64_t grain = MAX_HISTORY;
    const int64_t good_tactic_score = 800 * grain;
    const int64_t bad_tactic_score = -800 * grain;
    bool good_tactic;
    piece_type_t piece = get_move_piece_type(move);
    piece_type_t promote = get_move_promote(move);
    piece_type_t capture = piece_type(get_move_capture(move));
    if (promote != NONE && promote != QUEEN) good_tactic = false;
    else if (capture != NONE && piece <= capture) good_tactic = true;
    else good_tactic = (static_exchange_sign(pos, move) >= 0);
    return 6*capture - piece + 5 +
        (good_tactic ? good_tactic_score : bad_tactic_score);
}

/*
 * Sort moves at the root based on total nodes searched under that move.
 * Since the moves are sorted into position, |sel->scores| is not used to
 * select moves during root move selection.
 */
static void sort_root_moves(move_selector_t* sel)
{
    int i;
    for (i=0; root_data.root_moves[i].move != NO_MOVE; ++i) {
        sel->moves[i] = root_data.root_moves[i].move;
        if (sel->moves[i] == sel->hash_move[0]) {
            sel->scores[i] = INT64_MAX;
        } else if (sel->depth <= 2*PLY) {
            sel->scores[i] = root_data.root_moves[i].qsearch_score;
        } else if (options.multi_pv > 1) {
            sel->scores[i] = root_data.root_moves[i].score;
        } else {
            sel->scores[i] = (int64_t)root_data.root_moves[i].nodes;
        }
    }
    sel->moves_end = i;
    sel->moves[i] = NO_MOVE;

    for (i=0; sel->moves[i] != NO_MOVE; ++i) {
        move_t move = sel->moves[i];
        int64_t score = sel->scores[i];
        int j = i-1;
        while (j >= 0 && sel->scores[j] < score) {
            sel->scores[j+1] = sel->scores[j];
            sel->moves[j+1] = sel->moves[j];
            --j;
        }
        sel->scores[j+1] = score;
        sel->moves[j+1] = move;
    }
}

struct {
    int hits;
    int misses;
    int occupied;
    int evictions;
} pv_cache_stats;

static move_cache_t* pv_cache = NULL;
static int num_buckets;

/*
 * The pv cache stores counts of nodes searched under each move for a given
 * position encountered during the pv. When the cache hits during move
 * selection, moves are ordered by nodes searched rather than other heuristics.
 * This function allocates memory and initializes the pv cache.
 */
void init_pv_cache(const int max_bytes)
{
    assert(max_bytes >= 1024);
    int size = sizeof(move_cache_t);
    num_buckets = 1;
    while (size <= max_bytes >> 1) {
        size <<= 1;
        num_buckets <<= 1;
    }
    if (pv_cache != NULL) free(pv_cache);
    pv_cache = malloc(size);
    assert(pv_cache);
    clear_pv_cache();
}

/*
 * Clear all entries in the pv cache.
 */
void clear_pv_cache(void)
{
    memset(pv_cache, 0, num_buckets*sizeof(move_cache_t));
}

/*
 * Retrieve the pv cache entry associated with |pos|.
 */
static move_cache_t* get_pv_move_list(const position_t* pos)
{
    move_cache_t* m = &pv_cache[pos->hash % num_buckets];
    if (m->key == pos->hash) pv_cache_stats.hits++;
    else if (m->key != 0) pv_cache_stats.evictions++;
    else {
        pv_cache_stats.misses++;
        pv_cache_stats.occupied++;
    }
    return m;
}

/*
 * Add a count of nodes searched under a pv node to |sel|.
 */
void add_pv_move(move_selector_t* sel, move_t move, int64_t nodes)
{
    if (sel->generator == ESCAPE_GEN) return;
    assert2(is_pseudo_move_legal(sel->pos, move));
    assert2(is_move_legal(sel->pos, move));
    sel->pv_moves[sel->pv_index] = move;
    sel->pv_nodes[sel->pv_index++] = nodes;
    assert(sel->pv_index == sel->moves_so_far);
}

/*
 * Write all information stored about pv node counts in |sel| to the cache.
 */
void commit_pv_moves(move_selector_t* sel)
{
    if (sel->generator == ESCAPE_GEN) return;
    assert(sel->pv_index == sel->moves_so_far);
    move_cache_t* pv_cache = get_pv_move_list(sel->pos);
    pv_cache->key = sel->pos->hash;
    int i;
    for (i=0; i < sel->pv_index; ++i) {
        assert(sel->pv_moves[i]);
        assert2(is_move_legal(sel->pos, sel->pv_moves[i]));
        pv_cache->moves[i] = sel->pv_moves[i];
        pv_cache->nodes[i] = sel->pv_nodes[i];
    }
    pv_cache->moves[i] = NO_MOVE;
}

/*
 * Dump some information about pv cache activity to stdout.
 */
void print_pv_cache_stats(void)
{
    printf("info string pv cache entries %d", num_buckets);
    printf(" filled %d (%.2f%%)", pv_cache_stats.occupied,
            (float)pv_cache_stats.occupied / (float)num_buckets*100.);
    printf(" evictions %d", pv_cache_stats.evictions);
    printf(" hits %d (%.2f%%)", pv_cache_stats.hits,
            (float)pv_cache_stats.hits /
            (pv_cache_stats.hits + pv_cache_stats.misses)*100.);
    printf(" misses %d (%.2f%%)\n", pv_cache_stats.misses,
            (float)pv_cache_stats.misses /
            (pv_cache_stats.hits + pv_cache_stats.misses)*100.);
}

