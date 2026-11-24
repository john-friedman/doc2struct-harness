/* Included by runtime.c: native strings, Unicode, regex, arrays and builtins. */
#include "generated/entities.h"
#include "generated/unicode_digits.h"
static int unicode_digit(uint32_t cp){size_t lo=0,hi=sizeof(digit_points)/sizeof(*digit_points);while(lo<hi){size_t mid=lo+(hi-lo)/2;if(digit_points[mid]==cp)return digit_values[mid];if(digit_points[mid]<cp)lo=mid+1;else hi=mid;}return -1;}
static int hex_digit(char c);
static const char*entity_value(const char*name){size_t lo=0,hi=sizeof(entity_names)/sizeof(*entity_names);while(lo<hi){size_t mid=lo+(hi-lo)/2;int c=strcmp(entity_names[mid],name);if(!c)return entity_values[mid];if(c<0)lo=mid+1;else hi=mid;}return NULL;}
static V html_unescape(V source){
    const char*s=cstr(source);size_t n=obj(source)->n;char*buf=mem(n*4+1);size_t p=0,q=0;
    while(p<n){if(s[p]!='&'){buf[q++]=s[p++];continue;}size_t start=p++;
        if(p<n&&s[p]=='#'){++p;int base=10;if(p<n&&(s[p]=='x'||s[p]=='X')){base=16;++p;}size_t digits=p;uint32_t cp=0;while(p<n){int d=base==16?hex_digit(s[p]):s[p]>='0'&&s[p]<='9'?s[p]-'0':-1;if(d<0)break;cp=cp>0x110000?0x110000:cp*base+d;++p;}if(p==digits){buf[q++]='&';p=start+1;continue;}if(p<n&&s[p]==';')++p;
            if(cp==0||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))cp=0xfffd;
            static const uint32_t cp1252[]={0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};if(cp>=128&&cp<160)cp=cp1252[cp-128];q+=encode(cp,buf+q);continue;
        }
        size_t end=p;while(end<n&&end-p<32&&((s[end]>='A'&&s[end]<='Z')||(s[end]>='a'&&s[end]<='z')||(s[end]>='0'&&s[end]<='9')))++end;if(end<n&&s[end]==';')++end;
        const char*value=NULL;size_t matched=end;
        for(;matched>p;--matched){char key[40];size_t len=matched-p;memcpy(key,s+p,len);key[len]=0;value=entity_value(key);if(value)break;}
        if(value){size_t len=strlen(value);memcpy(buf+q,value,len);q+=len;p=matched;}else{buf[q++]='&';p=start+1;}
    }return strn(buf,q);
}
typedef pcre2_code *(*PCompile)(PCRE2_SPTR,PCRE2_SIZE,uint32_t,int*,PCRE2_SIZE*,pcre2_compile_context*);
typedef pcre2_match_data *(*PData)(const pcre2_code*,pcre2_general_context*);
typedef int (*PMatch)(const pcre2_code*,PCRE2_SPTR,PCRE2_SIZE,PCRE2_SIZE,uint32_t,pcre2_match_data*,pcre2_match_context*);
typedef PCRE2_SIZE *(*PVector)(pcre2_match_data*);
typedef void (*PFree)(pcre2_match_data*);
typedef int (*PInfo)(const pcre2_code*,uint32_t,void*);
typedef int (*PName)(const pcre2_code*,PCRE2_SPTR);
typedef int (*PError)(int,PCRE2_UCHAR*,PCRE2_SIZE);
typedef int (*PJit)(pcre2_code*,uint32_t);
typedef int (*PSub)(const pcre2_code*,PCRE2_SPTR,PCRE2_SIZE,PCRE2_SIZE,uint32_t,pcre2_match_data*,pcre2_match_context*,PCRE2_SPTR,PCRE2_SIZE,PCRE2_UCHAR*,PCRE2_SIZE*);
typedef pcre2_match_context*(*PMContext)(pcre2_general_context*);
typedef pcre2_jit_stack*(*PJStack)(PCRE2_SIZE,PCRE2_SIZE,pcre2_general_context*);
typedef void (*PJAssign)(pcre2_match_context*,pcre2_jit_callback,void*);
static PCompile p_compile;static PData p_data;static PMatch p_match;static PVector p_vector;
static PFree p_free;static PInfo p_info;static PName p_name;static PError p_error;static PJit p_jit;
static PSub p_substitute;
static PMContext p_match_context;static PJStack p_jit_stack;static PJAssign p_jit_assign;
static TLS pcre2_match_context*match_context;
static TLS V regex_cache;
typedef struct RxEntry {struct RxEntry*next;uint64_t hash;size_t n;int flags,line_guard;char*pattern;pcre2_code*code;} RxEntry;
static TLS RxEntry*compiled_regexes[4096];
static int regex_is_loaded;
static void regex_load(void){
#ifdef _WIN32
    HMODULE dll=LoadLibraryA("libpcre2-8-0.dll");
    if(!dll)dll=LoadLibraryA("C:/Program Files/Git/mingw64/bin/libpcre2-8-0.dll");
    if(!dll)dll=LoadLibraryA("C:/Program Files/Tesseract-OCR/libpcre2-8-0.dll");
    if(!dll)fail("PCRE2 native library is missing");
#define SYM(n) GetProcAddress(dll,"pcre2_" n "_8")
#else
    void*dll=dlopen("libpcre2-8.so.0",RTLD_NOW);if(!dll)fail("PCRE2 native library is missing");
#define SYM(n) dlsym(dll,"pcre2_" n "_8")
#endif
    p_compile=(PCompile)SYM("compile");p_data=(PData)SYM("match_data_create_from_pattern");p_match=(PMatch)SYM("match");p_vector=(PVector)SYM("get_ovector_pointer");p_free=(PFree)SYM("match_data_free");p_info=(PInfo)SYM("pattern_info");p_name=(PName)SYM("substring_number_from_name");p_error=(PError)SYM("get_error_message");p_jit=(PJit)SYM("jit_compile");
    p_substitute=(PSub)SYM("substitute");
    p_match_context=(PMContext)SYM("match_context_create");p_jit_stack=(PJStack)SYM("jit_stack_create");p_jit_assign=(PJAssign)SYM("jit_stack_assign");
#undef SYM
}
static V replace_string(V src,V needle,V replacement,int64_t limit){
    const char*s=cstr(src),*n=cstr(needle),*r=cstr(replacement);size_t sn=obj(src)->n,nn=obj(needle)->n,rn=obj(replacement)->n;
    V parts=list_new();size_t pos=0;int64_t count=0;
    if(!nn){append(parts,replacement);V chars=iter_values(src);for(size_t i=0;i<length(chars);++i){append(parts,item(chars,integer(i)));if(limit<0||count++<limit-1)append(parts,replacement);}return join(S(""),parts);}
    while(pos<=sn&&(limit<0||count<limit)){const char *hit=bytes_find(s+pos,sn-pos,n,nn);if(!hit)break;size_t p=(size_t)(hit-s);append(parts,strn(s+pos,p-pos));append(parts,replacement);pos=p+nn;++count;}
    append(parts,strn(s+pos,sn-pos));(void)r;(void)rn;return join(S(""),parts);
}
static V regex_compile(V pattern,int flags){
    if(type(pattern)==T_REGEX)return pattern;
    V key=list_new();append(key,pattern);append(key,integer(flags));obj(key)->type=T_TUPLE;
    V cached=map_find(regex_cache,key,UNBOUND);if(cached!=UNBOUND)return cached;
    V p=replace_string(pattern,S("\\u"),S("\\x{"),0);(void)p;
    /* PCRE2 accepts Python named groups and conditionals. ALT_BSUX enables
       \uXXXX. Python's \Z is strict end-of-input, PCRE2's equivalent is \z. */
    p=replace_string(pattern,S("\\Z"),S("\\z"),-1);
    /* Python re excludes combining marks from word characters. PCRE2 10.43+
       includes them, which would change token counts for accented math labels. */
    if(!(flags&256))p=replace_string(p,S("[^\\W_]"),S("[\\p{L}\\p{N}]"),-1);
    int line_guard=strstr(cstr(p),"(?<!\\A[ \\t]*)")!=NULL;
    if(line_guard)p=replace_string(p,S("(?<!\\A[ \\t]*)"),S(""),-1);
    uint32_t options=PCRE2_UTF|PCRE2_UCP|PCRE2_ALT_BSUX|PCRE2_DUPNAMES;
    if(flags&2)options|=PCRE2_CASELESS;if(flags&8)options|=PCRE2_MULTILINE;if(flags&16)options|=PCRE2_DOTALL;
    if(flags&256)options&=~PCRE2_UCP;
    uint64_t hash=hash_value(pattern)^(uint64_t)flags;size_t bucket=hash&4095;RxEntry*cached_code=compiled_regexes[bucket];
    while(cached_code&&!(cached_code->hash==hash&&cached_code->flags==flags&&cached_code->n==obj(pattern)->n&&!memcmp(cached_code->pattern,cstr(pattern),obj(pattern)->n)))cached_code=cached_code->next;
    int error=0;PCRE2_SIZE offset=0;pcre2_code *code=cached_code?cached_code->code:p_compile((PCRE2_SPTR)cstr(p),obj(p)->n,options,&error,&offset,NULL);
    if(!code){unsigned char message[256];p_error(error,message,sizeof(message));fail("regex compile at %zu: %s: %s",(size_t)offset,message,cstr(p));}
    if(!cached_code){p_jit(code,PCRE2_JIT_COMPLETE);RxEntry*entry=calloc(1,sizeof(RxEntry));if(!entry)fail("regex cache allocation");entry->hash=hash;entry->flags=flags;entry->n=obj(pattern)->n;entry->pattern=malloc(entry->n+1);memcpy(entry->pattern,cstr(pattern),entry->n);entry->pattern[entry->n]=0;entry->code=code;entry->next=compiled_regexes[bucket];compiled_regexes[bucket]=entry;}
    V rx=fresh(T_REGEX);obj(rx)->a=pattern;obj(rx)->ptr=code;obj(rx)->flags=flags|(line_guard?1048576:0);
    map_set(regex_cache,key,rx);return rx;
}
static V regex_match_at(V rx,V source,size_t start,size_t end,int anchored,int full){
    pcre2_match_data *md=p_data((pcre2_code*)obj(rx)->ptr,NULL);uint32_t options=anchored?PCRE2_ANCHORED:0;
retry_match:;
    int count=p_match(obj(rx)->ptr,(PCRE2_SPTR)cstr(source),end,start,options,md,match_context);
    if(count==PCRE2_ERROR_JIT_STACKLIMIT)count=p_match(obj(rx)->ptr,(PCRE2_SPTR)cstr(source),end,start,options|PCRE2_NO_JIT,md,match_context);
    if(count<0){p_free(md);if(count!=PCRE2_ERROR_NOMATCH)fail("regex match failed (%d)",count);return NONE;}
    PCRE2_SIZE *vector=p_vector(md);if(full&&vector[1]!=end){p_free(md);return NONE;}
    if(obj(rx)->flags&1048576){size_t pos=(size_t)vector[0],q=pos;uint32_t cp=decode(cstr(source),end,&q);int guarded=cp=='='||cp=='<'||cp=='>'||cp==0x2039||cp==0xd7;
        if(guarded){int blank=1;for(size_t z=0;z<pos;++z)if(cstr(source)[z]!=' '&&cstr(source)[z]!='\t'){blank=0;break;}if(blank){if(anchored||q>=end){p_free(md);return NONE;}start=q;goto retry_match;}}
    }
    V match=fresh(T_MATCH);Obj*m=obj(match);m->a=rx;m->b=source;m->n=(size_t)count;m->values=mem(2*(size_t)count*sizeof(V));
    for(int i=0;i<2*count;++i)m->values[i]=vector[i]==PCRE2_UNSET?integer(-1):integer(vector[i]);p_free(md);return match;
}
static int group_number(V match,V key){if(type(key)==T_STR){int g=p_name(obj(obj(match)->a)->ptr,(PCRE2_SPTR)cstr(key));if(g<0)fail("no regex group %s",cstr(key));return g;}return(int)asint(key);}
static V match_group(V match,V key){Obj*m=obj(match);int n=group_number(match,key);if(n<0)fail("negative group");if((size_t)n>=m->n)return NONE;int64_t a=asint(m->values[2*n]),b=asint(m->values[2*n+1]);if(a<0)return NONE;return strn(cstr(m->b)+a,(size_t)(b-a));}
static V expand_replacement(V replacement,V match){
    const char*s=cstr(replacement);size_t n=obj(replacement)->n;V parts=list_new();size_t start=0,p=0;
    while(p<n){if(s[p]!='\\'){++p;continue;}append(parts,strn(s+start,p-start));++p;
        if(p<n&&s[p]>='0'&&s[p]<='9'){int g=s[p++]-'0';if(p<n&&s[p]>='0'&&s[p]<='9')g=10*g+s[p++]-'0';V v=match_group(match,integer(g));append(parts,v==NONE?S(""):v);}
        else if(p+1<n&&s[p]=='g'&&s[p+1]=='<'){p+=2;size_t a=p;while(p<n&&s[p]!='>')++p;V k=strn(s+a,p-a);if(p<n)++p;V v=match_group(match,k);append(parts,v==NONE?S(""):v);}
        else if(p<n){char c=s[p++];char r=c=='n'?'\n':c=='t'?'\t':c=='r'?'\r':c;append(parts,strn(&r,1));}start=p;
    }
    append(parts,strn(s+start,n-start));return join(S(""),parts);
}
static V regex_operation(const char *name,V rx,V args,V kw){
    V source=item(args,integer(!strcmp(name,"sub")?1:0));size_t start=0,end=obj(source)->n;
    int positioned=strcmp(name,"sub")&&strcmp(name,"split");
    if(positioned&&length(args)>1)start=bytepos(source,asint(item(args,integer(1))));
    if(positioned&&length(args)>2)end=bytepos(source,asint(item(args,integer(2))));
    if(!strcmp(name,"search")||!strcmp(name,"match")||!strcmp(name,"fullmatch"))return regex_match_at(rx,source,start,end,strcmp(name,"search")!=0,!strcmp(name,"fullmatch"));
    V out=list_new();size_t cursor=start,last=start;int64_t count=0,limit=0;
    if(!strcmp(name,"sub")){limit=asint(map_find(kw,S("count"),integer(0)));if(length(args)>2)limit=asint(item(args,integer(2)));}
    if(!strcmp(name,"sub")&&!limit&&type(item(args,integer(0)))==T_STR){
        V replacement=item(args,integer(0));const char*s=cstr(replacement);size_t size=obj(replacement)->n;
        char*repl=mem(size*4+8);size_t used=0;
        for(size_t p=0;p<size;++p){char c=s[p];if(c=='$'){repl[used++]='$';repl[used++]='$';}
            else if(c=='\\'&&p+1<size){c=s[++p];if(c>='0'&&c<='9'){repl[used++]='$';repl[used++]='{';repl[used++]=c;if(p+1<size&&s[p+1]>='0'&&s[p+1]<='9')repl[used++]=s[++p];repl[used++]='}';}
                else if(c=='g'&&p+1<size&&s[p+1]=='<'){p+=2;repl[used++]='$';repl[used++]='{';while(p<size&&s[p]!='>')repl[used++]=s[p++];repl[used++]='}';}
                else repl[used++]=c=='n'?'\n':c=='t'?'\t':c=='r'?'\r':c;
            }else repl[used++]=c;
        }
        size_t capacity=obj(source)->n*2+used*16+64;unsigned char*buffer=malloc(capacity);if(!buffer)fail("regex replacement allocation");PCRE2_SIZE written=capacity;
        uint32_t options=PCRE2_SUBSTITUTE_GLOBAL|PCRE2_SUBSTITUTE_OVERFLOW_LENGTH|PCRE2_SUBSTITUTE_UNSET_EMPTY;
        int status=p_substitute(obj(rx)->ptr,(PCRE2_SPTR)cstr(source),obj(source)->n,0,options,NULL,match_context,(PCRE2_SPTR)repl,used,buffer,&written);
        if(status==PCRE2_ERROR_NOMEMORY){capacity=(size_t)written+1;free(buffer);buffer=malloc(capacity);if(!buffer)fail("regex replacement allocation");written=capacity;status=p_substitute(obj(rx)->ptr,(PCRE2_SPTR)cstr(source),obj(source)->n,0,options,NULL,match_context,(PCRE2_SPTR)repl,used,buffer,&written);}
        if(status==PCRE2_ERROR_JIT_STACKLIMIT){written=capacity;status=p_substitute(obj(rx)->ptr,(PCRE2_SPTR)cstr(source),obj(source)->n,0,options|PCRE2_NO_JIT,NULL,match_context,(PCRE2_SPTR)repl,used,buffer,&written);}
        if(status<0){unsigned char message[256];p_error(status,message,sizeof(message));free(buffer);fail("regex substitute: %s",message);}
        V result=strn((char*)buffer,(size_t)written);free(buffer);return result;
    }
    if(!strcmp(name,"split")){limit=asint(map_find(kw,S("maxsplit"),integer(0)));if(length(args)>1)limit=asint(item(args,integer(1)));}
    while(cursor<=end && (!limit||count<limit)){
        V match=regex_match_at(rx,source,cursor,end,0,0);if(match==NONE)break;
        size_t a=(size_t)asint(obj(match)->values[0]),b=(size_t)asint(obj(match)->values[1]);
        if(!strcmp(name,"finditer"))append(out,match);
        else if(!strcmp(name,"findall")){
            uint32_t groups=0;p_info(obj(rx)->ptr,PCRE2_INFO_CAPTURECOUNT,&groups);
            if(!groups)append(out,match_group(match,integer(0)));
            else if(groups==1){V v=match_group(match,integer(1));append(out,v==NONE?S(""):v);}
            else{V values=list_new();for(uint32_t g=1;g<=groups;++g){V v=match_group(match,integer(g));append(values,v==NONE?S(""):v);}obj(values)->type=T_TUPLE;append(out,values);}
        }else if(!strcmp(name,"sub")){
            append(out,strn(cstr(source)+last,a-last));V repl=item(args,integer(0));
            if(type(repl)==T_STR)append(out,expand_replacement(repl,match));else{V aa=list_new();append(aa,match);append(out,call(repl,aa,map_new()));}last=b;
        }else if(!strcmp(name,"split")){
            append(out,strn(cstr(source)+last,a-last));uint32_t groups=0;p_info(obj(rx)->ptr,PCRE2_INFO_CAPTURECOUNT,&groups);for(uint32_t g=1;g<=groups;++g)append(out,match_group(match,integer(g)));last=b;
        }else fail("regex operation %s",name);
        ++count;cursor=b;if(a==b){if(cursor==end){++cursor;break;}decode(cstr(source),end,&cursor);}
    }
    if(!strcmp(name,"sub")||!strcmp(name,"split"))append(out,strn(cstr(source)+last,end-last));
    return !strcmp(name,"sub")?join(S(""),out):out;
}
static int is_space(uint32_t c){return c==32||(c>=9&&c<=13)||(c>=28&&c<=31)||c==0x85||c==0xa0||c==0x1680||(c>=0x2000&&c<=0x200a)||c==0x2028||c==0x2029||c==0x202f||c==0x205f||c==0x3000;}
static int in_chars(uint32_t cp,V chars){if(chars==NONE)return is_space(cp);size_t p=0;while(p<obj(chars)->n)if(decode(cstr(chars),obj(chars)->n,&p)==cp)return 1;return 0;}
static V strip_string(V source,V chars,int left,int right){
    size_t a=0,b=obj(source)->n;
    if(left)while(a<b){size_t p=a;uint32_t c=decode(cstr(source),b,&p);if(!in_chars(c,chars))break;a=p;}
    if(right)while(b>a){size_t p=b-1;while(p>a&&((unsigned char)cstr(source)[p]&192)==128)--p;size_t q=p;uint32_t c=decode(cstr(source),b,&q);if(!in_chars(c,chars))break;b=p;}
    if(!a&&b==obj(source)->n)return source;
    return strn(cstr(source)+a,b-a);
}
static V unicode_normalize(V mode,V source){
    const char*m=cstr(mode);utf8proc_option_t opt=UTF8PROC_STABLE;
    opt|=strchr(m,'D')?UTF8PROC_DECOMPOSE:UTF8PROC_COMPOSE;if(strchr(m,'K'))opt|=UTF8PROC_COMPAT;
    utf8proc_uint8_t *result=NULL;utf8proc_ssize_t n=utf8proc_map((const utf8proc_uint8_t*)cstr(source),(utf8proc_ssize_t)obj(source)->n,&result,opt);
    if(n<0)fail("Unicode normalization: %s",utf8proc_errmsg(n));V out=strn((char*)result,(size_t)n);free(result);return out;
}
static V lower_string(V source,int fold,int upper){
    if(fold){utf8proc_uint8_t *result=NULL;utf8proc_ssize_t n=utf8proc_map((const utf8proc_uint8_t*)cstr(source),obj(source)->n,&result,UTF8PROC_STABLE|UTF8PROC_CASEFOLD);if(n<0)fail("casefold");V out=strn((char*)result,n);free(result);return out;}
    char*buf=mem(obj(source)->n*3+8);size_t p=0,q=0;
    while(p<obj(source)->n){uint32_t cp=decode(cstr(source),obj(source)->n,&p);if(!upper&&cp==0x130){q+=encode('i',buf+q);q+=encode(0x307,buf+q);continue;}cp=upper?utf8proc_toupper(cp):utf8proc_tolower(cp);q+=encode(cp,buf+q);}return strn(buf,q);
}
static V split_string(V source,V separator,int64_t maxsplit,int reverse){
    V out=list_new();const char*s=cstr(source);size_t n=obj(source)->n,p=0,last=0;int64_t count=0;
    if(separator==NONE){
        while(p<n){size_t a=p;uint32_t cp=decode(s,n,&p);if(!is_space(cp)){p=a;break;}}
        while(p<n){size_t a=p;while(p<n){size_t z=p;uint32_t cp=decode(s,n,&p);if(is_space(cp)){p=z;break;}}
            if(maxsplit>=0&&count>=maxsplit){append(out,strn(s+a,n-a));return out;}append(out,strn(s+a,p-a));++count;
            while(p<n){size_t z=p;uint32_t cp=decode(s,n,&p);if(!is_space(cp)){p=z;break;}}
        }return out;
    }
    size_t sn=obj(separator)->n;if(!sn)fail("empty separator");
    if(reverse&&maxsplit>=0){V all=split_string(source,separator,-1,0);size_t parts=length(all);if(parts<=(size_t)maxsplit+1)return all;size_t prefix=parts-(size_t)maxsplit;append(out,join(separator,slice_get(all,0,prefix,1)));extend(out,slice_get(all,prefix,parts,1));return out;}
    while(p<=n&&(maxsplit<0||count<maxsplit)){const char*found=bytes_find(s+p,n-p,cstr(separator),sn);if(!found)break;size_t a=found-s;append(out,strn(s+last,a-last));p=a+sn;last=p;++count;}
    append(out,strn(s+last,n-last));return out;
}
static V argument(V args,V kw,size_t i,const char*name,V fallback){return length(args)>i?item(args,integer(i)):map_find(kw,str(name),fallback);}
static V sorted_values(V values,V key,int reverse){
    V out=list_new();extend(out,values);size_t n=length(out);V *keys=mem((n+1)*sizeof(V));
    for(size_t i=0;i<n;++i){V v=item(out,integer(i));if(key!=NONE){V a=list_new();append(a,v);v=call(key,a,map_new());}keys[i]=v;}
    V *temp=mem((n+1)*sizeof(V)),*kt=mem((n+1)*sizeof(V));
    for(size_t width=1;width<n;width*=2){for(size_t lo=0;lo<n;lo+=width*2){size_t mid=lo+width<n?lo+width:n,hi=lo+2*width<n?lo+2*width:n,i=lo,j=mid;
            for(size_t k=lo;k<hi;++k){int left=i<mid&&(j>=hi||(reverse?ordering(keys[i],keys[j])>=0:ordering(keys[i],keys[j])<=0));size_t z=left?i++:j++;temp[k]=obj(out)->values[z];kt[k]=keys[z];}}
        memcpy(obj(out)->values,temp,n*sizeof(V));memcpy(keys,kt,n*sizeof(V));if(width>n/2)break;
    }return out;
}
V stringify(V v,int repr){
    int t=type(v);if(t==T_STR&&!repr)return v;if(t==0)return S("None");if(t==-2)return str(truth(v)?"True":"False");char buf[128];
    if(t==-1){snprintf(buf,sizeof(buf),"%lld",(long long)asint(v));return str(buf);}if(t==T_FLOAT){
        double value=asnum(v);if(!isfinite(value)){snprintf(buf,sizeof(buf),"%g",value);return str(buf);}if(value==0)return signbit(value)?S("-0.0"):S("0.0");
        int precision=1;for(;precision<17;++precision){snprintf(buf,sizeof(buf),"%.*g",precision,value);double back=strtod(buf,NULL);uint64_t a,b;memcpy(&a,&value,8);memcpy(&b,&back,8);if(a==b)break;}
        int exponent=(int)floor(log10(fabs(value)));char scientific[64];snprintf(scientific,sizeof(scientific),"%.*e",precision-1,value);char*e=strchr(scientific,'e');if(e)exponent=atoi(e+1);
        if(exponent>=-4&&exponent<16){int decimals=precision-1-exponent;if(decimals<0)decimals=0;snprintf(buf,sizeof(buf),"%.*f",decimals,value);if(!strchr(buf,'.'))strcat(buf,".0");}
        else snprintf(buf,sizeof(buf),"%s",scientific);return str(buf);
    }
    if(t==T_STR||t==T_BYTES){V parts=list_new();append(parts,S("'"));append(parts,replace_string(v,S("'"),S("\\'"),-1));append(parts,S("'"));return join(S(""),parts);}
    if(t==T_LIST||t==T_TUPLE||t==T_SET){V parts=list_new(),values=iter_values(v);for(size_t i=0;i<length(values);++i)append(parts,stringify(item(values,integer(i)),1));V joined=join(S(", "),parts);return binary(OP_ADD,binary(OP_ADD,str(t==T_LIST?"[":t==T_TUPLE?"(":"{"),joined),str(t==T_LIST?"]":t==T_TUPLE?")":"}"));}
    if(t==T_MAP){V parts=list_new(),keys=map_keys(v);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i));append(parts,binary(OP_ADD,binary(OP_ADD,stringify(k,1),S(": ")),stringify(map_find(v,k,NONE),1)));}return binary(OP_ADD,binary(OP_ADD,S("{"),join(S(", "),parts)),S("}"));}
    return S("<native object>");
}
V format_value(V v,V format,int conversion){char buf[256];const char*f=cstr(format);if(!*f)return stringify(v,conversion==114);if(!strcmp(f,".0%"))snprintf(buf,sizeof(buf),"%.0f%%",asnum(v)*100);else if(!strcmp(f,".2f"))snprintf(buf,sizeof(buf),"%.2f",asnum(v));else if(!strcmp(f,"02d"))snprintf(buf,sizeof(buf),"%02lld",(long long)asint(v));else fail("format %s",f);return str(buf);}
static V edit_distance(V a,V b,int lcs,int transpose){
    V av=iter_values(a),bv=iter_values(b);size_t n=length(av),m=length(bv);uint32_t *prev=mem((m+1)*4),*cur=mem((m+1)*4),*prev2=mem((m+1)*4);
    for(size_t j=0;j<=m;++j)prev[j]=lcs?0:(uint32_t)j;
    for(size_t i=1;i<=n;++i){cur[0]=lcs?0:(uint32_t)i;for(size_t j=1;j<=m;++j){int eq=equal(item(av,integer(i-1)),item(bv,integer(j-1)));if(lcs)cur[j]=eq?prev[j-1]+1:(prev[j]>cur[j-1]?prev[j]:cur[j-1]);else{uint32_t x=prev[j]+1,y=cur[j-1]+1,z=prev[j-1]+!eq;cur[j]=x<y?(x<z?x:z):(y<z?y:z);if(transpose&&i>1&&j>1&&equal(item(av,integer(i-1)),item(bv,integer(j-2)))&&equal(item(av,integer(i-2)),item(bv,integer(j-1)))&&prev2[j-2]+1<cur[j])cur[j]=prev2[j-2]+1;}}uint32_t*tmp=prev2;prev2=prev;prev=cur;cur=tmp;}
    return integer(prev[m]);
}
static V array_new(size_t rows,size_t cols,int ndim){V v=fresh(T_ARRAY);Obj*o=obj(v);o->a=integer(rows);o->b=integer(cols);o->flags=ndim;o->n=rows*cols;o->ptr=mem((o->n+1)*sizeof(double));return v;}
static V array_binary(int op,V a,V b){
    size_t ar=type(a)==T_ARRAY?(size_t)asint(obj(a)->a):1,ac=type(a)==T_ARRAY?(size_t)asint(obj(a)->b):1,br=type(b)==T_ARRAY?(size_t)asint(obj(b)->a):1,bc=type(b)==T_ARRAY?(size_t)asint(obj(b)->b):1;
    size_t rows=ar>br?ar:br,cols=ac>bc?ac:bc;V out=array_new(rows,cols,2);
    for(size_t r=0;r<rows;++r)for(size_t c=0;c<cols;++c){double x=type(a)==T_ARRAY?((double*)obj(a)->ptr)[(ar==1?0:r)*ac+(ac==1?0:c)]:asnum(a),y=type(b)==T_ARRAY?((double*)obj(b)->ptr)[(br==1?0:r)*bc+(bc==1?0:c)]:asnum(b);((double*)obj(out)->ptr)[r*cols+c]=op==OP_ADD?x+y:op==OP_SUB?x-y:op==OP_MULT?x*y:op==OP_DIV?x/y:op==OP_POW?pow(x,y):0;}return out;
}
static V assignment(V matrix){
    Obj*o=obj(matrix);size_t orig_n=asint(o->a),orig_m=asint(o->b),trans=orig_n>orig_m,n=trans?orig_m:orig_n,m=trans?orig_n:orig_m;
    double *u=mem((n+1)*sizeof(double)),*v=mem((m+1)*sizeof(double)),*minv=mem((m+1)*sizeof(double));size_t *p=mem((m+1)*sizeof(size_t)),*way=mem((m+1)*sizeof(size_t));unsigned char*used=mem(m+1);
    for(size_t i=1;i<=n;++i){p[0]=i;size_t j0=0;for(size_t j=0;j<=m;++j){minv[j]=INFINITY;used[j]=0;}
        do{used[j0]=1;size_t i0=p[j0],j1=0;double delta=INFINITY;for(size_t j=1;j<=m;++j)if(!used[j]){double a=((double*)o->ptr)[trans?(j-1)*orig_m+i0-1:(i0-1)*orig_m+j-1];double cur=a-u[i0]-v[j];if(cur<minv[j]){minv[j]=cur;way[j]=j0;}if(minv[j]<delta){delta=minv[j];j1=j;}}for(size_t j=0;j<=m;++j)if(used[j]){u[p[j]]+=delta;v[j]-=delta;}else minv[j]-=delta;j0=j1;}while(p[j0]);
        do{size_t j1=way[j0];p[j0]=p[j1];j0=j1;}while(j0);
    }
    V pairs=list_new();for(size_t j=1;j<=m;++j)if(p[j]){V pair=list_new();append(pair,integer(trans?j-1:p[j]-1));append(pair,integer(trans?p[j]-1:j-1));obj(pair)->type=T_TUPLE;append(pairs,pair);}pairs=sorted_values(pairs,NONE,0);
    V rows=list_new(),cols=list_new(),result=list_new();for(size_t i=0;i<length(pairs);++i){V pair=item(pairs,integer(i));append(rows,item(pair,integer(0)));append(cols,item(pair,integer(1)));}append(result,rows);append(result,cols);obj(result)->type=T_TUPLE;return result;
}
#include "forms_tokens.c"
#include "form_matching.c"
#include "format_tokens.c"
#include "table_scan.c"
#include "table_matching.c"
static V builtin_call(const char*name,V receiver,V args,V kw){
    size_t argc=length(args);V a=argc?item(args,integer(0)):NONE,b=argc>1?item(args,integer(1)):NONE,c=argc>2?item(args,integer(2)):NONE;
    int rt=type(receiver);const char*parent=rt==T_MODULE?obj(receiver)->s:(rt==T_NATIVE?obj(receiver)->s:"");
    if(!strncmp(name,"__c_",4)){
        if(!strcmp(name,"__c_norm")){V cache=attr(receiver,"_NORM"),hit=map_find(cache,a,UNBOUND);return hit==UNBOUND?call(attr(receiver,"__original_norm"),args,kw):hit;}
        if(!strcmp(name,"__c_trim"))return strip_string(a,attr(receiver,"_JS_TRIM"),1,1);
        if(!strcmp(name,"__c_keysim")){if(equal(a,b))return constant_float(1.0);V values[]={a,b,obj(attr(receiver,"_SPACELESS_KEYS"))->a};return call_pos(attr(receiver,"_key_sim"),3,values);}
        if(!strcmp(name,"__c_form_tokens"))return forms_tokenize(receiver,a);
        if(!strcmp(name,"__c_form_digits"))return forms_digits(a);
        if(!strcmp(name,"__c_form_weight")){size_t p=0;uint32_t cp=decode(cstr(a),obj(a)->n,&p);return unicode_len(a)<=2&&!truth(forms_digits(a))&&!form_cjk(cp)?attr(receiver,"WEAK_WEIGHT"):constant_float(1.0);}
        if(!strcmp(name,"__c_candidates"))return form_candidates(receiver,a,b,c,argument(args,kw,3,"index",NONE),truth(argument(args,kw,4,"at_start",FALSEV)));
        if(!strcmp(name,"__c_copies"))return form_copies(receiver,a,b,c,item(args,integer(3)),item(args,integer(4)));
        if(!strcmp(name,"__c_window_bound"))return integer(form_index(a)->bound[asint(b)]);
        if(!strcmp(name,"__c_lev"))return edit_distance(a,b,0,0);
        if(!strcmp(name,"__c_matches"))return table_matches(receiver,a,b,c,item(args,integer(3)),truth(item(args,integer(4))),item(args,integer(5)));
        if(!strcmp(name,"__c_table_spans"))return table_spans_native(a);
        if(!strcmp(name,"__c_table_kv"))return table_kv_native(receiver,a);
        if(!strcmp(name,"__c_table_pct"))return table_pct_native(a);
        if(!strcmp(name,"__c_table_fit"))return table_fit_native(receiver,a,b,c);
        if(!strcmp(name,"__c_format_tokens"))return format_tokens(receiver,a,b);
        if(!strcmp(name,"__c_value_sim"))return table_value_native(receiver,args,kw);
        if(!strcmp(name,"__c_pair_rows"))return table_pair_rows(receiver,a,b,c);
        return kernel_dispatch(name,receiver,args,kw);
    }
    if(rt==T_REGEX)return regex_operation(name,receiver,args,kw);
    if(rt==T_MATCH){
        if(!strcmp(name,"group")){if(argc<=1)return match_group(receiver,a==NONE?integer(0):a);V out=list_new();for(size_t i=0;i<argc;++i)append(out,match_group(receiver,item(args,integer(i))));obj(out)->type=T_TUPLE;return out;}
        if(!strcmp(name,"groups")){uint32_t groups=0;p_info(obj(obj(receiver)->a)->ptr,PCRE2_INFO_CAPTURECOUNT,&groups);V out=list_new();for(uint32_t i=1;i<=groups;++i){V v=match_group(receiver,integer(i));append(out,v==NONE?a:v);}obj(out)->type=T_TUPLE;return out;}
        if(!strcmp(name,"start")||!strcmp(name,"end")||!strcmp(name,"span")){int g=group_number(receiver,a==NONE?integer(0):a);int64_t lo=(size_t)g<obj(receiver)->n?asint(obj(receiver)->values[2*g]):-1,hi=(size_t)g<obj(receiver)->n?asint(obj(receiver)->values[2*g+1]):-1;V l=integer(lo<0?-1:(int64_t)cppos(obj(receiver)->b,lo)),h=integer(hi<0?-1:(int64_t)cppos(obj(receiver)->b,hi));if(!strcmp(name,"start"))return l;if(!strcmp(name,"end"))return h;V out=list_new();append(out,l);append(out,h);obj(out)->type=T_TUPLE;return out;}
    }
    if(rt==T_STR||rt==T_BYTES){
        if(!strcmp(name,"join"))return join(receiver,a);
        if(!strcmp(name,"strip")||!strcmp(name,"lstrip")||!strcmp(name,"rstrip"))return strip_string(receiver,a,strcmp(name,"rstrip")!=0,strcmp(name,"lstrip")!=0);
        if(!strcmp(name,"lower")||!strcmp(name,"casefold")||!strcmp(name,"upper"))return lower_string(receiver,!strcmp(name,"casefold"),!strcmp(name,"upper"));
        if(!strcmp(name,"split")||!strcmp(name,"rsplit"))return split_string(receiver,a,b==NONE?-1:asint(b),!strcmp(name,"rsplit"));
        if(!strcmp(name,"replace"))return replace_string(receiver,a,b,c==NONE?-1:asint(c));
        if(!strcmp(name,"startswith")||!strcmp(name,"endswith")){int suffix=!strcmp(name,"endswith");V values=type(a)==T_TUPLE?a:list_new();if(type(a)!=T_TUPLE)append(values,a);for(size_t i=0;i<length(values);++i){V s=item(values,integer(i));if(obj(s)->n<=obj(receiver)->n&&!memcmp(cstr(receiver)+(suffix?obj(receiver)->n-obj(s)->n:0),cstr(s),obj(s)->n))return TRUEV;}return FALSEV;}
        if(!strcmp(name,"removeprefix")||!strcmp(name,"removesuffix")){int suffix=!strcmp(name,"removesuffix");size_t n=obj(a)->n,m=obj(receiver)->n;if(n<=m&&!memcmp(cstr(receiver)+(suffix?m-n:0),cstr(a),n))return strn(cstr(receiver)+(suffix?0:n),m-n);return receiver;}
        if(!strcmp(name,"find")||!strcmp(name,"rfind")||!strcmp(name,"index")){size_t start=b==NONE?0:bytepos(receiver,asint(b));const char*found=strstr(cstr(receiver)+start,cstr(a));if(!strcmp(name,"rfind")){const char*next=found;while(next){found=next;next=strstr(next+1,cstr(a));}}if(!found){if(!strcmp(name,"index"))fail("substring not found");return integer(-1);}return integer(cppos(receiver,found-cstr(receiver)));}
        if(!strcmp(name,"count")){int64_t count=0;const char*p=cstr(receiver);while((p=strstr(p,cstr(a)))!=NULL){++count;p+=obj(a)->n?obj(a)->n:1;if(p>cstr(receiver)+obj(receiver)->n)break;}return integer(count);}
        if(!strcmp(name,"translate")){V parts=list_new();size_t p=0;while(p<obj(receiver)->n){size_t lo=p;uint32_t cp=decode(cstr(receiver),obj(receiver)->n,&p);V repl=map_find(a,integer(cp),UNBOUND);if(repl==UNBOUND)append(parts,strn(cstr(receiver)+lo,p-lo));else if(repl!=NONE)append(parts,numeric(repl)?chr_value((uint32_t)asint(repl)):repl);}return join(S(""),parts);}
        if(!strcmp(name,"isdigit")||!strcmp(name,"isalpha")||!strcmp(name,"isalnum")||!strcmp(name,"isspace")||!strcmp(name,"isupper")){size_t p=0;int ok=obj(receiver)->n!=0,cased=0;while(p<obj(receiver)->n){uint32_t cp=decode(cstr(receiver),obj(receiver)->n,&p);utf8proc_category_t cat=utf8proc_category(cp);int letter=cat>=UTF8PROC_CATEGORY_LU&&cat<=UTF8PROC_CATEGORY_LO,num=cat>=UTF8PROC_CATEGORY_ND&&cat<=UTF8PROC_CATEGORY_NO;int here=!strcmp(name,"isdigit")?(cat==UTF8PROC_CATEGORY_ND||cp==0xb2||cp==0xb3||cp==0xb9):!strcmp(name,"isalpha")?letter:!strcmp(name,"isalnum")?letter||num:!strcmp(name,"isspace")?is_space(cp):cat!=UTF8PROC_CATEGORY_LL;if(cat==UTF8PROC_CATEGORY_LU)cased=1;if(!here){ok=0;break;}}if(!strcmp(name,"isupper"))ok&=cased;return ok?TRUEV:FALSEV;}
        if(!strcmp(name,"encode")){int be=strstr(cstr(a),"be")!=NULL;char*buf=mem(unicode_len(receiver)*4+4);size_t p=0,q=0;while(p<obj(receiver)->n){uint32_t cp=decode(cstr(receiver),obj(receiver)->n,&p);if(cp>65535){cp-=65536;uint32_t hi=0xd800+(cp>>10),lo=0xdc00+(cp&1023);buf[q++]=(char)(be?hi>>8:hi);buf[q++]=(char)(be?hi:hi>>8);cp=lo;}buf[q++]=(char)(be?cp>>8:cp);buf[q++]=(char)(be?cp:cp>>8);}V out=strn(buf,q);obj(out)->type=T_BYTES;return out;}
        if(!strcmp(name,"format")){V out=receiver;for(size_t i=0;i<argc;++i){char pattern[24];snprintf(pattern,sizeof(pattern),"{%zu}",i);out=replace_string(out,str(pattern),stringify(item(args,integer(i)),0),-1);out=replace_string(out,S("{}"),stringify(item(args,integer(i)),0),1);}return out;}
    }
    if(rt==T_LIST||rt==T_TUPLE){
        if(!strcmp(name,"append")){append(receiver,a);return NONE;}if(!strcmp(name,"extend")){extend(receiver,a);return NONE;}
        if(!strcmp(name,"clear")){obj(receiver)->n=0;return NONE;}
        if(!strcmp(name,"pop")){int64_t i=a==NONE?(int64_t)length(receiver)-1:asint(a);V v=item(receiver,integer(i));item_delete(receiver,integer(i));return v;}
        if(!strcmp(name,"sort")){V sorted=sorted_values(receiver,map_find(kw,S("key"),NONE),truth(map_find(kw,S("reverse"),FALSEV)));obj(receiver)->values=obj(sorted)->values;return NONE;}
        if(!strcmp(name,"count")||!strcmp(name,"index")){int64_t count=0;for(size_t i=0;i<length(receiver);++i)if(equal(item(receiver,integer(i)),a)){if(!strcmp(name,"index"))return integer(i);++count;}if(!strcmp(name,"index"))fail("item not in list");return integer(count);}
        if(!strcmp(name,"insert")){int64_t i=asint(a);if(i<0)i+=length(receiver);if(i<0)i=0;if((size_t)i>length(receiver))i=length(receiver);append(receiver,b);Obj*o=obj(receiver);memmove(o->values+i+1,o->values+i,(o->n-i-1)*sizeof(V));o->values[i]=b;return NONE;}
    }
    if(rt==T_MAP||rt==T_SET){
        if(!strcmp(name,"get"))return map_find(receiver,a,b);
        if(!strcmp(name,"setdefault")){V x=map_find(receiver,a,UNBOUND);if(x==UNBOUND){x=b;map_set(receiver,a,b);}return x;}
        if(!strcmp(name,"keys"))return map_keys(receiver);
        if(!strcmp(name,"items")||!strcmp(name,"values")){V keys=map_keys(receiver),out=list_new();for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i)),val=map_find(receiver,k,NONE);if(!strcmp(name,"items")){V pair=list_new();append(pair,k);append(pair,val);obj(pair)->type=T_TUPLE;val=pair;}append(out,val);}return out;}
        if(!strcmp(name,"clear")){obj(receiver)->n=0;obj(receiver)->cap=0;obj(receiver)->entries=NULL;obj(receiver)->a=0;return NONE;}
        if(!strcmp(name,"add")){map_set(receiver,a,TRUEV);return NONE;}
        if(!strcmp(name,"discard")){map_del(receiver,a);return NONE;}
        if(!strcmp(name,"pop")){if(rt==T_SET){V keys=map_keys(receiver);if(!length(keys))fail("pop from empty set");V key=item(keys,integer(0));map_del(receiver,key);return key;}V v=map_find(receiver,a,b);map_del(receiver,a);return v;}
        if(!strcmp(name,"update")){if(rt==T_SET){for(size_t j=0;j<argc;++j){V seq=iter_values(item(args,integer(j)));for(size_t i=0;i<length(seq);++i)map_set(receiver,item(seq,integer(i)),TRUEV);}}else{if(argc)map_update(receiver,a);map_update(receiver,kw);}return NONE;}
        if(!strcmp(name,"intersection")||!strcmp(name,"union")){V out=set_from(receiver);for(size_t i=0;i<argc;++i){V other=item(args,integer(i));out=binary(!strcmp(name,"union")?OP_BITOR:OP_BITAND,out,type(other)==T_SET?other:set_from(other));}return out;}
        if(!strcmp(name,"difference_update")){V seq=iter_values(a);for(size_t i=0;i<length(seq);++i)map_del(receiver,item(seq,integer(i)));return NONE;}
        if(!strcmp(name,"most_common")){V pairs=list_new(),keys=map_keys(receiver);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i)),p=list_new();append(p,unary(OP_USUB,map_find(receiver,k,NONE)));append(p,integer(i));append(p,k);append(p,map_find(receiver,k,NONE));obj(p)->type=T_TUPLE;append(pairs,p);}pairs=sorted_values(pairs,NONE,0);V out=list_new();size_t n=a==NONE?length(pairs):(size_t)asint(a);if(n>length(pairs))n=length(pairs);for(size_t i=0;i<n;++i){V p=item(pairs,integer(i)),v=list_new();append(v,item(p,integer(2)));append(v,item(p,integer(3)));obj(v)->type=T_TUPLE;append(out,v);}return out;}
    }
    if(rt==T_CONTEXT){if(!strcmp(name,"get"))return obj(receiver)->a;if(!strcmp(name,"set")){V old=obj(receiver)->a;obj(receiver)->a=a;return old;}if(!strcmp(name,"reset")){obj(receiver)->a=a;return NONE;}}
    if(rt==T_FLOAT&&!strcmp(name,"is_integer"))return floor(asnum(receiver))==asnum(receiver)?TRUEV:FALSEV;
    if(!strcmp(parent,"re")||!strcmp(parent,"regex")){
        int flags=(int)asint(map_find(kw,S("flags"),integer(0)));
        if(!strcmp(name,"compile"))return regex_compile(a,b==NONE?flags:(int)asint(b));
        if(!strcmp(name,"escape")){const char*special="()[]{}?*+-|^$\\.&~# \t\n\r\v\f";V parts=list_new();V chars=iter_values(a);for(size_t i=0;i<length(chars);++i){V ch=item(chars,integer(i));if(obj(ch)->n==1&&strchr(special,cstr(ch)[0]))append(parts,S("\\"));append(parts,ch);}return join(S(""),parts);}
        size_t flags_index=!strcmp(name,"sub")?4:!strcmp(name,"split")?3:2;
        if(argc>flags_index)flags=(int)asint(item(args,integer(flags_index)));
        V rx=regex_compile(a,flags),rest=slice_get(args,1,argc<flags_index?argc:flags_index,1);return regex_operation(name,rx,rest,kw);
    }
    if(!strcmp(parent,"unicodedata")){
        if(!strcmp(name,"normalize"))return unicode_normalize(a,b);
        size_t p=0;uint32_t cp=decode(cstr(a),obj(a)->n,&p);
        if(!strcmp(name,"category"))return str(utf8proc_category_string(cp));
        if(!strcmp(name,"digit")){int digit=unicode_digit(cp);if(digit>=0)return integer(digit);if(argc>1)return b;fail("not a Unicode digit");}
    }
    if(!strcmp(parent,"html")){
        if(!strcmp(name,"unescape"))return html_unescape(a);
        if(!strcmp(name,"escape")){V out=replace_string(a,S("&"),S("&amp;"),-1);out=replace_string(out,S("<"),S("&lt;"),-1);out=replace_string(out,S(">"),S("&gt;"),-1);if(truth(argument(args,kw,1,"quote",TRUEV))){out=replace_string(out,S("\""),S("&quot;"),-1);out=replace_string(out,S("'"),S("&#x27;"),-1);}return out;}
    }
    if(!strcmp(parent,"math")){
        double x=asnum(a);if(!strcmp(name,"floor"))return integer((int64_t)floor(x));if(!strcmp(name,"ceil"))return integer((int64_t)ceil(x));if(!strcmp(name,"sqrt"))return number(sqrt(x));if(!strcmp(name,"isnan"))return isnan(x)?TRUEV:FALSEV;if(!strcmp(name,"isinf"))return isinf(x)?TRUEV:FALSEV;if(!strcmp(name,"isfinite"))return isfinite(x)?TRUEV:FALSEV;
    }
    if(!strcmp(name,"distance")||!strcmp(name,"similarity"))return edit_distance(a,b,!strcmp(name,"similarity"),!strcmp(parent,"OSA"));
    if(!strcmp(name,"linear_sum_assignment"))return assignment(a);
    if(!strcmp(name,"arange")){V out=array_new(1,(size_t)asint(a),1);for(size_t i=0;i<length(out);++i)((double*)obj(out)->ptr)[i]=(double)i;return out;}
    if(!strcmp(name,"zeros")){return array_new(asint(item(a,integer(0))),asint(item(a,integer(1))),2);}
    if(!strcmp(name,"field")){V v=fresh(T_FACTORY);obj(v)->a=map_find(kw,S("default_factory"),NONE);if(obj(v)->a==NONE)return map_find(kw,S("default"),NONE);return v;}
    if(!strcmp(name,"ContextVar")){V v=fresh(T_CONTEXT);obj(v)->a=map_find(kw,S("default"),NONE);return v;}
    if(!strcmp(name,"MetricValue")){V out=map_new();map_update(out,kw);if(map_find(out,S("metadata"),UNBOUND)==UNBOUND)map_set(out,S("metadata"),map_new());return out;}
    if(!strcmp(name,"replace")){V out=fresh(T_INSTANCE);Obj*o=obj(out),*src=obj(a);o->a=src->a;o->slots=src->slots;o->values=mem((o->slots+1)*sizeof(V));memcpy(o->values,src->values,o->slots*sizeof(V));if(src->b){o->b=map_new();map_update(o->b,src->b);}V keys=map_keys(kw);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i));attr_set(out,cstr(k),map_find(kw,k,NONE));}return out;}
    if(!strcmp(name,"fromkeys")){V out=map_new(),keys=iter_values(a);for(size_t i=0;i<length(keys);++i)map_set(out,item(keys,integer(i)),b);return out;}
    if(!strcmp(name,"maketrans")){V out=map_new();if(b!=NONE){V av=iter_values(a),bv=iter_values(b);for(size_t i=0;i<length(av);++i){size_t p=0;uint32_t cp=decode(cstr(item(av,integer(i))),obj(item(av,integer(i)))->n,&p);map_set(out,integer(cp),item(bv,integer(i)));}}else{V keys=map_keys(a);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i)),v=map_find(a,k,NONE);if(type(k)==T_STR){size_t p=0;k=integer(decode(cstr(k),obj(k)->n,&p));}map_set(out,k,v);}}return out;}
    if(!strcmp(name,"len"))return integer(length(a));
    if(!strcmp(name,"bool"))return truth(a)?TRUEV:FALSEV;
    if(!strcmp(name,"str")||!strcmp(name,"repr"))return !argc?S(""):stringify(a,!strcmp(name,"repr"));
    if(!strcmp(name,"int")){if(a==NONE)return integer(0);if(type(a)==T_STR){char*end;int64_t v=strtoll(cstr(a),&end,b==NONE?10:(int)asint(b));if(end==cstr(a)||*end)fail("invalid integer %s",cstr(a));return integer(v);}return integer(asint(a));}
    if(!strcmp(name,"float")){if(a==NONE)return number(0);if(type(a)==T_STR){char*end;double v=strtod(cstr(a),&end);if(end==cstr(a)||*end)fail("invalid float %s",cstr(a));return number(v);}return number(asnum(a));}
    if(!strcmp(name,"list")||!strcmp(name,"tuple")||!strcmp(name,"set")||!strcmp(name,"frozenset")){V out=list_new();if(a!=NONE)extend(out,a);if(!strcmp(name,"tuple"))obj(out)->type=T_TUPLE;if(!strcmp(name,"set")||!strcmp(name,"frozenset"))return set_from(out);return out;}
    if(!strcmp(name,"dict")){V out=map_new();if(a!=NONE){if(type(a)==T_MAP)map_update(out,a);else{V values=iter_values(a);for(size_t i=0;i<length(values);++i){V pair=item(values,integer(i));map_set(out,item(pair,integer(0)),item(pair,integer(1)));}}}map_update(out,kw);return out;}
    if(!strcmp(name,"Counter")){V out=map_new();obj(out)->flags=99;if(a!=NONE){if(type(a)==T_MAP)map_update(out,a);else{V values=iter_values(a);for(size_t i=0;i<length(values);++i){V k=item(values,integer(i));map_set(out,k,integer(asint(map_find(out,k,integer(0)))+1));}}}return out;}
    if(!strcmp(name,"range")){int64_t lo=b==NONE?0:asint(a),hi=b==NONE?asint(a):asint(b),step=c==NONE?1:asint(c);if(!step)fail("range step zero");V out=list_new();for(int64_t i=lo;step>0?i<hi:i>hi;i+=step)append(out,integer(i));return out;}
    if(!strcmp(name,"enumerate")){V values=iter_values(a),out=list_new();int64_t start=asint(b==NONE?map_find(kw,S("start"),integer(0)):b);for(size_t i=0;i<length(values);++i){V pair=list_new();append(pair,integer(start+i));append(pair,item(values,integer(i)));obj(pair)->type=T_TUPLE;append(out,pair);}return out;}
    if(!strcmp(name,"zip")){V out=list_new(),streams=list_new();size_t n=SIZE_MAX;for(size_t i=0;i<argc;++i){V seq=iter_values(item(args,integer(i)));append(streams,seq);if(length(seq)<n)n=length(seq);}if(!argc)n=0;for(size_t j=0;j<n;++j){V pair=list_new();for(size_t i=0;i<argc;++i)append(pair,item(item(streams,integer(i)),integer(j)));obj(pair)->type=T_TUPLE;append(out,pair);}return out;}
    if(!strcmp(name,"sorted"))return sorted_values(a,map_find(kw,S("key"),NONE),truth(map_find(kw,S("reverse"),FALSEV)));
    if(!strcmp(name,"reversed")){V seq=iter_values(a),out=list_new();for(size_t i=length(seq);i-->0;)append(out,item(seq,integer(i)));return out;}
    if(!strcmp(name,"sum")||!strcmp(name,"any")||!strcmp(name,"all")){V seq=iter_values(a),total=b==NONE?integer(0):b;double hi=0,lo=0;int floating=0;for(size_t i=0;i<length(seq);++i){V x=item(seq,integer(i));if(!strcmp(name,"any")&&truth(x))return TRUEV;if(!strcmp(name,"all")&&!truth(x))return FALSEV;if(!strcmp(name,"sum")){if(!floating&&type(x)!=T_FLOAT){total=binary(OP_ADD,total,x);continue;}if(!floating){hi=asnum(total);floating=1;}double y=asnum(x),t=hi+y;lo+=fabs(hi)>=fabs(y)?(hi-t)+y:(y-t)+hi;hi=t;}}if(!strcmp(name,"sum"))return floating?number(lo&&isfinite(lo)?hi+lo:hi):total;return !strcmp(name,"all")?TRUEV:FALSEV;}
    if(!strcmp(name,"min")||!strcmp(name,"max")){V seq=argc==1?iter_values(a):args,key=map_find(kw,S("key"),NONE);if(!length(seq)){V fallback=map_find(kw,S("default"),UNBOUND);if(fallback==UNBOUND)fail("empty min/max");return fallback;}V best=item(seq,integer(0)),bk=best;if(key!=NONE){V aa=list_new();append(aa,best);bk=call(key,aa,map_new());}for(size_t i=1;i<length(seq);++i){V v=item(seq,integer(i)),k=v;if(key!=NONE){V aa=list_new();append(aa,v);k=call(key,aa,map_new());}int cmp=ordering(k,bk);if(!strcmp(name,"min")?cmp<0:cmp>0){best=v;bk=k;}}return best;}
    if(!strcmp(name,"abs"))return type(a)==T_FLOAT?number(fabs(asnum(a))):integer(llabs(asint(a)));
    if(!strcmp(name,"round")){double scale=b==NONE?1:pow(10,asnum(b));return number(nearbyint(asnum(a)*scale)/scale);}
    if(!strcmp(name,"ord")){size_t p=0;return integer(decode(cstr(a),obj(a)->n,&p));}if(!strcmp(name,"chr"))return chr_value((uint32_t)asint(a));
    if(!strcmp(name,"id"))return integer((int64_t)a);if(!strcmp(name,"hash"))return integer((int64_t)hash_value(a));
    if(!strcmp(name,"getattr")){int t=type(a);if(t==T_INSTANCE){Obj*o=obj(a);V slot=map_find(obj(o->a)->locals_map,b,UNBOUND);if(slot!=UNBOUND&&o->values[asint(slot)]!=UNBOUND)return o->values[asint(slot)];V x=o->b?map_find(o->b,b,UNBOUND):UNBOUND;if(x!=UNBOUND)return x;V m=map_find(obj(o->a)->env->vars,b,UNBOUND);if(m==UNBOUND)return c;}if(t==T_MAP){V x=map_find(a,b,UNBOUND);return x==UNBOUND?c:x;}return attr(a,cstr(b));}
    if(!strcmp(name,"setattr")){attr_set(a,cstr(b),c);return NONE;}
    if(!strcmp(name,"iter")){V v=fresh(T_ITER);obj(v)->a=iter_values(a);obj(v)->b=integer(0);return v;}
    if(!strcmp(name,"next")){if(type(a)==T_ITER){size_t p=(size_t)asint(obj(a)->b);if(p>=length(obj(a)->a))return b;obj(a)->b=integer(p+1);return item(obj(a)->a,integer(p));}V seq=iter_values(a);return length(seq)?item(seq,integer(0)):b;}
    if(!strcmp(name,"map")){V seq=iter_values(b),out=list_new();for(size_t i=0;i<length(seq);++i){V aa=list_new();append(aa,item(seq,integer(i)));append(out,call(a,aa,map_new()));}return out;}
    if(!strcmp(name,"isinstance")){V choices=type(b)==T_TUPLE?b:list_new();if(type(b)!=T_TUPLE)append(choices,b);for(size_t i=0;i<length(choices);++i){V cls=item(choices,integer(i));int t=type(a);if(type(cls)==T_CLASS){if(t==T_INSTANCE&&obj(a)->a==cls)return TRUEV;}else if(type(cls)==T_NATIVE){const char*n=obj(cls)->s;if((!strcmp(n,"str")&&t==T_STR)||((!strcmp(n,"dict")||!strcmp(n,"Mapping"))&&t==T_MAP)||(!strcmp(n,"list")&&t==T_LIST)||(!strcmp(n,"tuple")&&t==T_TUPLE)||(!strcmp(n,"int")&&(t==-1||t==-2))||(!strcmp(n,"float")&&t==T_FLOAT)||(!strcmp(n,"bool")&&t==-2)||(!strcmp(n,"set")&&t==T_SET)||(!strcmp(n,"ndarray")&&t==T_ARRAY))return TRUEV;}}return FALSEV;}
    if(!strcmp(name,"bisect_left")){size_t lo=0,hi=length(a);while(lo<hi){size_t mid=lo+(hi-lo)/2;if(ordering(item(a,integer(mid)),b)<0)lo=mid+1;else hi=mid;}return integer(lo);}
    if(!strcmp(name,"object"))return fresh(T_INSTANCE);
    if(!strcmp(name,"ValueError")||!strcmp(name,"RuntimeError")||!strcmp(name,"TypeError"))return stringify(a,0);
    fail("unimplemented native call: %s.%s (receiver %d)",parent,name,rt);return NONE;
}
