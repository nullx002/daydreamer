use std::sync::{Arc, mpsc};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time;
use std::time::{Duration, Instant};

use board;
use movegen::MoveSelector;
use movement::{Move, NO_MOVE};
use options;
use position;
use position::{AttackData, Position, UndoState};
use score;
use score::{Score, score_is_valid, is_mate_score};

// Inside the search, we keep the remaining depth to search as a floating point
// value to accomodate fractional extensions and reductions better. Elsewhere
// depths are all integers to accommodate depth-indexed arrays.
pub type SearchDepth = f32;
pub const ONE_PLY_F: SearchDepth = 1.;
pub const MAX_PLY_F: SearchDepth = 127.;

pub fn is_quiescence_depth(sd: SearchDepth) -> bool {
    sd < ONE_PLY_F
}

pub type Depth = usize;
pub const ONE_PLY: Depth = 1;
pub const MAX_PLY: Depth = 127;

// EngineState is an atomic value that tracks engine state. It's atomic so that
// we can safely signal the search to stop based on external inputs without
// requiring the search thread to poll input. Logically it should be an enum,
// but I don't know of a way to get atomic operations on an enum type.
#[derive(Clone)]
pub struct EngineState {
    pub state: Arc<AtomicUsize>,
}

pub const WAITING_STATE: usize = 0;
pub const SEARCHING_STATE: usize = 1;
pub const STOPPING_STATE: usize = 2;

impl EngineState {
    pub fn new() -> EngineState {
        EngineState {
            state: Arc::new(AtomicUsize::new(WAITING_STATE)),
        }
    }

    pub fn enter(&self, state: usize) {
        self.state.store(state, Ordering::Release);
    }

