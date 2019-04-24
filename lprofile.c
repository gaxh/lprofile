#include "lprofile.h"
#include "imap.h"
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#define LPROFILE_METATBL_NAME "_LPMETA_"
#define LPROFILE_REGKEY_NAME "_LPREG_"

#define LP_ENABLE_LOG 0

#if LP_ENABLE_LOG
#define lplog(fmt, args...) printf(fmt, ## args)
#else
#define lplog(fmt, args...)
#endif

typedef struct CallFrame {
    void *proto;
    const char *source;
    const char *name;
    const char *namewhat;
    const char *what;
    int line;
    int istailcall;
    uint64_t call_evt_hpc;
    uint64_t call_real_hpc;
    uint64_t ret_hpc;
    uint64_t total_nspan;
    uint64_t real_nspan;
    uint64_t sub_nspan;
    uint64_t yield_nspan;
} CallFrame;

typedef struct ProtoRecord {
    void *proto;
    char source[64];
    char name[32];
    char namewhat[8];
    char what[8];
    int line;
    int istailcall;
    int callnb;
    uint64_t total_nspan;
    uint64_t real_nspan;
    uint64_t coroutine_nspan;
} ProtoRecord;

typedef struct RecordPool {
    ImapContext usedmap;
    int cap;
    int nb;
    ProtoRecord *pool;
} RecordPool;

typedef struct CallStack {
    int cap;
    int nb;
    int ref;
    CallFrame *stk;
    struct CallStack *nextnode;
} CallStack;

typedef struct CallStackPool {
    ImapContext usedmap;
    CallStack freelist;
    int usednb;
    int freenb;
    int stat_usednb;
} CallStackPool;

typedef struct ProfileContext {
    ImapContext runnings;
    CallStackPool stacks;
    RecordPool records;
    uint64_t stat_lossnspan;
    uint64_t stat_realnspan;
    uint64_t stat_yieldnspan;
    bool enabled;
    void *proto_yield;
    bool trace_tailcall;
} ProfileContext;

typedef struct DumpArg {
    lua_State *L;
    ProfileContext *pc;
} DumpArg;

static inline void __recordpool_init(lua_State *L, RecordPool *rp){
    void *ud;
    lua_Alloc af = lua_getallocf(L, &ud);

    imap_init(&rp->usedmap);
    rp->nb = 0;
    rp->cap = 100;
    rp->pool = af(ud, NULL, 0, rp->cap * sizeof(rp->pool[0]));

    lplog("__recordpool_init rp=%p\n", rp);
}

static inline void __recordpool_destroy(lua_State *L, RecordPool *rp){
    void *ud;
    lua_Alloc af = lua_getallocf(L, &ud);

    af(ud, rp->pool, rp->cap * sizeof(rp->pool[0]), 0);
    imap_destroy(&rp->usedmap);

    lplog("__recordpool_destroy rp=%p\n", rp);
}

static inline void __recordpool_clear(lua_State *L, RecordPool *rp){
    imap_clear(&rp->usedmap);
    rp->nb = 0;
}

