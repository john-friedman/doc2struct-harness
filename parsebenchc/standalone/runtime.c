#include "runtime.h"
#define PCRE2_CODE_UNIT_WIDTH 8
#include "vendor/pcre2.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef struct Block {struct Block *next;size_t used,capacity; unsigned char bytes[1];} Block;
static TLS Block *blocks,*current;
static TLS Env *builtins;
static TLS V modules;
static TLS V symbols,empty_kwargs;
static TLS const char *call_stack[1024];
static TLS int call_depth;
TLS char error_message[2048];
TLS Trap *trap_head;
typedef struct {Code code;const char*name;uint64_t bytes,calls;int64_t ticks;} AllocationStat;
static int64_t profile_clock(void){
#ifdef _WIN32
    LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart;
#else
    return (int64_t)clock();
#endif
}
static TLS AllocationStat allocation_stats[2048];
static TLS unsigned active_allocation;
static TLS int profile_allocations;
static TLS int memo_disabled;
static TLS const char*dump_function;
typedef struct {const char*key;size_t n;V value;unsigned generation;} StringConstant;
typedef struct {uint64_t key;V value;unsigned generation;} FloatConstant;
static TLS StringConstant string_constants[8192];
static TLS FloatConstant float_constants[256];
static TLS unsigned constant_generation;
static TLS V zero_float,one_float,half_float;

