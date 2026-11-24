/* Formatting tokens retain per-character style bits and script lead flags. */
V format_has(V token,V kind){int k=(int)asint(kind);V bits=obj(token)->values[1];size_t n=length(bits),count=0;for(size_t i=0;i<n;++i)count+=(asint(item(bits,integer(i)))&k)!=0;return(count&&(k==16||k==32||2*count>=n))?TRUEV:FALSEV;}
static int format_lead(V chars,size_t ci){int lead=0;const int kinds[]={16,32};for(int z=0;z<2;++z){int k=kinds[z];int64_t x=(int64_t)ci-1;while(x>=0){V pair=item(chars,integer(x)),ch=item(pair,integer(0));if(!contains(S("-−+–"),ch)||!(asint(item(pair,integer(1)))&k))break;--x;}if(x<(int64_t)ci-1&&x>=0){V pair=item(chars,integer(x)),ch=item(pair,integer(0));if(obj(ch)->n==1&&cstr(ch)[0]>='0'&&cstr(ch)[0]<='9'&&!(asint(item(pair,integer(1)))&k))lead|=k;}}return lead;}
static V format_token(V cls,V key,V bits,V unit,int lead){V v=fresh(T_INSTANCE);Obj*o=obj(v);o->a=cls;o->slots=4;o->values=mem(4*sizeof(V));o->values[0]=key;o->values[1]=bits;o->values[2]=unit;o->values[3]=integer(lead);return v;}
static V format_tokens(V module,V chars,V unit){
    size_t capacity=256,used=0;char*word=malloc(capacity);if(!word)fail("format token allocation");V out=list_new(),bits=list_new(),cls=attr(module,"Tok");int lead=0;const int split=207;
#define FMT_FLUSH() do{if(used){append(out,format_token(cls,strn(word,used),bits,unit,lead));used=0;bits=list_new();}}while(0)
    for(size_t i=0;i<length(chars);++i){V pair=item(chars,integer(i)),ch=item(pair,integer(0)),style=item(pair,integer(1));size_t p=0;uint32_t cp=decode(cstr(ch),obj(ch)->n,&p);utf8proc_category_t cat=utf8proc_category(cp);int wordchar=(cat>=UTF8PROC_CATEGORY_LU&&cat<=UTF8PROC_CATEGORY_ME)||(cat>=UTF8PROC_CATEGORY_ND&&cat<=UTF8PROC_CATEGORY_NO);
        if(form_cjk(cp)){FMT_FLUSH();V key=form_fold(module,cp);if(obj(key)->n){V b=list_new();for(size_t j=0;j<length(key);++j)append(b,style);append(out,format_token(cls,key,b,unit,0));}}
        else if(wordchar){V key=form_fold(module,cp);if(obj(key)->n&&used&&((asint(style)^asint(item(bits,integer(length(bits)-1))))&split))FMT_FLUSH();if(obj(key)->n&&!used)lead=format_lead(chars,i);size_t n=obj(key)->n;if(used+n>capacity){capacity=(used+n)*2;char*next=realloc(word,capacity);if(!next)fail("format token allocation");word=next;}memcpy(word+used,cstr(key),n);used+=n;for(size_t j=0;j<length(key);++j)append(bits,style);}
        else FMT_FLUSH();
    }FMT_FLUSH();free(word);return out;
#undef FMT_FLUSH
}