static void __recordpool_record(lua_State *L, RecordPool *rp, CallFrame *cf){
    void *val;
    ProtoRecord *pr;

    if(imap_get(&rp->usedmap, (uint64_t)cf->proto, &val)){
        pr = &rp->pool[(uint64_t)val];
    }else{
        uint64_t id;

        if(rp->nb >= rp->cap){
            void *ud;
            lua_Alloc af = lua_getallocf(L, &ud);
            int newcap = rp->cap * 2;

            rp->pool = af(ud, rp->pool, rp->cap * sizeof(rp->pool[0]), newcap * sizeof(rp->pool[0]));
            rp->cap = newcap;
        }

        pr = &rp->pool[rp->nb];
        id = rp->nb;
        ++rp->nb;

        pr->proto = cf->proto;

#define SAFE_COPY_STRING(dst, src)\
        do{\
            (src) = (src) ? (src) : "";\
            strncpy((dst), (src), sizeof(dst));\
            (dst)[sizeof(dst) - 1] = 0;\
        }while(0)

        SAFE_COPY_STRING(pr->source, cf->source);
        SAFE_COPY_STRING(pr->name, cf->name);
        SAFE_COPY_STRING(pr->namewhat, cf->namewhat);
        SAFE_COPY_STRING(pr->what, cf->what);

#undef SAFE_COPY_STRING

        pr->line = cf->line;
        pr->istailcall = 0;
        pr->callnb = 0;
        pr->total_nspan = 0;
        pr->real_nspan = 0;
        pr->coroutine_nspan = 0;

        imap_set(&rp->usedmap, (uint64_t)cf->proto, (void *)id);
    }

    ++pr->callnb;
    pr->total_nspan += cf->total_nspan;
    pr->real_nspan += cf->real_nspan;
    pr->istailcall |= cf->istailcall;
    pr->coroutine_nspan += cf->total_nspan - cf->yield_nspan;
}

static inline void __callstack_init(lua_State *L, CallStack *cs){
    void *ud;
    lua_Alloc af = lua_getallocf(L, &ud);

    cs->nb = 0;
    cs->cap = 100;
    cs->ref = 0;
    cs->nextnode = NULL;
    cs->stk = af(ud, NULL, 0, cs->cap * sizeof(cs->stk[0]));

    lplog("__callstack_init cs=%p\n", cs);
}

static inline void __callstack_destroy(lua_State *L, CallStack *cs){
    void *ud;
    lua_Alloc af = lua_getallocf(L, &ud);

    af(ud, cs->stk, cs->cap * sizeof(cs->stk[0]), 0);
    lplog("__callstack_destroy cs=%p\n", cs);
}

static inline CallFrame *__callstack_push(lua_State *L, CallStack *cs){
    CallFrame *cf;

    if(cs->nb >= cs->cap){
        void *ud;
        lua_Alloc af = lua_getallocf(L, &ud);
        int newcap = cs->cap * 2;

        cs->stk = af(ud, cs->stk, cs->cap * sizeof(cs->stk[0]), newcap * sizeof(cs->stk[0]));
        cs->cap = newcap;
    }

    cf = &(cs->stk[cs->nb]);
    ++cs->nb;

    return cf;
}

static inline CallFrame *__callstack_pop(lua_State *L, CallStack *cs){
    return cs->nb > 0 ? (--cs->nb, &cs->stk[cs->nb]) : NULL;
}

static inline CallFrame *__callstack_top(lua_State *L, CallStack *cs){
    return cs->nb > 0 ? &cs->stk[cs->nb - 1] : NULL;
}

static inline void __callstackpool_newfreenode(lua_State *L, CallStackPool *csp){
    void *ud;
    lua_Alloc af = lua_getallocf(L, &ud);
    CallStack *cs;
    CallStack *nextnode;

    cs = af(ud, NULL, 0, sizeof(cs[0]));
    __callstack_init(L, cs);

    nextnode = csp->freelist.nextnode;
    csp->freelist.nextnode = cs;
    cs->nextnode = nextnode;
    ++csp->freenb;

    lplog("__callstackpool_newfreenode csp=%p,cs=%p,freenb=%d\n", csp, cs, csp->freenb);
}

static inline CallStack *__callstackpool_acquire(lua_State *L, CallStackPool *csp, void *key){
    void *val;
    CallStack *cs;

    if(imap_get(&csp->usedmap, (uint64_t)key, &val)){
        cs = val;
    }else{
        if(!csp->freelist.nextnode){
            __callstackpool_newfreenode(L, csp);
        }

        cs = csp->freelist.nextnode;
        csp->freelist.nextnode = cs->nextnode;
        --csp->freenb;
        ++csp->usednb;
        csp->stat_usednb = csp->usednb > csp->stat_usednb ? csp->usednb : csp->stat_usednb;

        imap_set(&csp->usedmap, (uint64_t)key, (void *)cs);

        lplog("__callstackpool_acquire csp=%p,key=%p\n", csp, key);
    }

    ++cs->ref;
    return cs;
}

