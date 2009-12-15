
#ifndef EVAL_H
#define EVAL_H
#ifdef __cplusplus
extern "C" {
#endif

#ifdef UFO_EVAL
    #define PAWN_VAL            100
    #define PAWN_VAL_ENDGAME    100
    #define KNIGHT_VAL          320
    #define KNIGHT_VAL_ENDGAME  320
    #define BISHOP_VAL          330
    #define BISHOP_VAL_ENDGAME  330
    #define ROOK_VAL            500
    #define ROOK_VAL_ENDGAME    500
    #define QUEEN_VAL           900
    #define QUEEN_VAL_ENDGAME   900
    #define KING_VAL            20000
    #define KING_VAL_ENDGAME    20000
#else
    #define PAWN_VAL            100
    #define PAWN_VAL_ENDGAME    130
    #define KNIGHT_VAL          325
    #define KNIGHT_VAL_ENDGAME  335
    #define BISHOP_VAL          325
    #define BISHOP_VAL_ENDGAME  335
    #define ROOK_VAL            500
    #define ROOK_VAL_ENDGAME    505
    #define QUEEN_VAL           975
    #define QUEEN_VAL_ENDGAME   980
    #define KING_VAL            20000
    #define KING_VAL_ENDGAME    20000
#endif

extern int piece_square_values[BK+1][0x80];
extern int endgame_piece_square_values[BK+1][0x80];
extern const int material_values[];
extern const int endgame_material_values[];
#define material_value(piece)               material_values[piece]
#define endgame_material_value(piece)       endgame_material_values[piece]
#define piece_square_value(piece, square)   piece_square_values[piece][square]
#define endgame_piece_square_value(piece, square) \
    endgame_piece_square_values[piece][square]

typedef struct {
    int midgame;
    int endgame;
} score_t;

#ifdef __cplusplus
} // extern "C"
#endif
#endif // EVAL_H
