#ifndef __IMAP_H__
#define __IMAP_H__

#include <stdint.h>
#include <stdbool.h>
#include "avlhash.h"

typedef struct ImapContext {
    struct avl_hash_map table;
} ImapContext;

void imap_init(ImapContext *);
void imap_destroy(ImapContext *);

void imap_set(ImapContext *, uint64_t key, void *val);
bool imap_get(ImapContext *, uint64_t key, void **out);
void imap_remove(ImapContext *, uint64_t key);
void imap_clear(ImapContext *);

typedef void (*ImapForeachCb)(void *ud, uint64_t key, void *val);
void imap_foreach(ImapContext *, ImapForeachCb cb, void *ud);

#endif