static inline CallStack *__callstackpool_get(lua_State *L, CallStackPool *csp, void *key){
    void *val;

    return imap_get(&csp->usedmap, (uint64_t)key, &val) ? (CallStack *)val : NULL;
}

static inline void __callstackpool_release(lua_State *L, CallStackPool *csp, void *key){
    void *val;
    CallStack *cs;

    if(imap_get(&csp->usedmap, (uint64_t)key, &val)){
        cs = val;
        --cs->ref;

        if(cs->ref <= 0){
            CallStack *nextnode;

            imap_remove(&csp->usedmap, (uint64_t)key);
            nextnode = csp->freelist.nextnode;
            csp->freelist.nextnode = cs;
            cs->nextnode = nextnode;
            ++csp->freenb;
            --csp->usednb;
            cs->ref = 0;

            lplog("__callstackpool_release csp=%p,key=%p\n", csp, key);
        }
    }
}

static inline void __callstackpool_init(lua_State *L, CallStackPool *csp){
    imap_init(&csp->usedmap);
    csp->usednb = 0;
    csp->freenb = 0;
    csp->stat_usednb = 0;
    csp->freelist.nextnode = NULL;

    for(int i = 0; i < 100; ++i){
        __callstackpool_newfreenode(L, csp);
    }

    lplog("__callstackpool_init csp=%p\n", csp);
}

static inline void __callstackpool_freeusednodecb(void *ud, uint64_t key, void *val){
    lua_State *L = ud;
    __callstack_destroy(L, (CallStack *)val);

    lplog("__callstackpool_freeusednodecb key=%lu,val=%p\n", key, val);
}

static inline void __callstackpool_destroy(lua_State *L, CallStackPool *csp){
    CallStack *cs;

    while((cs = csp->freelist.nextnode) != NULL){
        csp->freelist.nextnode = cs->nextnode;
        __callstack_destroy(L, cs);
    }

    imap_foreach(&csp->usedmap, __callstackpool_freeusednodecb, L);
    imap_destroy(&csp->usedmap);

    lplog("__callstackpool_destroy csp=%p\n", csp);
}

static inline void __profilecontext_init(lua_State *L, ProfileContext *pc){
    imap_init(&pc->runnings);
    __callstackpool_init(L, &pc->stacks);
    __recordpool_init(L, &pc->records);
    pc->stat_lossnspan = 0;
    pc->stat_realnspan = 0;
    pc->stat_yieldnspan = 0;
    pc->enabled = true;
    pc->proto_yield = NULL;
    pc->trace_tailcall = false;

    lplog("__profilecontext_init pc=%p\n", pc);
}

static inline void __profilecontext_destroy(lua_State *L, ProfileContext *pc){
    __recordpool_destroy(L, &pc->records);
    __callstackpool_destroy(L, &pc->stacks);
    imap_destroy(&pc->runnings);

    lplog("__profilecontext_destroy pc=%p\n", pc);
}

static ProfileContext *__profilecontext_getorcreate(lua_State *L);

static int __profilecontext_gc(lua_State *L){
    void *ud;
    lua_Alloc af = lua_getallocf(L, &ud);
    ProfileContext *pc = *(ProfileContext **)lua_touserdata(L, -1);
    __profilecontext_destroy(L, pc);
    af(ud, pc, sizeof(pc[0]), 0);

    lplog("__profilecontext_gc pc=%p\n", pc);
    return 0;
}

static inline ProfileContext *__profilecontext_get(lua_State *L){
    ProfileContext *pc;

    lua_getfield(L, LUA_REGISTRYINDEX, LPROFILE_REGKEY_NAME);

    if(lua_isnil(L, -1)){
        lua_pop(L, 1);
        return NULL;
    }

    pc = *(ProfileContext **)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return pc;
}

