/* Linear HTML table boundary scanner, retaining the reference's handling of
   malformed nesting and unclosed rows/cells. Offsets count Unicode code points. */
static int tag_at(V text,size_t p,const char*tag){size_t n=strlen(tag);if(p+n>obj(text)->n)return 0;for(size_t i=0;i<n;++i){unsigned c=(unsigned char)cstr(text)[p+i];if(c>='A'&&c<='Z')c+=32;if(c!=(unsigned char)tag[i])return 0;}return 1;}
static int tag_word(V text,size_t p){if(p>=obj(text)->n)return 0;size_t q=p;uint32_t cp=decode(cstr(text),obj(text)->n,&q);return(cp>='a'&&cp<='z')||(cp>='A'&&cp<='Z')||(cp>='0'&&cp<='9')||cp=='_'||cp==0x130||cp==0x131||cp==0x17f||cp==0x212a;}
static void table_span_add(V out,V text,size_t a,size_t b){V span=list_new();append(span,integer(cppos(text,a)));append(span,integer(cppos(text,b)));obj(span)->type=T_TUPLE;append(out,span);}
static V table_spans_native(V text){
    size_t n=obj(text)->n,depth=0,start=0;unsigned char*stack=malloc(n+1);if(!stack)fail("table scanner allocation");V out=list_new();
    for(size_t p=0;p<n;++p){if(cstr(text)[p]!='<')continue;
        if(tag_at(text,p,"<table")&&!tag_word(text,p+6)){int any=0;for(size_t i=0;i<depth;++i)any|=stack[i];if(depth&&!any){table_span_add(out,text,start,p);depth=0;}while(depth&&!stack[depth-1])--depth;if(!depth)start=p;stack[depth++]=0;p+=5;}
        else if(tag_at(text,p,"</table>")){if(depth&&!--depth)table_span_add(out,text,start,p+8);p+=7;}
        else{size_t q=p+1;int close=q<n&&cstr(text)[q]=='/';q+=close;const char*tags[]={"thead","tbody","tfoot","tr","td","th"};for(size_t i=0;i<6;++i){size_t len=strlen(tags[i]);if(tag_at(text,q,tags[i])&&!tag_word(text,q+len)){if(depth)stack[depth-1]=(unsigned char)(!close&&i>=4);p=q+len-1;break;}}}
    }
    if(depth)table_span_add(out,text,start,n);free(stack);return out;
}
typedef struct {V kv,pct,digits;int footnote_free;} TripleCache;
static TripleCache*triple_cache(V triple){if(!obj(triple)->ptr){TripleCache*c=mem(sizeof(*c));obj(triple)->ptr=c;}return obj(triple)->ptr;}
static V table_kv_native(V module,V x){TripleCache*cache=triple_cache(x);if(cache->kv)return cache->kv;V k=field(x,0),args[]={field(x,3)};V norm=call_pos(attr(module,"nx_norm"),1,args);V parts=list_new();append(parts,item(k,integer(0)));append(parts,S("\001"));append(parts,item(k,integer(1)));append(parts,S("\002"));append(parts,norm);return cache->kv=join(S(""),parts);}
static V table_pct_native(V x){TripleCache*cache=triple_cache(x);if(cache->pct)return cache->pct;return cache->pct=binary(OP_ADD,binary(OP_ADD,join(S(" "),field(x,0)),S(" ")),join(S(" "),field(x,6)));}
static V table_fit_native(V module,V triples,V gis,V kv_rows){
    V rows=map_new();int64_t total=0;
    for(size_t i=0;i<length(triples);++i){V x=item(triples,integer(i)),pairs=map_find(kv_rows,table_kv_native(module,x),NONE);if(pairs==NONE||!length(pairs))continue;
        V ri=field(x,4),row=map_find(rows,ri,UNBOUND);if(row==UNBOUND){row=map_new();map_set(rows,ri,row);}int64_t old=(int64_t)obj(row)->slots,best=old;
        for(size_t j=0;j<length(pairs);++j){V pair=item(pairs,integer(j));if(gis!=NONE&&!contains(gis,item(pair,integer(0))))continue;int duplicate=0;for(size_t k=0;k<j;++k)if(equal(pair,item(pairs,integer(k)))){duplicate=1;break;}if(duplicate)continue;int64_t count=asint(map_find(row,pair,integer(0)))+1;map_set(row,pair,integer(count));if(count>best)best=count;}
        obj(row)->slots=(size_t)best;total+=best-old;
    }return integer(total);
}