void *mem(size_t n){
    n=(n+7)&~(size_t)7;
    if(profile_allocations)allocation_stats[active_allocation].bytes+=n;
    if(!current || current->used+n>current->capacity){
        Block *b=current?current->next:blocks;
        if(!b || b->capacity<n){
            size_t cap=n>(4u<<20)?n:(4u<<20);
            b=calloc(1,sizeof(Block)+cap);if(!b){fprintf(stderr,"out of memory\n");exit(2);}
            b->capacity=cap;
            if(current){b->next=current->next;current->next=b;}else{b->next=blocks;blocks=b;}
        }
        current=b;
    }
    void *p=current->bytes+current->used;current->used+=n;memset(p,0,n);return p;
}
void arena_reset(void){size_t kept=0;Block*b=blocks,*last=NULL;while(b){Block*next=b->next;if(kept>=(64u<<20)){if(last)last->next=next;else blocks=next;free(b);}else{b->used=0;kept+=b->capacity;last=b;}b=next;}current=blocks;call_depth=0;trap_head=NULL;++constant_generation;zero_float=one_float=half_float=0;}
void fail(const char *fmt,...){
    char text[2048];va_list a;va_start(a,fmt);vsnprintf(text,sizeof(text),fmt,a);va_end(a);
    snprintf(error_message,sizeof(error_message),"%s",text);
    if(trap_head)longjmp(trap_head->jump,1);
    fprintf(stderr,"%s\n",text);for(int i=call_depth-1;i>=0;--i)fprintf(stderr,"  in %s\n",call_stack[i]);exit(2);
}
void trap_push(Trap *t){t->previous=trap_head;trap_head=t;}
void trap_pop(Trap *t){trap_head=t->previous;}
void raise_value(V v){fail("%s",cstr(stringify(v,0)));}
Obj *obj(V v){if(!v ||(v&7))fail("expected object, got tag %llu",(unsigned long long)v);return (Obj*)v;}
int type(V v){if((v&7)==1)return -1;if((v&7)==2)return -2;if(v==NONE||v==UNBOUND)return 0;return obj(v)->type;}
static V fresh(int t){Obj *o=mem(sizeof(Obj));o->type=t;return(V)o;}
V integer(int64_t n){return ((V)n<<3)|1;}
V number(double n){V*cached=n==0&&!signbit(n)?&zero_float:n==1?&one_float:n==.5?&half_float:NULL;if(cached&&*cached)return*cached;V v=fresh(T_FLOAT);obj(v)->f=n;if(cached)*cached=v;return v;}
V constant_float(double n){uint64_t bits;memcpy(&bits,&n,8);size_t pos=(size_t)(bits^(bits>>32))&255;while(float_constants[pos].generation==constant_generation&&float_constants[pos].key!=bits)pos=(pos+1)&255;FloatConstant*c=float_constants+pos;if(c->generation!=constant_generation){c->generation=constant_generation;c->key=bits;c->value=number(n);}return c->value;}
int64_t asint(V v){if((v&7)==1)return (int64_t)v>>3;if((v&7)==2)return v==TRUEV;if(type(v)==T_FLOAT)return(int64_t)obj(v)->f;if(type(v)==T_STR)return strtoll(obj(v)->s,NULL,10);fail("integer conversion of type %d",type(v));return 0;}
double asnum(V v){return type(v)==T_FLOAT?obj(v)->f:(double)asint(v);}
V strn(const char *s,size_t n){V v=fresh(T_STR);obj(v)->s=mem(n+1);memcpy(obj(v)->s,s,n);obj(v)->n=n;return v;}
V str(const char*s){return strn(s,strlen(s));}
V constant_string(const char*s,size_t n){size_t pos=((uintptr_t)s>>3)&8191;while(string_constants[pos].generation==constant_generation&&(string_constants[pos].key!=s||string_constants[pos].n!=n))pos=(pos+1)&8191;StringConstant*c=string_constants+pos;if(c->generation!=constant_generation){c->generation=constant_generation;c->key=s;c->n=n;c->value=strn(s,n);}return c->value;}
static V symbol(const char*s){Obj temporary={0};temporary.type=T_STR;temporary.s=(char*)s;temporary.n=strlen(s);V key=(V)&temporary;V found=map_find(symbols,key,UNBOUND);if(found!=UNBOUND)return found;found=str(s);map_set(symbols,found,found);return found;}
const char *cstr(V v){if(type(v)!=T_STR && type(v)!=T_BYTES)fail("expected string, got %d",type(v));return obj(v)->s;}
static const char*bytes_find(const char*s,size_t n,const char*needle,size_t size){if(size>n)return NULL;if(!size)return s;for(size_t p=0;p+size<=n;++p)if(s[p]==needle[0]&&!memcmp(s+p,needle,size))return s+p;return NULL;}
static uint32_t decode(const char *s,size_t n,size_t *p){
    if(*p>=n)return 0;unsigned char c=(unsigned char)s[(*p)++];if(c<128)return c;
    uint32_t cp=c& (c<224?31:c<240?15:7);int tail=c<224?1:c<240?2:3;
    while(tail--&&*p<n)cp=(cp<<6)|((unsigned char)s[(*p)++]&63);return cp;
}
static size_t encode(uint32_t c,char *s){
    if(c<128){s[0]=(char)c;return 1;}if(c<2048){s[0]=(char)(192|(c>>6));s[1]=(char)(128|(c&63));return 2;}
    if(c<65536){s[0]=(char)(224|(c>>12));s[1]=(char)(128|((c>>6)&63));s[2]=(char)(128|(c&63));return 3;}
    s[0]=(char)(240|(c>>18));s[1]=(char)(128|((c>>12)&63));s[2]=(char)(128|((c>>6)&63));s[3]=(char)(128|(c&63));return 4;
}
static V chr_value(uint32_t c){char buf[4];return strn(buf,encode(c,buf));}
static size_t unicode_len(V s){Obj *o=obj(s);if(o->cap)return o->cap-1;size_t p=0,n=0;while(p<o->n){decode(o->s,o->n,&p);++n;}o->cap=n+1;return n;}
static uint32_t*unicode_offsets(V s){Obj*o=obj(s);if(o->ptr)return o->ptr;size_t n=unicode_len(s),p=0;uint32_t*index=mem((n+1)*sizeof(uint32_t));for(size_t i=0;i<n;++i){index[i]=(uint32_t)p;decode(o->s,o->n,&p);}index[n]=(uint32_t)o->n;o->ptr=index;return index;}
static size_t bytepos(V s,int64_t cp){size_t n=unicode_len(s);if(cp<0)cp+=n;if(cp<0)cp=0;if((size_t)cp>n)cp=(int64_t)n;if(n==obj(s)->n)return(size_t)cp;if(obj(s)->n>64)return unicode_offsets(s)[cp];size_t p=0;while(cp--)decode(obj(s)->s,obj(s)->n,&p);return p;}
static size_t cppos(V s,size_t b){size_t n=unicode_len(s);if(n==obj(s)->n)return b;if(obj(s)->n>64){uint32_t*index=unicode_offsets(s);size_t lo=0,hi=n;while(lo<hi){size_t mid=lo+(hi-lo)/2;if(index[mid]<b)lo=mid+1;else hi=mid;}return lo;}size_t p=0,cp=0;while(p<b&&p<obj(s)->n){decode(obj(s)->s,obj(s)->n,&p);++cp;}return cp;}
size_t length(V v){int t=type(v);if(t==T_STR)return unicode_len(v);if(t==T_ITER)return length(obj(v)->a)-(size_t)asint(obj(v)->b);if(t==T_INSTANCE){V f=attr(v,"__len__");return(size_t)asint(call(f,list_new(),map_new()));}if(t==0)return 0;return obj(v)->n;}
int truth(V v){int t=type(v);if(t==0)return 0;if(t==-1)return asint(v)!=0;if(t==-2)return v==TRUEV;if(t==T_FLOAT)return obj(v)->f!=0;if(t==T_STR||t==T_BYTES||t==T_LIST||t==T_TUPLE||t==T_MAP||t==T_SET)return obj(v)->n!=0;return 1;}
V list_new(void){return fresh(T_LIST);}
void append(V v,V child){Obj *o=obj(v);if(o->n==o->cap){size_t c=o->cap?o->cap*2:8;V *p=mem(c*sizeof(V));if(o->n)memcpy(p,o->values,o->n*sizeof(V));o->values=p;o->cap=c;}o->values[o->n++]=child;}
static int equal(V a,V b);
static int ordering(V a,V b);
static uint64_t hash_value(V v){
    int t=type(v);if(t==-1||t==-2)return(uint64_t)asint(v)*UINT64_C(11400714819323198485);if(t==0)return 8;
    if(t==T_FLOAT){double n=asnum(v);if(floor(n)==n)return(uint64_t)(int64_t)n*UINT64_C(11400714819323198485);uint64_t h;memcpy(&h,&n,8);return h;}
    Obj*o=obj(v);if(o->hash)return o->hash;
    uint64_t h=UINT64_C(1469598103934665603);
    if(t==T_STR||t==T_BYTES){for(size_t i=0;i<o->n;++i)h=(h^(unsigned char)o->s[i])*UINT64_C(1099511628211);}
    else if(t==T_TUPLE||t==T_LIST){for(size_t i=0;i<o->n;++i)h=(h^hash_value(o->values[i]))*UINT64_C(1099511628211);}
    else if(t==T_INSTANCE){for(size_t i=0;i<o->slots;++i)h=(h^hash_value(o->values[i]))*UINT64_C(1099511628211);}
    else if(t==T_SET){h=UINT64_C(1927868237);if(o->a)for(size_t i=0;i<length(o->a);++i){V key=item(o->a,integer(i));uint64_t x=hash_value(key);x^=x>>16;x*=UINT64_C(0x9e3779b185ebca87);h^=x;}h^=o->n*UINT64_C(0x517cc1b727220a95);}
    else h=(uint64_t)v;
    o->hash=h?h:1;return o->hash;
}
V map_new(void){return fresh(T_MAP);}
static void map_grow(Obj *o){
    size_t cap=o->cap?o->cap*2:16;Entry *next=mem(cap*sizeof(Entry));
    for(size_t i=0;i<o->cap;++i)if(o->entries[i].key && o->entries[i].key!=UNBOUND){Entry e=o->entries[i];size_t p=e.hash&(cap-1);while(next[p].key)p=(p+1)&(cap-1);next[p]=e;}
    o->entries=next;o->cap=cap;
}
void map_set(V map,V key,V child){
    Obj *o=obj(map);if(!o->cap||o->n*3>=o->cap*2)map_grow(o);
    uint64_t h=hash_value(key);size_t p=h&(o->cap-1),dead=SIZE_MAX,scanned=0;
    while(o->entries[p].key&&scanned++<o->cap){if(o->entries[p].key==UNBOUND){if(dead==SIZE_MAX)dead=p;}else if(o->entries[p].hash==h&&equal(o->entries[p].key,key)){o->entries[p].value=child;o->hash=0;return;}p=(p+1)&(o->cap-1);}
    if(scanned>=o->cap&&dead==SIZE_MAX){map_grow(o);map_set(map,key,child);return;}
    if(dead!=SIZE_MAX)p=dead;
    o->entries[p]=(Entry){key,child,h};++o->n;o->hash=0;
    /* Insertion order is a separate dense vector, as in Python dictionaries. */
    if(!o->a)o->a=list_new();append(o->a,key);
}
V map_find(V map,V key,V fallback){
    Obj *o=obj(map);if(!o->cap)return fallback;uint64_t h=hash_value(key);size_t p=h&(o->cap-1),scanned=0;
    while(o->entries[p].key&&scanned++<o->cap){if(o->entries[p].key!=UNBOUND&&o->entries[p].hash==h&&equal(o->entries[p].key,key))return o->entries[p].value;p=(p+1)&(o->cap-1);}return fallback;
}
void map_del(V map,V key){Obj*o=obj(map);if(!o->cap)return;uint64_t h=hash_value(key);size_t p=h&(o->cap-1),scanned=0;while(o->entries[p].key&&scanned++<o->cap){if(o->entries[p].key!=UNBOUND&&o->entries[p].hash==h&&equal(o->entries[p].key,key)){o->entries[p].key=UNBOUND;--o->n;o->hash=0;if(o->a){Obj*keys=obj(o->a);for(size_t i=0;i<keys->n;++i)if(equal(keys->values[i],key)){memmove(keys->values+i,keys->values+i+1,(keys->n-i-1)*sizeof(V));--keys->n;break;}}return;}p=(p+1)&(o->cap-1);}}
static V map_keys(V map){V out=list_new();Obj*o=obj(map);if(o->a)for(size_t i=0;i<length(o->a);++i){V k=item(o->a,integer(i));if(map_find(map,k,UNBOUND)!=UNBOUND)append(out,k);}return out;}
V iter_values(V v){
    int t=type(v);if(t==T_LIST||t==T_TUPLE)return v;
    if(t==T_MAP||t==T_SET)return map_keys(v);
    if(t==T_STR||t==T_BYTES){V out=list_new();size_t p=0;while(p<obj(v)->n){size_t a=p;if(t==T_STR)decode(obj(v)->s,obj(v)->n,&p);else ++p;append(out,strn(obj(v)->s+a,p-a));}return out;}
    if(t==T_ITER){V out=slice_get(obj(v)->a,asint(obj(v)->b),(int64_t)length(obj(v)->a),1);obj(v)->b=integer(length(obj(v)->a));return out;}
    if(t==T_INSTANCE){V m=attr(v,"__iter__");return iter_values(call(m,list_new(),map_new()));}
    fail("not iterable: type %d",t);return NONE;
}
void extend(V list,V values){V v=iter_values(values);size_t n=length(v);for(size_t i=0;i<n;++i)append(list,item(v,integer(i)));}
V set_from(V v){V out=map_new();obj(out)->type=T_SET;V seq=iter_values(v);for(size_t i=0;i<length(seq);++i)map_set(out,item(seq,integer(i)),TRUEV);return out;}
void map_update(V m,V other){V keys=map_keys(other);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i));map_set(m,k,map_find(other,k,NONE));}}
static V clone_value(V v){int t=type(v);if(t==T_LIST||t==T_TUPLE){V out=list_new();for(size_t i=0;i<length(v);++i)append(out,clone_value(item(v,integer(i))));obj(out)->type=t;return out;}if(t==T_MAP||t==T_SET){V out=map_new();obj(out)->type=t;obj(out)->flags=obj(v)->flags;V keys=map_keys(v);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i));map_set(out,k,clone_value(map_find(v,k,NONE)));}return out;}if(t==T_INSTANCE){Obj*src=obj(v);V out=fresh(T_INSTANCE);Obj*dst=obj(out);dst->a=src->a;dst->slots=src->slots;dst->values=mem((dst->slots+1)*sizeof(V));for(size_t i=0;i<dst->slots;++i)dst->values[i]=clone_value(src->values[i]);if(src->b)dst->b=clone_value(src->b);return out;}return v;}
static V instance_map(V v){Obj*o=obj(v);V out=map_new(),fields=obj(o->a)->b;for(size_t i=0;i<o->slots;++i)if(o->values[i]!=UNBOUND)map_set(out,item(fields,integer(i)),o->values[i]);if(o->b)map_update(out,o->b);return out;}
Env *env_new(Env *p){Env*e=mem(sizeof(Env));e->parent=p;return e;}
V get(Env *e,const char *name){V k=symbol(name);for(Env*p=e;p;p=p->parent){if(p->function){V slot=map_find(p->function->locals_map,k,UNBOUND);if(slot!=UNBOUND){V value=p->locals[asint(slot)];if(value==UNBOUND)fail("unbound local %s",name);return value;}}if(p->vars){V v=map_find(p->vars,k,UNBOUND);if(v!=UNBOUND)return v;}}if(builtins&&builtins->vars){V v=map_find(builtins->vars,k,UNBOUND);if(v!=UNBOUND)return v;}fail("undefined name: %s",name);return NONE;}
void put(Env *e,const char *name,V v){V k=symbol(name);if(e->function){V slot=map_find(e->function->locals_map,k,UNBOUND);if(slot!=UNBOUND){e->locals[asint(slot)]=v;return;}}if(!e->vars)e->vars=map_new();map_set(e->vars,k,v);}
void nonlocal_set(Env *e,const char *name,V v){V k=symbol(name);for(Env*p=e->parent;p;p=p->parent){if(p->function){V slot=map_find(p->function->locals_map,k,UNBOUND);if(slot!=UNBOUND){p->locals[asint(slot)]=v;return;}}if(p->vars&&map_find(p->vars,k,UNBOUND)!=UNBOUND){map_set(p->vars,k,v);return;}}put(e,name,v);}
V slice_new(V lo,V hi,V step){V v=fresh(T_SLICE);obj(v)->a=lo;obj(v)->b=hi;obj(v)->c=step;return v;}
V slice_get(V v,int64_t lo,int64_t hi,int64_t step){
    int64_t n=(int64_t)length(v);if(!step)fail("zero slice step");
    if(lo<0)lo+=n;if(hi<0)hi+=n;
    if(step>0){if(lo<0)lo=0;if(hi<0)hi=0;if(lo>n)lo=n;if(hi>n)hi=n;}
    else{if(lo>=n)lo=n-1;if(hi>=n)hi=n-1;}
    if(type(v)==T_STR||type(v)==T_BYTES){
        if(step==1){if(hi<lo)hi=lo;size_t a=bytepos(v,lo),b=bytepos(v,hi);return strn(cstr(v)+a,b-a);}
        V parts=list_new();for(int64_t i=lo;step>0?i<hi:i>hi;i+=step)append(parts,item(v,integer(i)));return join(S(""),parts);
    }
    V out=list_new();for(int64_t i=lo;step>0?i<hi:i>hi;i+=step)if(i>=0&&i<n)append(out,item(v,integer(i)));if(type(v)==T_TUPLE)obj(out)->type=T_TUPLE;return out;
}
static V slice_item(V v,V key){Obj*s=obj(key);int64_t step=s->c==NONE?1:asint(s->c),n=(int64_t)length(v);int64_t lo=s->a==NONE?(step>0?0:n-1):asint(s->a),hi=s->b==NONE?(step>0?n:-n-1):asint(s->b);return slice_get(v,lo,hi,step);}
static V match_group(V match,V key);
V item(V v,V key){
    int t=type(v);if(t==T_NATIVE||t==T_CLASS)return v; /* erased generic type alias */
    if(t==T_INSTANCE){V m=obj(v)->b?map_find(obj(v)->b,S("__getitem__"),UNBOUND):UNBOUND;if(m==UNBOUND)m=attr(v,"__getitem__");V a=list_new();append(a,key);return call(m,a,map_new());}
    if(t==T_MATCH)return match_group(v,key);
    if(t==T_ARRAY){
        Obj*o=obj(v);if(type(key)==T_TUPLE){V r=item(key,integer(0)),c=item(key,integer(1));
            if(type(r)==T_SLICE&&c==NONE){V out=fresh(T_ARRAY);*obj(out)=*o;obj(out)->a=integer(o->n);obj(out)->b=integer(1);obj(out)->flags=2;return out;}
            if(r==NONE&&type(c)==T_SLICE){V out=fresh(T_ARRAY);*obj(out)=*o;obj(out)->a=integer(1);obj(out)->b=integer(o->n);obj(out)->flags=2;return out;}
            return number(((double*)o->ptr)[asint(r)*(size_t)asint(o->b)+asint(c)]);}
        return number(((double*)o->ptr)[asint(key)]);
    }
    if(type(key)==T_SLICE)return slice_item(v,key);
    if(t==T_MAP||t==T_SET){V child=map_find(v,key,UNBOUND);if(child==UNBOUND&&obj(v)->flags==99)return integer(0);if(child==UNBOUND)fail("missing key: %s",cstr(stringify(key,1)));return child;}
    int64_t i=asint(key),n=(int64_t)length(v);if(i<0)i+=n;if(i<0||i>=n)fail("index %lld outside %lld",(long long)i,(long long)n);
    if(t==T_STR){size_t a=bytepos(v,i),b=bytepos(v,i+1);return strn(cstr(v)+a,b-a);}if(t==T_BYTES)return integer((unsigned char)cstr(v)[i]);
    return obj(v)->values[i];
}
void item_set(V v,V key,V child){
    if(type(v)==T_MAP||type(v)==T_SET){map_set(v,key,child);return;}
    if(type(v)==T_ARRAY){Obj*o=obj(v);V row=item(key,integer(0)),col=item(key,integer(1));((double*)o->ptr)[asint(row)*(size_t)asint(o->b)+asint(col)]=asnum(child);return;}
    if(type(key)==T_SLICE){V indices=slice_item(v,key),repl=iter_values(child);(void)indices;Obj*s=obj(key);int64_t n=(int64_t)length(v),lo=s->a==NONE?0:asint(s->a),hi=s->b==NONE?n:asint(s->b);if(lo<0)lo+=n;if(hi<0)hi+=n;if(lo<0)lo=0;if(hi<lo)hi=lo;if(hi>n)hi=n;V freshlist=list_new();for(int64_t i=0;i<lo;++i)append(freshlist,item(v,integer(i)));extend(freshlist,repl);for(int64_t i=hi;i<n;++i)append(freshlist,item(v,integer(i)));obj(v)->values=obj(freshlist)->values;obj(v)->n=obj(freshlist)->n;obj(v)->cap=obj(freshlist)->cap;return;}
    int64_t i=asint(key),n=(int64_t)length(v);if(i<0)i+=n;if(i<0||i>=n)fail("assignment index");obj(v)->values[i]=child;obj(v)->hash=0;
}
void item_delete(V v,V key){if(type(v)==T_MAP||type(v)==T_SET){map_del(v,key);return;}if(type(key)==T_SLICE){item_set(v,key,list_new());return;}int64_t i=asint(key);if(i<0)i+=length(v);Obj*o=obj(v);if(i<0||(size_t)i>=o->n)fail("delete index");memmove(o->values+i,o->values+i+1,(o->n-i-1)*sizeof(V));--o->n;}
static int numeric(V v){return type(v)<0||type(v)==T_FLOAT;}
static int equal(V a,V b){
    if(a==b)return 1;int ta=type(a),tb=type(b);if(numeric(a)&&numeric(b))return asnum(a)==asnum(b);if(ta!=tb && !((ta==T_LIST||ta==T_TUPLE)&&(tb==T_LIST||tb==T_TUPLE)))return 0;
    if(ta==0)return 0;
    if(ta==T_STR||ta==T_BYTES)return obj(a)->n==obj(b)->n&&!memcmp(cstr(a),cstr(b),obj(a)->n);
    if(ta==T_LIST||ta==T_TUPLE){if(length(a)!=length(b))return 0;for(size_t i=0;i<length(a);++i)if(!equal(item(a,integer(i)),item(b,integer(i))))return 0;return 1;}
    if(ta==T_MAP||ta==T_SET){if(length(a)!=length(b))return 0;V keys=map_keys(a);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i)),v=map_find(b,k,UNBOUND);if(v==UNBOUND||!equal(v,map_find(a,k,NONE)))return 0;}return 1;}
    if(ta==T_INSTANCE){Obj*x=obj(a),*y=obj(b);if(x->a!=y->a||x->slots!=y->slots)return 0;for(size_t i=0;i<x->slots;++i)if(!equal(x->values[i],y->values[i]))return 0;if(!x->b&&!y->b)return 1;if(!x->b||!y->b)return 0;return equal(x->b,y->b);}
    return 0;
}
static int ordering(V a,V b){if(equal(a,b))return 0;if(numeric(a)&&numeric(b))return asnum(a)<asnum(b)?-1:1;
    int t=type(a);if(t==T_STR||t==T_BYTES){size_t n=obj(a)->n<obj(b)->n?obj(a)->n:obj(b)->n;int c=memcmp(cstr(a),cstr(b),n);return c?c:(obj(a)->n<obj(b)->n?-1:1);}
    if(t==T_LIST||t==T_TUPLE){size_t n=length(a)<length(b)?length(a):length(b);for(size_t i=0;i<n;++i){int c=ordering(item(a,integer(i)),item(b,integer(i)));if(c)return c;}return length(a)<length(b)?-1:1;}
    fail("unorderable %d %d",t,type(b));return 0;
}
static int contains(V collection,V key){int t=type(collection);if(t==T_SET||t==T_MAP)return map_find(collection,key,UNBOUND)!=UNBOUND;if(t==T_STR){return bytes_find(cstr(collection),obj(collection)->n,cstr(key),obj(key)->n)!=NULL;}if(t==T_ITER){Obj*it=obj(collection);while((size_t)asint(it->b)<length(it->a)){size_t i=(size_t)asint(it->b);it->b=integer(i+1);if(equal(item(it->a,integer(i)),key))return 1;}return 0;}V seq=iter_values(collection);for(size_t i=0;i<length(seq);++i)if(equal(item(seq,integer(i)),key))return 1;return 0;}
V compare(int op,V a,V b){int r=0;
    if(type(a)==T_SET&&type(b)==T_SET&&(op==OP_LT||op==OP_LTE||op==OP_GT||op==OP_GTE)){
        V left=(op==OP_GT||op==OP_GTE)?b:a,right=(op==OP_GT||op==OP_GTE)?a:b;
        int strict=op==OP_LT||op==OP_GT;V keys=map_keys(left);r=strict?length(left)<length(right):length(left)<=length(right);
        for(size_t i=0;r&&i<length(keys);++i)r=contains(right,item(keys,integer(i)));return r?TRUEV:FALSEV;
    }
    switch(op){case OP_EQ:r=equal(a,b);break;case OP_NOTEQ:r=!equal(a,b);break;case OP_IS:r=a==b;break;case OP_ISNOT:r=a!=b;break;case OP_IN:r=contains(b,a);break;case OP_NOTIN:r=!contains(b,a);break;case OP_LT:r=ordering(a,b)<0;break;case OP_LTE:r=ordering(a,b)<=0;break;case OP_GT:r=ordering(a,b)>0;break;case OP_GTE:r=ordering(a,b)>=0;break;}return r?TRUEV:FALSEV;}
