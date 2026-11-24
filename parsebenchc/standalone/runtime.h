#ifndef PARSEBENCH_C_RUNTIME_H
#define PARSEBENCH_C_RUNTIME_H
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include "vendor/utf8proc.h"
#ifdef _MSC_VER
#define TLS __declspec(thread)
#else
#define TLS _Thread_local
#endif

typedef uintptr_t V;
typedef struct Obj Obj;
typedef struct Env Env;
typedef V (*Code)(Env *);
typedef struct Entry { V key, value; uint64_t hash; } Entry;
enum { T_FLOAT=1,T_STR,T_BYTES,T_LIST,T_TUPLE,T_MAP,T_SET,T_FN,T_NATIVE,
       T_CLASS,T_INSTANCE,T_MODULE,T_BOUND,T_SLICE,T_REGEX,T_MATCH,T_CONTEXT,
       T_FACTORY,T_ARRAY,T_ITER };
enum { OP_ADD,OP_SUB,OP_MULT,OP_DIV,OP_FLOORDIV,OP_MOD,OP_POW,OP_BITOR,
       OP_BITAND,OP_BITXOR,OP_LSHIFT,OP_RSHIFT,OP_NOT,OP_USUB,OP_UADD,OP_INVERT,
       OP_EQ,OP_NOTEQ,OP_LT,OP_LTE,OP_GT,OP_GTE,OP_IN,OP_NOTIN,OP_IS,OP_ISNOT };
enum { FN_PROPERTY=1,FN_STATIC=2,FN_CLASS=4,FN_CACHE=8,FN_CLONE=16,FN_ATTACH=32 };
#define NONE ((V)3)
#define UNBOUND ((V)7)
#define FALSEV ((V)2)
#define TRUEV ((V)10)
struct Env { V vars; Env *parent; V *locals; Obj *function; };
struct Obj {
    int type, flags;
    uint64_t hash;
    size_t n, cap;
    char *s;
    V *values;
    Entry *entries;
    V a,b,c,d;
    double f;
    Env *env;
    Code code;
    void *ptr;
    V locals_map;
    size_t slots;
    unsigned profile_index;
};
typedef struct Trap { jmp_buf jump; struct Trap *previous; } Trap;
extern TLS char error_message[2048];
extern TLS Trap *trap_head;
void *mem(size_t size);
void fail(const char *fmt,...);
void trap_push(Trap *t);
void trap_pop(Trap *t);
void raise_value(V value);
Obj *obj(V value);
int type(V value);
V integer(int64_t value);
V number(double value);
V constant_float(double value);
V constant_string(const char*value,size_t n);
#define S(x) constant_string(x,sizeof(x)-1)
int64_t asint(V value);
double asnum(V value);
V str(const char *value);
V strn(const char *value,size_t n);
const char *cstr(V value);
V stringify(V value,int repr);
V format_has(V token,V kind);
size_t length(V value);
int truth(V value);
V list_new(void);
void append(V list,V value);
void extend(V list,V iterable);
V iter_values(V value);
V set_from(V value);
V map_new(void);
void map_set(V map,V key,V value);
V map_find(V map,V key,V fallback);
void map_update(V map,V other);
void map_del(V map,V key);
Env *env_new(Env *parent);
V get(Env *e,const char *name);
void put(Env *e,const char *name,V value);
void nonlocal_set(Env *e,const char *name,V value);
V attr(V value,const char *name);
void attr_set(V value,const char *name,V child);
V item(V value,V key);
void item_set(V value,V key,V child);
void item_delete(V value,V key);
V slice_new(V lo,V hi,V step);
V slice_get(V value,int64_t lo,int64_t hi,int64_t step);
V binary(int op,V a,V b);
V inplace(int op,V a,V b);
V unary(int op,V a);
V compare(int op,V a,V b);
V function_new(Code code,Env *closure,V params,V defaults,int positional,const char *vararg,const char *kwarg,const char *name,V slots);
V call(V function,V args,V kwargs);
V call_pos(V function,size_t count,V *values);
V class_new(V name,Env *parent);
void class_field(V cls,V name,V fallback);
V module_get(const char *name);
V join(V separator,V values);
V format_value(V value,V format,int conversion);
V json_parse(const char *src,size_t length,size_t *consumed);
void json_write(FILE *out,V value);
void runtime_init(void);
void arena_reset(void);
#endif
