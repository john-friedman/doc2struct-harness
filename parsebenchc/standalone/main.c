/* Native executable: no Python headers, Python objects, or Python runtime. */
#include "runtime.c"
#include "kernels.c"
#include "library.c"
#include "json.c"
static void debug_return(Env*e,V result){FILE*f=fopen("parsebenchc/build/standalone/function_dump.jsonl","ab");if(!f)return;V snapshot=map_new();Obj*fn=e->function;for(size_t i=0;i<fn->slots;++i)if(e->locals[i]!=UNBOUND)map_set(snapshot,fn->values[i],e->locals[i]);map_set(snapshot,S("__result"),result);json_write(f,snapshot);fputc('\n',f);fclose(f);}
V scorer_init_text_next(Env*);V scorer_init_formatting_v2(Env*);V scorer_init_table_next(Env*);V scorer_init_forms_v2(Env*);V scorer_init_chart_next(Env*);

typedef struct {const char*name;Code init;} ModuleInit;
static ModuleInit scorer_modules[]={
    {"text_next",scorer_init_text_next},{"formatting_v2",scorer_init_formatting_v2},
    {"table_next",scorer_init_table_next},{"forms_v2",scorer_init_forms_v2},{"chart_next",scorer_init_chart_next},
    {NULL,NULL}
};
V module_get(const char*name){
    V key=str(name),v=map_find(modules,key,UNBOUND);if(v!=UNBOUND)return v;
    v=fresh(T_MODULE);obj(v)->s=(char*)cstr(key);obj(v)->env=env_new(builtins);obj(v)->env->vars=map_new();map_set(modules,key,v);
    const char*shortname=strrchr(name,'.');shortname=shortname?shortname+1:name;
    for(int i=0;scorer_modules[i].name;++i)if(!strcmp(shortname,scorer_modules[i].name)){
        Env*e=obj(v)->env;scorer_modules[i].init(e);
        if(!strcmp(shortname,"text_next")||!strcmp(shortname,"formatting_v2")){
            put(e,"__original_lcs",get(e,"_lcs_pairs"));put(e,"_lcs_pairs",native("__c_lcs",v));put(e,"_tile",native("__c_tile",v));
        }
        if(!strcmp(shortname,"text_next")){
            put(e,"_utf16_len",native("__c_u16_len",v));
            put(e,"_lcs_len",native("__c_lcs_len",v));
            put(e,"_large_lcs_pairs",native("__c_large_lcs",v));
            const char*memo[]={"_strip_markup","_word_key","_fold","_fold_marks","_shape","_joined","split_run",NULL};
            for(int j=0;memo[j];++j){V fn=get(e,memo[j]);obj(fn)->flags|=FN_CACHE;if(j==6)obj(fn)->flags|=FN_CLONE;obj(fn)->cap=65536;}
            const char*parsed[]={"_raw_lines","tokenize","score_tokens",NULL};for(int j=0;parsed[j];++j){V fn=get(e,parsed[j]);obj(fn)->flags|=FN_CACHE|FN_CLONE;obj(fn)->cap=16;}
        }
        if(!strcmp(shortname,"formatting_v2")){
            put(e,"__fold_chars",map_new());put(e,"tokens_of",native("__c_format_tokens",v));
            const char*memo[]={"_is_word","_key_chars","parse",NULL};for(int j=0;memo[j];++j){V fn=get(e,memo[j]);obj(fn)->flags|=FN_CACHE;if(j==2)obj(fn)->flags|=FN_CLONE;obj(fn)->cap=j==2?8:8192;}
        }
        if(!strcmp(shortname,"forms_v2")){
            put(e,"_candidates",native("__c_candidates",v));put(e,"_copies",native("__c_copies",v));put(e,"_window_bound",native("__c_window_bound",v));put(e,"_lev",native("__c_lev",v));
            put(e,"_window_hits",native("__c_window",v));put(e,"__fold_chars",map_new());put(e,"__token_cache",map_new());
            put(e,"tokenize",native("__c_form_tokens",v));put(e,"_isdigits",native("__c_form_digits",v));put(e,"_weight",native("__c_form_weight",v));
            const char*memo[]={"clean_text","word_keys",NULL};for(int j=0;memo[j];++j){V fn=get(e,memo[j]);obj(fn)->flags|=FN_CACHE;if(j==1)obj(fn)->flags|=FN_CLONE;obj(fn)->cap=16384;}
        }
        if(!strcmp(shortname,"table_next")){
            put(e,"_match_triples_native",native("__c_matches",v));
            put(e,"_table_spans",native("__c_table_spans",v));put(e,"_kv_native",native("__c_table_kv",v));put(e,"_fit_native",native("__c_table_fit",v));put(e,"_pct_key",native("__c_table_pct",v));
            put(e,"__original_norm",get(e,"nx_norm"));put(e,"nx_norm",native("__c_norm",v));put(e,"_trim",native("__c_trim",v));
            put(e,"_u16_len",native("__c_u16_len",v));put(e,"_sorted_pair",native("__c_sorted_pair",v));put(e,"is_empty",native("__c_empty",v));
            put(e,"key_sim",native("__c_keysim",v));
            const char*cloned[]={"parse_tables","_html_cell","clean_grid","build_triples","_value_bag",NULL};
            size_t sizes[]={32,16384,256,1024,512};
            for(int j=0;cloned[j];++j){V fn=get(e,cloned[j]);obj(fn)->flags|=FN_CACHE|FN_CLONE;if(j<2)obj(fn)->flags|=FN_ATTACH;obj(fn)->cap=sizes[j];}
            const char*memo[]={"decode_entities","_base","_post","_surface","_key","_ungroup","value_sim","is_numeric","path_of","is_section_row","infer_schema","_locale","strip_footnotes","_commas_dropped","_num_key",NULL};
            for(int j=0;memo[j];++j){V fn=get(e,memo[j]);obj(fn)->flags|=FN_CACHE;obj(fn)->cap=65536;}
            V numeric=get(e,"nx_number");obj(numeric)->flags|=FN_CACHE|FN_CLONE;obj(numeric)->cap=65536;
            put(e,"__original_value_sim",get(e,"value_sim"));put(e,"value_sim",native("__c_value_sim",v));put(e,"_pair_rows_by_values",native("__c_pair_rows",v));
        }
        return v;
    }
    return v;
}
void runtime_init(void){
    memo_disabled=getenv("PARSEBENCHC_NO_CACHE")!=NULL;
    dump_function=getenv("PARSEBENCHC_DUMP");
    if(!regex_is_loaded){regex_load();regex_is_loaded=1;}
    if(!match_context){match_context=p_match_context(NULL);pcre2_jit_stack*stack=p_jit_stack(32768,8u<<20,NULL);p_jit_assign(match_context,NULL,stack);}
    symbols=map_new();empty_kwargs=map_new();builtins=env_new(NULL);modules=map_new();regex_cache=map_new();
    const char*names[]={"len","range","enumerate","any","all","sum","min","max","abs","round","str","int","float","bool","list","tuple","set","frozenset","dict","sorted","reversed","zip","next","iter","map","ord","chr","getattr","setattr","isinstance","id","hash","repr","object","ValueError","RuntimeError","TypeError",NULL};
    for(int i=0;names[i];++i)put(builtins,names[i],native(names[i],NONE));
    put(builtins,"Mapping",native("Mapping",NONE));put(builtins,"Sequence",native("Sequence",NONE));
}
static V invoke(const char*module,const char*name,size_t n,...){
    V m=module_get(module),a=list_new();va_list ap;va_start(ap,n);for(size_t i=0;i<n;++i)append(a,va_arg(ap,V));va_end(ap);return call(attr(m,name),a,map_new());
}
#include "document.c"
static V score_pair(V job){
    if(map_find(job,S("function"),UNBOUND)!=UNBOUND){V value=invoke_args(cstr(map_find(job,S("module"),S("forms_v2"))),cstr(map_find(job,S("function"),NONE)),map_find(job,S("args"),list_new()),map_find(job,S("kwargs"),map_new()));V result=map_new();map_set(result,S("value"),value);return result;}
    if(map_find(job,S("testcase"),UNBOUND)!=UNBOUND)return score_document(map_find(job,S("testcase"),NONE),map_find(job,S("result"),NONE));
    V reference=map_find(job,S("reference"),S("")),prediction=map_find(job,S("prediction"),S(""));
    V flags=map_find(job,S("flags"),map_new()),out=map_new();
    const char*metric=cstr(map_find(job,S("metric"),S("all")));
    if(!strcmp(metric,"tokenize")){V ref=invoke("text_next","tokenize",1,reference),hyp=invoke("text_next","tokenize",1,prediction);map_set(out,S("reference"),ref);map_set(out,S("prediction"),hyp);return out;}
    if(!strcmp(metric,"table_debug")){V args=list_new();append(args,reference);append(args,prediction);return instance_map(invoke_args("table_next","score_table_next",args,flags));}
    if(!strcmp(metric,"forms_locate")||!strcmp(metric,"forms_debug")){V metadata=map_find(job,S("metadata"),map_new()),rules=map_find(job,S("rules"),list_new());V ref=invoke("forms_v2","forms_reference",2,metadata,rules);
        if(!strcmp(metric,"forms_locate")){V out=invoke("forms_v2","read_output",1,prediction),ret=map_new();map_set(ret,S("value"),invoke("forms_v2","locate",2,ref,out));return ret;}
        return instance_map(invoke("forms_v2","score_forms",2,ref,prediction));}
    if(!strcmp(metric,"all")||!strcmp(metric,"text")){
        V result=invoke("text_next","score_text_next",2,reference,prediction);map_set(out,S("text_next"),attr(result,"score"));
        const char*lanes[]={"words","critical","integrity","order",NULL};for(int i=0;lanes[i];++i){V value=attr(result,lanes[i]);if(value!=NONE){char name[64];snprintf(name,sizeof(name),"text_next_%s",lanes[i]);map_set(out,str(name),value);}}
    }
    if(!strcmp(metric,"all")||!strcmp(metric,"formatting")){
        V result=invoke("formatting_v2","score_formatting",2,reference,prediction);map_set(out,S("formatting_v2"),attr(result,"score"));
        V lanes=attr(result,"lanes"),keys=map_keys(lanes);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i)),v=map_find(lanes,k,NONE);if(v!=NONE)map_set(out,binary(OP_ADD,S("formatting_v2_"),k),v);}
    }
    if(!strcmp(metric,"all")||!strcmp(metric,"tables")){
        V a=list_new();append(a,reference);append(a,prediction);V result=call(attr(module_get("table_next"),"score_table_next"),a,flags);map_set(out,S("table_next"),attr(result,"score"));map_set(out,S("table_next_content"),attr(result,"content"));map_set(out,S("table_next_row_order"),attr(result,"row_order"));
    }
    if(!strcmp(metric,"forms")){
        V metadata=map_find(job,S("metadata"),map_new()),rules=map_find(job,S("rules"),list_new());
        V ref=invoke("forms_v2","forms_reference",2,metadata,rules);
        if(ref!=NONE){V result=invoke("forms_v2","score_forms",2,ref,prediction);map_set(out,S("forms_v2"),attr(result,"score"));V lanes=attr(result,"lanes"),keys=map_keys(lanes);for(size_t i=0;i<length(keys);++i){V k=item(keys,integer(i)),value=map_find(lanes,k,NONE);if(value!=NONE)map_set(out,binary(OP_ADD,S("forms_v2_"),k),value);}}
    }
    return out;
}
#include "parallel.c"
#ifdef _WIN32
int wmain(int argc,wchar_t**wide){char**args=calloc((size_t)argc+1,sizeof(char*));if(!args)return 2;for(int i=0;i<argc;++i){int n=WideCharToMultiByte(CP_UTF8,0,wide[i],-1,NULL,0,NULL,NULL);args[i]=malloc(n);if(!args[i])return 2;WideCharToMultiByte(CP_UTF8,0,wide[i],-1,args[i],n,NULL,NULL);}int code=native_main(argc,args);for(int i=0;i<argc;++i)free(args[i]);free(args);return code;}
#else
int main(int argc,char**argv){return native_main(argc,argv);}
#endif