static inline ProfileContext *__profilecontext_getorcreate(lua_State *L){
    ProfileContext *pc;

    lua_getfield(L, LUA_REGISTRYINDEX, LPROFILE_REGKEY_NAME);
    if(lua_isnil(L, -1)){
        void *ud;
        lua_Alloc af = lua_getallocf(L, &ud);
        ProfileContext **p;

        lua_pop(L, 1);
        pc = af(ud, NULL, 0, sizeof(pc[0]));
        __profilecontext_init(L, pc);
        p = lua_newuserdata(L, sizeof(p[0]));
        *p = pc;
        if(luaL_newmetatable(L, LPROFILE_METATBL_NAME)){
            lua_pushcfunction(L, __profilecontext_gc);
            lua_setfield(L, -2, "__gc");
        }

        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, LPROFILE_REGKEY_NAME);
    }

    pc = *(ProfileContext **)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return pc;
}

static inline uint64_t gethpc(){
    struct timespec ti;
    clock_gettime(CLOCK_REALTIME, &ti);
    return (uint64_t)1000000000 * ti.tv_sec + (uint64_t)ti.tv_nsec;
}

/* tailcall直接覆盖用的hook */
static void lua_hook_cb(lua_State *L, lua_Debug *ar){
    uint64_t event_hpc = gethpc();
    int event = ar->event;
    lua_Debug dbg;
    int ret;
    void *proto;
    const char *source;
    const char *name;
    const char *namewhat;
    const char *what;
    int line;
    ProfileContext *pc = __profilecontext_get(L);
    void *co;
    void *val;
    CallStack *cs;

    if(!pc || !pc->enabled){
        return;
    }

    lua_pushthread(L);
    co = (void *)lua_topointer(L, -1);
    lua_pop(L, 1);

    cs = __callstackpool_get(L, &pc->stacks, co);
    if(!cs){
        return;
    }

    if(!imap_get(&pc->runnings, (uint64_t)co, &val)){
        return;
    }

    ret = lua_getstack(L, 0, &dbg);
    if(!ret){
        return;
    }

    ret = lua_getinfo(L, "nSlf", &dbg);
    if(!ret){
        return;
    }

    proto = (void *)lua_topointer(L, -1);
    source = dbg.source;
    name = dbg.name;
    namewhat = dbg.namewhat;
    what = dbg.what;
    line = dbg.linedefined;

    lplog("lua_hook_cb proto=%p,event=%d,name=%s,source=%s\n", proto, event, name ? name : "", source ? source : "");

    if(event == LUA_HOOKCALL){
        uint64_t hpc;
        CallFrame *cf = __callstack_push(L, cs);
        cf->proto = proto;
        cf->source = source;
        cf->name = name;
        cf->namewhat = namewhat;
        cf->what = what;
        cf->line = line;
        cf->call_evt_hpc = event_hpc;
        cf->ret_hpc = 0;
        cf->istailcall = 0;
        cf->total_nspan = 0;
        cf->real_nspan = 0;
        cf->sub_nspan = 0;
        cf->yield_nspan = 0;

        hpc = gethpc();
        cf->call_real_hpc = hpc;
        pc->stat_lossnspan += hpc - event_hpc;
    }else if(event == LUA_HOOKTAILCALL){
        CallFrame *cf = __callstack_top(L, cs);
        if(cf){
            cf->proto = proto;
            cf->source = source;
            cf->name = name;
            cf->namewhat = namewhat;
            cf->what = what;
            cf->line = line;
            cf->istailcall = 1;
        }

        pc->stat_lossnspan += gethpc() - event_hpc;
    }else if(event == LUA_HOOKRET){
        uint64_t hpc;
        CallFrame *cf;
        CallFrame *precf;

        while((cf = __callstack_pop(L, cs)) != NULL && cf->proto != proto){
            lplog("lua_hook_cb proto not match this=%p,recorded=%p\n", proto, cf->proto);
        }

        if(!cf){
            lplog("lua_hook_cb proto no call frame this=%p\n", proto);
            return;
        }

        cf->ret_hpc = event_hpc;
        cf->total_nspan = event_hpc - cf->call_real_hpc;
        cf->real_nspan = cf->total_nspan - cf->sub_nspan;

        if(proto == pc->proto_yield){
            cf->yield_nspan += cf->real_nspan;
            pc->stat_yieldnspan += cf->real_nspan;
        }

        __recordpool_record(L, &pc->records, cf);

        precf = __callstack_top(L, cs);
        hpc = gethpc();
        if(precf){
            precf->sub_nspan += hpc - cf->call_evt_hpc;
            precf->yield_nspan += cf->yield_nspan;
        }

        pc->stat_realnspan += cf->real_nspan;
        pc->stat_lossnspan += hpc - event_hpc;
    }
}

