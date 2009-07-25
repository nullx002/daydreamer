
#ifndef TRANS_TABLE_H
#define TRANS_TABLE_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    hashkey_t key;
    move_t move;
    uint16_t depth;
    int32_t score;
    score_type_t score_type;
} transposition_entry_t;
extern transposition_entry_t* transposition_table;

#ifdef __cplusplus
} // extern "C"
#endif
#endif // TRANS_TABLE_H
