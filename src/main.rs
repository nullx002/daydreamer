#![allow(dead_code)]

extern crate rand;
extern crate time;

#[macro_use] pub mod macros;
pub mod board;
pub mod bitboard;
pub mod movement;
pub mod movegen;
pub mod options;
pub mod perft;
pub mod position;
pub mod search;
pub mod uci;

fn main() {
    bitboard::initialize();
    let mut search_data = search::SearchData::new();

    // Treat each argument as a file containing uci commands.
    for arg in ::std::env::args().skip(1) {
        uci::read_stream(&mut search_data, Some(arg.to_string()));
    }
    // Read from stdin.
    uci::read_stream(&mut search_data, None);
}