/* 追踪tailcall用的hook */
static void lua_hook_cb_tracetailcall(lua_State *L, lua_Debug *ar){
    uint64_t event_hpc = gethpc();
    int event = ar->event;
    lua_Debug dbg;
    int ret;
    void *proto;
    const char *source;
    const char *name;
    const char *namewhat;
    const char *what;
    int line;
    ProfileContext *pc = __profilecontext_get(L);
    void *co;
    void *val;
    CallStack *cs;

    if(!pc || !pc->enabled){
        return;
    }

    lua_pushthread(L);
    co = (void *)lua_topointer(L, -1);
    lua_pop(L, 1);

    cs = __callstackpool_get(L, &pc->stacks, co);
    if(!cs){
        return;
    }

    if(!imap_get(&pc->runnings, (uint64_t)co, &val)){
        return;
    }

    ret = lua_getstack(L, 0, &dbg);
    if(!ret){
        return;
    }

    ret = lua_getinfo(L, "nSlf", &dbg);
    if(!ret){
        return;
    }

    proto = (void *)lua_topointer(L, -1);
    source = dbg.source;
    name = dbg.name;
    namewhat = dbg.namewhat;
    what = dbg.what;
    line = dbg.linedefined;

    lplog("lua_hook_cb proto=%p,event=%d,name=%s,source=%s\n", proto, event, name ? name : "", source ? source : "");

    if(event == LUA_HOOKCALL){
        uint64_t hpc;
        CallFrame *cf = __callstack_push(L, cs);
        cf->proto = proto;
        cf->source = source;
        cf->name = name;
        cf->namewhat = namewhat;
        cf->what = what;
        cf->line = line;
        cf->call_evt_hpc = event_hpc;
        cf->ret_hpc = 0;
        cf->istailcall = 0;
        cf->total_nspan = 0;
        cf->real_nspan = 0;
        cf->sub_nspan = 0;
        cf->yield_nspan = 0;

        hpc = gethpc();
        cf->call_real_hpc = hpc;
        pc->stat_lossnspan += hpc - event_hpc;
    }else if(event == LUA_HOOKTAILCALL){
        uint64_t hpc;
        CallFrame *cf = __callstack_push(L, cs);
        cf->proto = proto;
        cf->source = source;
        cf->name = name;
        cf->namewhat = namewhat;
        cf->what = what;
        cf->line = line;
        cf->call_evt_hpc = event_hpc;
        cf->ret_hpc = 0;
        cf->istailcall = 1;
        cf->total_nspan = 0;
        cf->real_nspan = 0;
        cf->sub_nspan = 0;
        cf->yield_nspan = 0;

        hpc = gethpc();
        cf->call_real_hpc = hpc;
        pc->stat_lossnspan += hpc - event_hpc;
    }else if(event == LUA_HOOKRET){
        uint64_t hpc;
        CallFrame *cf;
        CallFrame *precf;

        /* 无论是不是tailcall，ret必须匹配得上callstack的栈顶，否则丢弃 */
        while((cf = __callstack_pop(L, cs)) != NULL && cf->proto != proto){
            lplog("lua_hook_cb proto not match this=%p,recorded=%p\n", proto, cf->proto);
        }

        if(!cf){
            lplog("lua_hook_cb proto no call frame this=%p\n", proto);
            return;
        }

        do {
            cf->ret_hpc = event_hpc;
            cf->total_nspan = cf->ret_hpc - cf->call_real_hpc;
            cf->real_nspan = cf->total_nspan - cf->sub_nspan;

            if(proto == pc->proto_yield){
                cf->yield_nspan += cf->real_nspan;
                pc->stat_yieldnspan += cf->real_nspan;
            }

            __recordpool_record(L, &pc->records, cf);

            precf = __callstack_top(L, cs);
            hpc = gethpc();
            if(precf){
                precf->sub_nspan += hpc - cf->call_evt_hpc;
                precf->yield_nspan += cf->yield_nspan;
            }

            pc->stat_realnspan += cf->real_nspan;

        }while(cf->istailcall && (cf = __callstack_pop(L, cs)) != NULL);

        pc->stat_lossnspan += gethpc() - event_hpc;
    }
}


