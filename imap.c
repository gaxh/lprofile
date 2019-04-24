#include "imap.h"
#include <lauxlib.h>
#include <stdlib.h>

#define IMAP_LUA_KEYNAME "_IK_"

static size_t hashcb(const void *val){
    return (size_t)val;
}

static int cmpcb(const void *x, const void *y){
    uint64_t x_ = (uint64_t)x;
    uint64_t y_ = (uint64_t)y;

    return x_ == y_ ? 0 : ( x_ < y_ ? -1 : 1 );
}

void imap_init(ImapContext *mc){
    avl_map_init(&mc->table, hashcb, cmpcb);
}

void imap_destroy(ImapContext *mc){
    avl_map_destroy(&mc->table);
}

void imap_set(ImapContext *mc, uint64_t key, void *val){
    avl_map_set(&mc->table, (void *)key, (void *)val);
}

bool imap_get(ImapContext *mc, uint64_t key, void **out){
    struct avl_hash_entry *e = avl_map_find(&mc->table, (void *)key);

    return e ? (*out = e->value, true) : false;
}

void imap_remove(ImapContext *mc, uint64_t key){
    avl_map_remove(&mc->table, (void *)key);
}

void imap_clear(ImapContext *mc){
    avl_map_clear(&mc->table);
}

void imap_foreach(ImapContext *mc, ImapForeachCb cb, void *ud){
    struct avl_hash_entry *e;

    for(e = avl_map_first(&mc->table); e != NULL; e = avl_map_next(&mc->table, e)){
        cb(ud, (uint64_t)e->node.key, e->value);
    }
}