V unary(int op,V a){if(op==OP_NOT)return truth(a)?FALSEV:TRUEV;if(op==OP_INVERT)return integer(~asint(a));if(op==OP_UADD)return a;return type(a)==T_FLOAT?number(-asnum(a)):integer(-asint(a));}
static V array_binary(int op,V a,V b);
V binary(int op,V a,V b){
    int ta=type(a),tb=type(b);
    if(ta==T_ARRAY||tb==T_ARRAY)return array_binary(op,a,b);
    if(op==OP_ADD&&(ta==T_STR||ta==T_BYTES)){size_t n=obj(a)->n,m=obj(b)->n;char*s=mem(n+m+1);memcpy(s,cstr(a),n);memcpy(s+n,cstr(b),m);V out=strn(s,n+m);obj(out)->type=ta;return out;}
    if(op==OP_ADD&&(ta==T_LIST||ta==T_TUPLE)){V v=list_new();extend(v,a);extend(v,b);obj(v)->type=ta;return v;}
    if(op==OP_MULT&&(ta==T_STR||ta==T_LIST||ta==T_TUPLE||tb==T_STR||tb==T_LIST||tb==T_TUPLE)){if(numeric(a)){V v=a;a=b;b=v;ta=type(a);}int64_t count=asint(b);if(count<0)count=0;V v=list_new();for(int64_t i=0;i<count;++i)extend(v,a);if(ta==T_STR)return join(S(""),v);obj(v)->type=ta;return v;}
    if((op==OP_BITOR||op==OP_BITAND||op==OP_SUB||op==OP_BITXOR)&&(ta==T_SET||ta==T_MAP)){
        V out=map_new();obj(out)->type=ta;V ka=map_keys(a),kb=map_keys(b);
        for(size_t i=0;i<length(ka);++i){V k=item(ka,integer(i));int in=contains(b,k);if(op==OP_BITOR||(op==OP_BITAND&&in)||((op==OP_SUB||op==OP_BITXOR)&&!in))map_set(out,k,map_find(a,k,NONE));}
        if(op==OP_BITOR||op==OP_BITXOR)for(size_t i=0;i<length(kb);++i){V k=item(kb,integer(i));if(op==OP_BITOR||!contains(a,k))map_set(out,k,map_find(b,k,NONE));}return out;
    }
    if(op==OP_DIV)return number(asnum(a)/asnum(b));if(op==OP_FLOORDIV)return integer((int64_t)floor(asnum(a)/asnum(b)));
    if(op==OP_POW)return number(pow(asnum(a),asnum(b)));
    if(op==OP_BITOR)return integer(asint(a)|asint(b));if(op==OP_BITAND)return integer(asint(a)&asint(b));if(op==OP_BITXOR)return integer(asint(a)^asint(b));if(op==OP_LSHIFT)return integer(asint(a)<<asint(b));if(op==OP_RSHIFT)return integer(asint(a)>>asint(b));
    if(ta==T_FLOAT||tb==T_FLOAT){double x=asnum(a),y=asnum(b);return number(op==OP_ADD?x+y:op==OP_SUB?x-y:op==OP_MULT?x*y:fmod(x,y));}
    int64_t x=asint(a),y=asint(b);return integer(op==OP_ADD?x+y:op==OP_SUB?x-y:op==OP_MULT?x*y:(x%y+y)%y);
}
V inplace(int op,V a,V b){if(op==OP_ADD&&type(a)==T_LIST){extend(a,b);return a;}if(op==OP_BITOR&&(type(a)==T_MAP||type(a)==T_SET)){map_update(a,b);return a;}return binary(op,a,b);}
V join(V sep,V values){V seq=iter_values(values);size_t size=0,n=length(seq);for(size_t i=0;i<n;++i){V v=item(seq,integer(i));if(type(v)!=T_STR)fail("join non-string %d",type(v));size+=obj(v)->n;}if(n)size+=(n-1)*obj(sep)->n;char*s=mem(size+1);size_t p=0;for(size_t i=0;i<n;++i){if(i){memcpy(s+p,cstr(sep),obj(sep)->n);p+=obj(sep)->n;}V v=item(seq,integer(i));memcpy(s+p,cstr(v),obj(v)->n);p+=obj(v)->n;}return strn(s,p);}
static V native(const char *name,V receiver){V v=fresh(T_NATIVE);obj(v)->s=(char*)name;obj(v)->a=receiver;return v;}
V function_new(Code code,Env*closure,V params,V defaults,int positional,const char*vararg,const char*kwarg,const char*name,V slots){V v=fresh(T_FN);Obj*o=obj(v);o->code=code;o->env=closure;o->a=params;o->b=defaults;o->n=positional;o->c=vararg?str(vararg):NONE;o->d=kwarg?str(kwarg):NONE;o->s=(char*)name;o->slots=length(slots);o->values=obj(slots)->values;o->locals_map=map_new();for(size_t i=0;i<o->slots;++i)map_set(o->locals_map,o->values[i],integer(i));if(profile_allocations){unsigned index=(unsigned)(((uintptr_t)code>>4)%2047)+1;while(allocation_stats[index].code&&allocation_stats[index].code!=code)index=index==2047?1:index+1;allocation_stats[index].code=code;allocation_stats[index].name=name;o->profile_index=index;}return v;}
V class_new(V name,Env*parent){V v=fresh(T_CLASS);obj(v)->s=(char*)cstr(name);obj(v)->env=env_new(parent);obj(v)->env->vars=map_new();obj(v)->a=map_new();obj(v)->b=list_new();obj(v)->locals_map=map_new();return v;}
void class_field(V cls,V name,V fallback){map_set(obj(cls)->locals_map,name,integer(length(obj(cls)->b)));append(obj(cls)->b,name);map_set(obj(cls)->a,name,fallback);}
static V bound(V fn,V receiver){V v=fresh(T_BOUND);obj(v)->a=fn;obj(v)->b=receiver;return v;}
V attr(V v,const char *name){
    int t=type(v);V key=symbol(name);
    if(t==T_MODULE){V x=map_find(obj(v)->env->vars,key,UNBOUND);if(x!=UNBOUND)return x;
        if(!strcmp(name,"IGNORECASE")||!strcmp(name,"I"))return integer(2);
        if(!strcmp(name,"MULTILINE"))return integer(8);if(!strcmp(name,"DOTALL"))return integer(16);
        if(!strcmp(name,"ASCII"))return integer(256);if(!strcmp(name,"UNICODE")||!strcmp(name,"V0"))return integer(0);
        if(!strcmp(name,"inf"))return number(INFINITY);if(!strcmp(name,"nan"))return number(NAN);
        return native(name,v);}
    if(t==T_INSTANCE){Obj*o=obj(v);V cls=o->a,slot=map_find(obj(cls)->locals_map,key,UNBOUND);if(slot!=UNBOUND&&o->values[asint(slot)]!=UNBOUND)return o->values[asint(slot)];V x=o->b?map_find(o->b,key,UNBOUND):UNBOUND;if(x!=UNBOUND)return x;x=map_find(obj(cls)->env->vars,key,UNBOUND);if(x!=UNBOUND){if(type(x)==T_FN){if(obj(x)->flags&FN_STATIC)return x;V f=bound(x,obj(x)->flags&FN_CLASS?cls:v);if(obj(x)->flags&FN_PROPERTY)return call(f,list_new(),map_new());return f;}return x;}fail("missing attribute %s.%s",obj(cls)->s,name);}
    if(t==T_CLASS){V x=map_find(obj(v)->env->vars,key,UNBOUND);if(x!=UNBOUND)return type(x)==T_FN&&(obj(x)->flags&FN_CLASS)?bound(x,v):x;}
    if(t==T_REGEX&&strcmp(name,"pattern")==0)return obj(v)->a;
    if(t==T_MATCH&&strcmp(name,"string")==0)return obj(v)->b;
    if(t==T_MAP){const char*methods[]={"get","setdefault","items","keys","values","update","clear","pop","copy","fromkeys",NULL};for(int i=0;methods[i];++i)if(!strcmp(methods[i],name))return native(name,v);if(map_find(v,key,UNBOUND)!=UNBOUND)return map_find(v,key,NONE);}
    return native(name,v);
}
void attr_set(V v,const char *name,V child){if(type(v)==T_INSTANCE){Obj*o=obj(v);V key=symbol(name),slot=map_find(obj(o->a)->locals_map,key,UNBOUND);if(slot!=UNBOUND)o->values[asint(slot)]=child;else{if(!o->b)o->b=map_new();map_set(o->b,key,child);}o->hash=0;}else if(type(v)==T_MODULE||type(v)==T_CLASS)put(obj(v)->env,name,child);else fail("set attribute on %d",type(v));}
static V builtin_call(const char*name,V receiver,V args,V kw);
static void debug_return(Env*e,V result);
V call(V fn,V args,V kw){
    int t=type(fn);if(t==T_NATIVE)return builtin_call(obj(fn)->s,obj(fn)->a,args,kw);
    if(t==T_BOUND){V a=list_new();append(a,obj(fn)->b);extend(a,args);return call(obj(fn)->a,a,kw);}
    if(t==T_CLASS){V v=fresh(T_INSTANCE);obj(v)->a=fn;V fields=obj(fn)->b;obj(v)->slots=length(fields);obj(v)->values=mem((length(fields)+1)*sizeof(V));
        for(size_t i=0;i<length(fields);++i){V key=item(fields,integer(i)),val=map_find(obj(fn)->a,key,UNBOUND);if(i<length(args))val=item(args,integer(i));V named=map_find(kw,key,UNBOUND);if(named!=UNBOUND)val=named;if(type(val)==T_FACTORY)val=call(obj(val)->a,list_new(),map_new());obj(v)->values[i]=val;}
        V keys=map_keys(kw);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i));if(map_find(obj(fn)->locals_map,k,UNBOUND)==UNBOUND)attr_set(v,cstr(k),map_find(kw,k,NONE));}
        V init=map_find(obj(fn)->env->vars,S("__init__"),UNBOUND);if(init!=UNBOUND){V a=list_new();append(a,v);extend(a,args);call(init,a,kw);}return v;
    }
    if(t!=T_FN)fail("not callable: type %d",t);
    Obj*o=obj(fn);V cache_key=NONE,cache=NONE;Obj temporary_key={0};
    if((o->flags&FN_CACHE)&&!memo_disabled){cache=(V)o->ptr;if(!cache){cache=map_new();o->ptr=(void*)cache;}
        if(!length(kw)&&!(o->flags&FN_ATTACH)){temporary_key.type=T_TUPLE;temporary_key.values=obj(args)->values;temporary_key.n=length(args);cache_key=(V)&temporary_key;}
        else{cache_key=list_new();extend(cache_key,args);V keys=map_keys(kw);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i));append(cache_key,k);append(cache_key,map_find(kw,k,NONE));}obj(cache_key)->type=T_TUPLE;}
        if(o->flags&FN_ATTACH)append(cache_key,obj(get(o->env,"_ATTACH_MARKS"))->a);
        V hit=map_find(cache,cache_key,UNBOUND);if(hit!=UNBOUND)return o->flags&FN_CLONE?clone_value(hit):hit;
        if(cache_key==(V)&temporary_key){cache_key=list_new();extend(cache_key,args);obj(cache_key)->type=T_TUPLE;}
    }
    Env *e=env_new(o->env);e->function=o;e->locals=mem((o->slots+1)*sizeof(V));for(size_t i=0;i<o->slots;++i)e->locals[i]=UNBOUND;V names=o->a;size_t n=length(args);
    if(n>o->n&&o->c==NONE)fail("%s received %zu arguments, accepts %zu",o->s,n,o->n);
    for(size_t i=0;i<length(names);++i){V k=item(names,integer(i)),v=map_find(o->b,k,UNBOUND);if(i<n&&i<o->n)v=item(args,integer(i));V x=map_find(kw,k,UNBOUND);if(x!=UNBOUND)v=x;if(v==UNBOUND)fail("%s missing argument %s",o->s,cstr(k));e->locals[i]=v;}
    if(o->c!=NONE)put(e,cstr(o->c),slice_get(args,(int64_t)o->n,(int64_t)n,1));
    if(o->d!=NONE){V rest=map_new(),keys=map_keys(kw);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i));if(!contains(names,k))map_set(rest,k,map_find(kw,k,NONE));}put(e,cstr(o->d),rest);}
    if(call_depth>=1024)fail("recursion limit");call_stack[call_depth++]=o->s;Trap *saved=trap_head;
    unsigned previous_allocation=active_allocation;active_allocation=o->profile_index;if(profile_allocations)++allocation_stats[active_allocation].calls;
    int64_t began=profile_allocations?profile_clock():0;
    V result=o->code(e);if(profile_allocations){int64_t elapsed=profile_clock()-began;allocation_stats[active_allocation].ticks+=elapsed;if(previous_allocation)allocation_stats[previous_allocation].ticks-=elapsed;}if(dump_function&&!strcmp(dump_function,o->s))debug_return(e,result);active_allocation=previous_allocation;trap_head=saved;--call_depth;
    if(cache!=NONE){if(length(cache)>=o->cap){obj(cache)->n=0;obj(cache)->cap=0;obj(cache)->entries=NULL;obj(cache)->a=0;}map_set(cache,cache_key,o->flags&FN_CLONE?clone_value(result):result);}
    return result;
}
V call_pos(V fn,size_t n,V*values){Obj args={0};args.type=T_LIST;args.n=n;args.cap=n;args.values=values;return call(fn,(V)&args,empty_kwargs);}

/* Remaining native library and entry point are below the generated scorer. */
