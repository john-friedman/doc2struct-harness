/* The form scanner uses code points and compact token records directly. */
static int form_cjk(uint32_t cp){return(cp>=0x3005&&cp<=0x3007)||(cp>=0x3040&&cp<=0x30ff)||(cp>=0x3400&&cp<=0x4dbf)||(cp>=0x4e00&&cp<=0x9fff)||(cp>=0xf900&&cp<=0xfaff)||(cp>=0xe00&&cp<=0xe7f);}
static int glyph_in(V characters,uint32_t cp){char bytes[4];size_t n=encode(cp,bytes);return bytes_find(cstr(characters),obj(characters)->n,bytes,n)!=NULL;}
static V form_fold(V module,uint32_t cp){
    V cache=attr(module,"__fold_chars"),key=integer(cp),hit=map_find(cache,key,UNBOUND);if(hit!=UNBOUND)return hit;
    V normalized=unicode_normalize(S("NFKD"),chr_value(cp));char*bytes=mem(obj(normalized)->n+1);size_t p=0,n=0;
    while(p<obj(normalized)->n){size_t a=p;uint32_t c=decode(cstr(normalized),obj(normalized)->n,&p);if(utf8proc_category(c)!=UTF8PROC_CATEGORY_MN){memcpy(bytes+n,cstr(normalized)+a,p-a);n+=p-a;}}
    V folded=lower_string(strn(bytes,n),0,0);map_set(cache,key,folded);return folded;
}
static V form_token(V cls,V key,size_t start,size_t end,int box){V result=fresh(T_INSTANCE);Obj*o=obj(result);o->a=cls;o->slots=4;o->values=mem(4*sizeof(V));o->values[0]=key;o->values[1]=integer(start);o->values[2]=integer(end);o->values[3]=integer(box);return result;}
static V forms_tokenize(V module,V text){
    V cache=attr(module,"__token_cache"),cached=map_find(cache,text,UNBOUND);if(cached!=UNBOUND)return clone_value(cached);
    V cls=attr(module,"Tok"),checked=attr(module,"CHECKED_CHARS"),unchecked=attr(module,"UNCHECKED_CHARS"),out=list_new();
    size_t capacity=256,used=0,start=0,p=0,ci=0;char*word=mem(capacity);
#define FLUSH_WORD(end_index) do{if(used){append(out,form_token(cls,strn(word,used),start,(end_index),0));used=0;}}while(0)
    while(p<obj(text)->n){uint32_t cp=decode(cstr(text),obj(text)->n,&p);
        if((cp==0x25cf||cp==0x25c9)&&length(out)&&!used){Obj*last=obj(item(out,integer(length(out)-1)));if(truth(last->values[3])){V between=slice_get(text,asint(last->values[2]),(int64_t)ci,1);if(!truth(strip_string(between,NONE,1,1))){++ci;continue;}}}
        int state=cp>=128?(glyph_in(checked,cp)?1:glyph_in(unchecked,cp)?2:0):0;
        if(state){FLUSH_WORD(ci);append(out,form_token(cls,state==1?attr(module,"CHECKED"):attr(module,"UNCHECKED"),ci,ci+1,state));}
        else if(form_cjk(cp)){FLUSH_WORD(ci);V key=form_fold(module,cp);if(obj(key)->n)append(out,form_token(cls,key,ci,ci+1,0));}
        else{
            utf8proc_category_t category=cp<128?UTF8PROC_CATEGORY_CN:utf8proc_category(cp);
            int isword=cp<128?((cp>='0'&&cp<='9')||(cp>='a'&&cp<='z')||(cp>='A'&&cp<='Z')):(category>=UTF8PROC_CATEGORY_LU&&category<=UTF8PROC_CATEGORY_ME)||(category>=UTF8PROC_CATEGORY_ND&&category<=UTF8PROC_CATEGORY_NO);
            if(isword){V key=NONE;char ascii_byte[1];const char*bytes;size_t n;
                if(cp<128){ascii_byte[0]=(char)(cp>='A'&&cp<='Z'?cp+32:cp);bytes=ascii_byte;n=1;}else{key=form_fold(module,cp);bytes=cstr(key);n=obj(key)->n;}
                if(n&&!used)start=ci;if(used+n>capacity){size_t next=(used+n)*2;char*buffer=mem(next);memcpy(buffer,word,used);word=buffer;capacity=next;}memcpy(word+used,bytes,n);used+=n;
            }else FLUSH_WORD(ci);
        }
        ++ci;
    }
    FLUSH_WORD(ci);
#undef FLUSH_WORD
    if(length(cache)<16384)map_set(cache,text,clone_value(out));return out;
}
static V forms_digits(V key){if(!obj(key)->n)return FALSEV;for(size_t i=0;i<obj(key)->n;++i)if(cstr(key)[i]<'0'||cstr(key)[i]>'9')return FALSEV;return TRUEV;}
