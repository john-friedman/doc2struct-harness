typedef struct {const char*s;size_t n,p;} Json;
static void json_ws(Json*j){while(j->p<j->n&&(j->s[j->p]==' '||j->s[j->p]=='\t'||j->s[j->p]=='\r'||j->s[j->p]=='\n'))++j->p;}
static int hex_digit(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static V json_string(Json*j){
    if(j->s[j->p++]!='"')fail("JSON string expected");
    size_t end=j->p;while(end<j->n){char c=j->s[end++];if(c=='"')break;if(c=='\\'&&end<j->n)++end;}
    char*buf=mem(end-j->p+1);size_t q=0;
    while(j->p<j->n){char c=j->s[j->p++];if(c=='"')return strn(buf,q);if(c!='\\'){buf[q++]=c;continue;}if(j->p>=j->n)fail("JSON escape");c=j->s[j->p++];
        if(c=='u'){uint32_t cp=0;for(int i=0;i<4;++i){if(j->p>=j->n||hex_digit(j->s[j->p])<0)fail("JSON unicode escape");cp=cp*16+hex_digit(j->s[j->p++]);}
            if(cp>=0xd800&&cp<=0xdbff&&j->p+6<=j->n&&j->s[j->p]=='\\'&&j->s[j->p+1]=='u'){j->p+=2;uint32_t low=0;for(int i=0;i<4;++i)low=low*16+hex_digit(j->s[j->p++]);cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);}q+=encode(cp,buf+q);
        }else buf[q++]=c=='n'?'\n':c=='r'?'\r':c=='t'?'\t':c=='b'?'\b':c=='f'?'\f':c;
    }fail("unterminated JSON string");return NONE;
}
static V json_value(Json*j){
    json_ws(j);if(j->p>=j->n)fail("unexpected end of JSON");char c=j->s[j->p];
    if(c=='"')return json_string(j);
    if(c=='{'||c=='['){int map=c=='{';++j->p;V out=map?map_new():list_new();json_ws(j);if(j->p<j->n&&j->s[j->p]==(map?'}':']')){++j->p;return out;}
        while(j->p<j->n){if(map){json_ws(j);V key=json_string(j);json_ws(j);if(j->s[j->p++]!=':')fail("expected JSON colon");map_set(out,key,json_value(j));}else append(out,json_value(j));json_ws(j);char end=j->s[j->p++];if(end==(map?'}':']'))return out;if(end!=',')fail("expected JSON comma");}fail("unterminated JSON container");
    }
    if(!strncmp(j->s+j->p,"true",4)){j->p+=4;return TRUEV;}if(!strncmp(j->s+j->p,"false",5)){j->p+=5;return FALSEV;}if(!strncmp(j->s+j->p,"null",4)){j->p+=4;return NONE;}
    size_t a=j->p;while(j->p<j->n&&strchr("-+0123456789.eE",j->s[j->p]))++j->p;if(a==j->p)fail("invalid JSON at byte %zu",j->p);V text=strn(j->s+a,j->p-a);if(strpbrk(cstr(text),".eE"))return number(strtod(cstr(text),NULL));return integer(strtoll(cstr(text),NULL,10));
}
V json_parse(const char*s,size_t n,size_t*consumed){Json j={s,n,0};V out=json_value(&j);json_ws(&j);if(consumed)*consumed=j.p;return out;}
static void json_quote(FILE*out,V v){fputc('"',out);const char*s=cstr(v);for(size_t i=0;i<obj(v)->n;++i){unsigned char c=s[i];if(c=='"'||c=='\\'){fputc('\\',out);fputc(c,out);}else if(c=='\n')fputs("\\n",out);else if(c=='\r')fputs("\\r",out);else if(c=='\t')fputs("\\t",out);else if(c<32)fprintf(out,"\\u%04x",c);else fputc(c,out);}fputc('"',out);}
void json_write(FILE*out,V v){int t=type(v);if(t==0)fputs("null",out);else if(t==-2)fputs(truth(v)?"true":"false",out);else if(t==-1)fprintf(out,"%lld",(long long)asint(v));else if(t==T_FLOAT){if(isfinite(asnum(v)))fprintf(out,"%.17g",asnum(v));else fputs("null",out);}else if(t==T_STR||t==T_BYTES)json_quote(out,v);else if(t==T_LIST||t==T_TUPLE||t==T_SET){V values=t==T_SET?map_keys(v):v;fputc('[',out);for(size_t i=0;i<length(values);++i){if(i)fputc(',',out);json_write(out,item(values,integer(i)));}fputc(']',out);}else if(t==T_MAP){V keys=map_keys(v);fputc('{',out);for(size_t i=0;i<length(keys);++i){if(i)fputc(',',out);V key=item(keys,integer(i));json_quote(out,type(key)==T_STR?key:stringify(key,0));fputc(':',out);json_write(out,map_find(v,key,NONE));}fputc('}',out);}else if(t==T_INSTANCE)json_write(out,instance_map(v));else fputs("null",out);}