    pub fn load(&self) -> usize {
        self.state.load(Ordering::Acquire)
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum SearchResult {
    Aborted,
    FailHigh,
    FailLow,
    Exact,
}

// in_millis converts a duration to integer milliseconds. It's always at least
// 1, to avoid divide-by-zero errors.
pub fn in_millis(d: &Duration) -> u64 {
   1 + d.as_secs() * 1000 + d.subsec_nanos() as u64 / 1_000_000
}

// SearchConstraints track the conditions for a search as specified via UCI.
// This is mostly about how much searching we should do before stopping, but
// also includes a list of moves to consider at the root.
pub struct SearchConstraints {
    pub infinite: bool,
    pub searchmoves: Vec<Move>, // TODO: this doesn't seem quite right here, maybe move out
    pub node_limit: u64,
    pub depth_limit: Depth,

    use_timer: bool,
    hard_limit: Duration,
    soft_limit: Duration,
    start_time: Instant,
}

impl SearchConstraints {
    pub fn new() -> SearchConstraints {
        SearchConstraints {
            infinite: false,
            searchmoves: Vec::new(),
            node_limit: u64::max_value(),
            depth_limit: MAX_PLY,
            use_timer: false,
            hard_limit: Duration::new(0, 0),
            soft_limit: Duration::new(0, 0),
            start_time: time::Instant::now(),
        }
    }

    pub fn set_timer(&mut self, us: board::Color, wtime: u32, btime: u32,
                     winc: u32, binc: u32, movetime: u32, movestogo: u32) {
        self.start_time = time::Instant::now();
        if wtime == 0 && btime == 0 && winc == 0 && binc == 0 && movetime == 0 {
            self.use_timer = false;
            return;
        }
        self.use_timer = true;
        
        if movetime != 0 {
            self.hard_limit = Duration::from_millis(max!(0, movetime - options::time_buffer()) as u64);
            self.soft_limit = self.hard_limit;
            return;
        }
        let time = if us == board::Color::White { wtime } else { btime };
        let inc = if us == board::Color::White { winc } else { binc };
        let (mut soft_limit, mut hard_limit);
        if movestogo != 0 {
            // x/y time control
            soft_limit = time / clamp!(movestogo, 2, 20);
            hard_limit = if movestogo == 1 {
                max!(time - 250, time / 2)
            } else {
                min!(time / 4, time * 4 / movestogo)
            };
        } else {
            // x+y time control
            soft_limit = time / 30 + inc;
            hard_limit = max!((time / 5) as i32, (inc as i32) - 250) as u32;
        }
        soft_limit = min!(soft_limit, time - options::time_buffer()) * 6 / 10;
        hard_limit = min!(hard_limit, time - options::time_buffer());
        self.soft_limit = Duration::from_millis(soft_limit as u64);
        self.hard_limit = Duration::from_millis(hard_limit as u64);
    }
}

pub struct SearchStats {
   nodes: u64,
   qnodes: u64,
   pvnodes: u64,
}

impl SearchStats {
    pub fn new() -> SearchStats {
        SearchStats {
            nodes: 0,
            qnodes: 0,
            pvnodes: 0,
        }
    }
}

pub struct RootMove {
   m: Move,
   score: Score,
   pv: Vec<Move>,
}

impl RootMove {
   pub fn new(m: Move) -> RootMove {
      RootMove {
         m: m,
         score: score::MIN_SCORE,
         pv: Vec::with_capacity(MAX_PLY),
      }
   }
}

pub struct SearchData {
    pub pos: Position,
    pub root_moves: Vec<RootMove>,
    pub current_depth: Depth,
    pub constraints: SearchConstraints,
    pub stats: SearchStats,
    pub state: EngineState,
    pub uci_channel: mpsc::Receiver<String>,
    pub pv_stack: [[Move; MAX_PLY + 1]; MAX_PLY + 1],
}

impl SearchData {
    pub fn new() -> SearchData {
        // Dummy receiver channel to avoid uninitialized memory.
        let (_, rx) = mpsc::channel();
        SearchData {
            pos: Position::from_fen(position::START_FEN),
            root_moves: Vec::new(),
            current_depth: 0,
            constraints: SearchConstraints::new(),
            stats: SearchStats::new(),
            state: EngineState::new(),
            uci_channel: rx,
            //search_stack: vec,
            pv_stack: [[NO_MOVE; MAX_PLY + 1]; MAX_PLY + 1],
        }
    }

    pub fn reset(&mut self) {
      self.root_moves = Vec::new();
      self.current_depth = 0;
      self.stats = SearchStats::new();
      self.pv_stack = [[NO_MOVE; MAX_PLY + 1]; MAX_PLY + 1];
    }

    pub fn should_stop(&self, depth: Depth, nodes: u64) -> bool {
        if self.constraints.infinite {
            return false;
        }
        if depth > self.constraints.depth_limit || nodes > self.constraints.node_limit {
            return true
        }
        if self.constraints.use_timer {
            return self.constraints.start_time.elapsed() > self.constraints.hard_limit
        }
        false
    }

    pub fn init_ply(&mut self, ply: usize) {
        self.pv_stack[ply][ply] = NO_MOVE;
    }

    pub fn update_pv(&mut self, ply: usize, m: Move) {
        self.pv_stack[ply][ply] = m;
        let mut i = ply + 1;
        while self.pv_stack[ply + 1][i] != NO_MOVE {
            self.pv_stack[ply][i] = self.pv_stack[ply + 1][i];
            i += 1;
        }
        self.pv_stack[ply][i] = NO_MOVE;
    }
}

pub fn go(data: &mut SearchData) {
   // We might have received a stop command already, in which case we
   // shouldn't start searching.
   if data.state.load() == STOPPING_STATE {
      return;
   }
   data.state.enter(SEARCHING_STATE);
   data.reset();

   let ad = AttackData::new(&data.pos);
   let mut moves = data.constraints.searchmoves.clone();
   if moves.len() == 0 {
      let mut ms = MoveSelector::legal();
      while let Some(m) = ms.next(&data.pos, &ad) {
         moves.push(m);
      }
   }
   if moves.len() == 0 {
      println!("info string no moves to search");
      println!("bestmove (none)");
      return
   }
   for m in moves.iter() {
      data.root_moves.push(RootMove::new(*m));
   }

   deepening_search(data);

   println!("info string time {} soft limit {} hard limit {}",
            in_millis(&data.constraints.start_time.elapsed()),
            in_millis(&data.constraints.soft_limit),
            in_millis(&data.constraints.hard_limit));
   println!("bestmove {}", data.root_moves[0].m);
   data.state.enter(WAITING_STATE);
}

fn should_deepen(data: &SearchData) -> bool {
   if data.state.load() == STOPPING_STATE { return false }
   if data.constraints.infinite { return true }
   if data.constraints.depth_limit < data.current_depth { return false }
   if !data.constraints.use_timer { return true }
   // If we're much more than halfway through our time, we won't make it
   // through the first move of the next iteration anyway.
   if data.constraints.start_time.elapsed() > data.constraints.soft_limit {
      return false
   }
   true
}

fn should_print(data: &SearchData) -> bool {
   data.constraints.start_time.elapsed().as_secs() > 1
}

// print_pv_single prints the search data for a single root move.
fn print_pv_single(data: &SearchData, rm: &RootMove, ordinal: usize, alpha: Score, beta: Score) {
    let ms = in_millis(&data.constraints.start_time.elapsed());
    let nps = if ms < 20 {
        String::new()  // don't report nps if we just started.
    } else {
        format!("nps {} ", data.stats.nodes * 1000 / ms)
    };
    let mut pv = String::new();
    pv.push_str(&format!("{} ", rm.m));
    for m in rm.pv.iter() {
        pv.push_str(&format!("{} ", *m));
    }
    debug_assert!(score_is_valid(rm.score));
    let bound = if rm.score <= alpha {
        String::from("upperbound ")
    } else if rm.score >= beta {
        String::from("lowerbound ")
    } else {
        String::new()
    };
    let score = if is_mate_score(rm.score) {
       let mut mate_in = (score::MATE_SCORE - rm.score.abs() + 1) / 2;
       if rm.score < 0 { mate_in *= -1; }
       format!("mate {}", mate_in)
    } else {
       format!("cp {}", rm.score)
    };
    println!("info multipv {} depth {} score {} {}time {} nodes {} {}pv {}",
             ordinal, data.current_depth, score, bound, ms, data.stats.nodes, nps, pv);
}

// print_pv prints out the most up-to-date information about the current
// principal variations in the format expected by UCI.
fn print_pv(data: &SearchData, alpha: Score, beta: Score) {
    // We need to print the n highest-scoring moves. They may not be in order
    // so we extract them in order with a heap.
    use std::collections::BinaryHeap;

    let mut heap = BinaryHeap::new();
    for entry in data.root_moves.iter().enumerate() {
        let (i, rm) = entry;
        heap.push((rm.score, i));
    }

    for i in 0..options::multi_pv() {
        if let Some((_, idx)) = heap.pop() {
            print_pv_single(data, &data.root_moves[idx], i + 1, alpha, beta);
        } else {
            panic!("failed to extract root move for printing");
        }
    }
}

fn deepening_search(data: &mut SearchData) {
   data.current_depth = 2 * ONE_PLY;
   while should_deepen(data) {
      if should_print(data) {
         println!("info depth {}", data.current_depth);
      }
      // TODO: aspiration windows
      let (alpha, beta) = (score::MIN_SCORE, score::MAX_SCORE);
      let result = root_search(data, alpha, beta);
      if result == SearchResult::Aborted { break }
      data.root_moves.sort_by(|a, b| b.score.cmp(&a.score));
      print_pv(data, alpha, beta);
      data.current_depth += ONE_PLY;
   }
}

// Note: the non-exact result stuff isn't currently used because I
// haven't implemented aspiration or reductions yet.
fn root_search(data: &mut SearchData, mut alpha: Score, beta: Score) -> SearchResult {
    let (orig_alpha, mut best_alpha) = (alpha, alpha);
    data.init_ply(0);

    let ad = AttackData::new(&data.pos);
    let undo = UndoState::undo_state(&data.pos);
    let mut move_number = 0;
    for i in 0..data.root_moves.len() {
        let m = data.root_moves[i].m;
        if should_print(data) {
            println!("info currmove {} currmovenumber {}", m, move_number + 1);
        }
        data.stats.nodes += 1;
        data.pos.do_move(m, &ad);
        let mut full_search = move_number < options::multi_pv();
        let mut score = score::MIN_SCORE;
        let depth = data.current_depth as SearchDepth;
        alpha = if full_search { orig_alpha } else { best_alpha };
        if !full_search {
            // TODO: test depth reductions here
            score = -search(data, 1, -alpha - 1, -alpha, depth - ONE_PLY_F);
            if score > alpha { full_search = true };
        }
        if full_search {
            score = -search(data, 1, -beta, -alpha, depth - ONE_PLY_F);
        }
        data.pos.undo_move(m, &undo);
        debug_assert!(score_is_valid(score));
        if data.state.load() == STOPPING_STATE { return SearchResult::Aborted; }
        data.root_moves[i].score = score::MIN_SCORE;
        if full_search || score > alpha {
            // We have updated move info for the root.
            data.root_moves[i].score = score;
            data.root_moves[i].pv.clear();
            for ply in 1..MAX_PLY {
                let mv = data.pv_stack[1][ply];
                if mv == NO_MOVE { break }
                data.root_moves[i].pv.push(mv);
            }
        }
        if score > alpha {
            if move_number > options::multi_pv() { print_pv(data, alpha, beta) }
            alpha = score;
            if score > best_alpha { best_alpha = score }
        }
        if score >= beta { break }
        move_number += 1;
    }
    if alpha == orig_alpha {
        return SearchResult::FailLow;
    } else if alpha >= beta {
        return SearchResult::FailHigh;
    }
    SearchResult::Exact
}

fn search(data: &mut SearchData, ply: usize,
          mut alpha: Score, mut beta: Score, depth: SearchDepth) -> Score {
    data.init_ply(ply);
    if data.state.load() == STOPPING_STATE { return score::DRAW_SCORE; }
    if is_quiescence_depth(depth) {
        return quiesce(data, ply, alpha, beta, depth);
    }

    debug_assert!(score_is_valid(alpha) && score_is_valid(beta));
    alpha = max!(alpha, score::mated_in(ply));
    beta = min!(beta, score::mate_in(ply));
    if alpha >= beta { return alpha }
    // TODO: trivial draw detection

    let open_window = beta - alpha > 1;
    let mut best_score = score::MIN_SCORE;
    let ad = AttackData::new(&data.pos);
    let undo = UndoState::undo_state(&data.pos);
    // TODO: nullmove, razoring
    let mut num_moves = 0;

    // TODO: proper move ordering interface, plus generate pseudo-legal moves
    // and check legality afterwards.
    let mut selector = MoveSelector::legal();
    while let Some(m) = selector.next(&data.pos, &ad) {
        // TODO: pruning, futility, depth extension
        data.pos.do_move(m, &ad);
        data.stats.nodes += 1;
        num_moves += 1;
        let mut score = score::MIN_SCORE;
        let mut full_search = open_window && num_moves == 1;
        if !full_search {
            // TODO: depth reductions
            score = -search(data, ply + 1, -alpha - 1, -alpha, depth - ONE_PLY_F);
            if open_window && score > alpha { full_search = true; }
        }
        if full_search {
            score = -search(data, ply + 1, -beta, -alpha, depth - ONE_PLY_F);
        }
        debug_assert!(score_is_valid(score));
        data.pos.undo_move(m, &undo);

        // If we're aborting, the score from the last move shouldn't be trusted,
        // since we didn't finish searching it, so bail out without updating
        // pv, bounds, etc.
        if data.state.load() == STOPPING_STATE { return score::DRAW_SCORE; }
        if score > best_score {
            best_score = score;
            if score > alpha {
                alpha = score;
                data.update_pv(ply, m);
            }
            if score >= beta { break }
        }
    }
    if num_moves == 0 {
        // Stalemate or checkmate. We have to be careful not to prune away any
        // moves without checking their legality until we know that there's at
        // least one legal move so that this check is valid.
        if data.pos.checkers() != 0 { return score::mated_in(ply) }
        return score::DRAW_SCORE;
    }
    debug_assert!(score_is_valid(best_score));
    best_score
}

fn quiesce(data: &mut SearchData, ply: usize,
           mut alpha: Score, mut beta: Score, depth: SearchDepth) -> Score {
    if data.state.load() == STOPPING_STATE { return score::DRAW_SCORE; }
    debug_assert!(score_is_valid(alpha) && score_is_valid(beta));
    alpha = max!(alpha, score::mated_in(ply));
    beta = min!(beta, score::mate_in(ply));
    if alpha >= beta { return alpha }
    // TODO: trivial draw detection
    if ply >= MAX_PLY { return score::DRAW_SCORE }
    data.init_ply(ply);

    let mut best_score = score::MIN_SCORE;
    if data.pos.checkers() == 0 {
        best_score = data.pos.material_score();
        if best_score >= alpha {
            alpha = best_score;
            debug_assert!(score_is_valid(best_score));
            if best_score >= beta { return best_score }
        }
    }

    let ad = AttackData::new(&data.pos);
    let undo = UndoState::undo_state(&data.pos);
    let mut num_moves = 0;

    let mut selector = MoveSelector::new(&data.pos, depth);
    while let Some(m) = selector.next(&data.pos, &ad) {
        if !data.pos.pseudo_move_is_legal(m, &ad) { continue }
        data.pos.do_move(m, &ad);
        data.stats.nodes += 1;
        num_moves += 1;

        // TODO: pruning
        let score = -quiesce(data, ply + 1, -beta, -alpha, depth - ONE_PLY_F);
        debug_assert!(score_is_valid(score));
        data.pos.undo_move(m, &undo);

        // If we're aborting, the score from the last move shouldn't be trusted,
        // since we didn't finish searching it, so bail out without updating
        // pv, bounds, etc.
        if data.state.load() == STOPPING_STATE { return score::DRAW_SCORE; }
        if score > best_score {
            best_score = score;
            if score > alpha {
                alpha = score;
                data.update_pv(ply, m);
            }
            if score >= beta { break }
        }
    }
    // Detect checkmate. We can't find stalemate because we don't reliably generate
    // quiet moves, but evasion movegen is always exhaustive.
    if num_moves == 0 && data.pos.checkers() != 0 { return score::mated_in(ply) }
    debug_assert!(score_is_valid(best_score));
    best_score
}