static int pbegin(lua_State *L){
    ProfileContext *pc = __profilecontext_getorcreate(L);
    void *co;
    CallStack *cs;

    lua_pushthread(L);
    co = (void *)lua_topointer(L, -1);
    lua_pop(L, 1);

    imap_set(&pc->runnings, (uint64_t)co, (void *)1);

    if(pc->trace_tailcall){
        lua_sethook(L, lua_hook_cb_tracetailcall, LUA_MASKCALL | LUA_MASKRET, 0);
    }else{
        lua_sethook(L, lua_hook_cb, LUA_MASKCALL | LUA_MASKRET, 0);
    }

    cs = __callstackpool_acquire(L, &pc->stacks, co);
    cs->nb = 0;

    return 0;
}

static int pend(lua_State *L){
    ProfileContext *pc = __profilecontext_getorcreate(L);
    void *co;

    lua_pushthread(L);
    co = (void *)lua_topointer(L, -1);
    lua_pop(L, 1);

    imap_remove(&pc->runnings, (uint64_t)co);
    lua_sethook(L, NULL, 0, 0);

    __callstackpool_release(L, &pc->stacks, co);

    return 0;
}

static int pclear(lua_State *L){
    ProfileContext *pc = __profilecontext_getorcreate(L);
    __recordpool_clear(L, &pc->records);
    pc->stat_lossnspan = 0;
    pc->stat_realnspan = 0;
    pc->stat_yieldnspan = 0;
    return 0;
}

static void dump_one_cb(void *ud, uint64_t key, void *val){
    lua_State *L = ((DumpArg *)ud)->L;
    ProfileContext *pc = ((DumpArg *)ud)->pc;
    ProtoRecord *pr = &pc->records.pool[(uint64_t)val];

    lua_pushinteger(L, (uint64_t)pr->proto);
    lua_newtable(L);

    lua_pushinteger(L, (uint64_t)pr->proto);
    lua_setfield(L, -2, "proto");
    lua_pushstring(L, pr->source);
    lua_setfield(L, -2, "source");
    lua_pushstring(L, pr->name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, pr->namewhat);
    lua_setfield(L, -2, "namewhat");
    lua_pushstring(L, pr->what);
    lua_setfield(L, -2, "what");
    lua_pushinteger(L, pr->line);
    lua_setfield(L, -2, "line");
    lua_pushinteger(L, pr->callnb);
    lua_setfield(L, -2, "callnb");
    lua_pushinteger(L, pr->total_nspan);
    lua_setfield(L, -2, "total_nspan");
    lua_pushinteger(L, pr->real_nspan);
    lua_setfield(L, -2, "real_nspan");
    lua_pushboolean(L, pr->istailcall);
    lua_setfield(L, -2, "istailcall");
    lua_pushinteger(L, pr->coroutine_nspan);
    lua_setfield(L, -2, "coroutine_nspan");

    lua_settable(L, -3);
}

static int pdump(lua_State *L){
    ProfileContext *pc = __profilecontext_getorcreate(L);
    
    DumpArg ud;

    ud.L = L;
    ud.pc = pc;

    lua_newtable(L);
    imap_foreach(&pc->records.usedmap, dump_one_cb, &ud);

    return 1;
}

static int preset(lua_State *L){
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, LPROFILE_REGKEY_NAME);
    return 0;
}

static int pinfo(lua_State *L){
    ProfileContext *pc = __profilecontext_getorcreate(L);

    lua_newtable(L);

    lua_pushinteger(L, pc->stacks.usednb);
    lua_setfield(L, -2, "stackpoolusednb");
    lua_pushinteger(L, pc->stacks.freenb);
    lua_setfield(L, -2, "stackpoolfreenb");
    lua_pushinteger(L, pc->records.cap);
    lua_setfield(L, -2, "recordpoolcap");
    lua_pushinteger(L, pc->records.nb);
    lua_setfield(L, -2, "recordpoolnb");
    lua_pushinteger(L, pc->stacks.stat_usednb);
    lua_setfield(L, -2, "stackpoolstatusednb");
    lua_pushinteger(L, pc->stat_lossnspan);
    lua_setfield(L, -2, "stat_lossnspan");
    lua_pushinteger(L, pc->stat_realnspan);
    lua_setfield(L, -2, "stat_realnspan");
    lua_pushboolean(L, pc->enabled ? 1 : 0);
    lua_setfield(L, -2, "enabled");
    lua_pushinteger(L, (uint64_t)pc->proto_yield);
    lua_setfield(L, -2, "proto_yield");
    lua_pushboolean(L, pc->trace_tailcall ? 1 : 0);
    lua_setfield(L, -2, "trace_tailcall");
    lua_pushinteger(L, pc->stat_yieldnspan);
    lua_setfield(L, -2, "stat_yieldnspan");

    return 1;
}

static int penable(lua_State *L){
    ProfileContext *pc = __profilecontext_getorcreate(L);
    pc->enabled = true;

    return 0;
}

static int pdisable(lua_State *L){
    ProfileContext *pc = __profilecontext_getorcreate(L);
    pc->enabled = false;

    return 0;
}

static int psetyieldproto(lua_State *L){
    void *yield;
    ProfileContext *pc;

    if(lua_isnoneornil(L, 1)){
        yield = NULL;
    }else{
        yield = (void *)lua_topointer(L, 1);
    }

    pc = __profilecontext_getorcreate(L);
    pc->proto_yield = yield;

    return 0;
}

static int pgetyieldproto(lua_State *L){
    ProfileContext *pc = __profilecontext_getorcreate(L);

    lua_pushinteger(L, (uint64_t)pc->proto_yield);

    return 1;
}

static int ptracetailcall(lua_State *L){
    bool val;
    ProfileContext *pc;

    if(lua_isnoneornil(L, 1)){
        val = false;
    }else{
        val = (bool)lua_toboolean(L, 1);
    }

    pc = __profilecontext_getorcreate(L);
    pc->trace_tailcall = val;

    return 0;
}

int luaopen_lprofile_c(lua_State *L){
    luaL_checkversion(L);

    luaL_Reg lib[] = {
        {"pbegin", pbegin},
        {"pend", pend},
        {"pclear", pclear},
        {"pdump", pdump},
        {"preset", preset},
        {"pinfo", pinfo},
        {"penable", penable},
        {"pdisable", pdisable},
        {"psetyieldproto", psetyieldproto},
        {"pgetyieldproto", pgetyieldproto},
        {"ptracetailcall", ptracetailcall},
        {NULL, NULL},
    };

    luaL_newlib(L, lib);
    return 1;
}




