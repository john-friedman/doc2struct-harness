/* Typed kernels used by the C scorer. Integer alphabets avoid dynamic string
   operations in alignment loops. Every tie follows the versioned algorithm. */
typedef struct {uint32_t*data;size_t n;} Words;
static Words words(V input,V alphabet){V values=iter_values(input);Words w={mem((length(values)+1)*sizeof(uint32_t)),length(values)};for(size_t i=0;i<w.n;++i){V key=item(values,integer(i)),id=map_find(alphabet,key,UNBOUND);if(id==UNBOUND){id=integer(length(alphabet)+1);map_set(alphabet,key,id);}w.data[i]=(uint32_t)asint(id);}return w;}
static V pair_value(size_t a,size_t b){V v=list_new();append(v,integer(a));append(v,integer(b));obj(v)->type=T_TUPLE;return v;}
static V triple_value(size_t a,size_t b,size_t c){V v=pair_value(a,b);append(v,integer(c));return v;}
static V kernel_lcs_length(V a,V b){
    V alphabet=map_new();Words r=words(a,alphabet),h=words(b,alphabet);if(r.n<h.n){Words t=r;r=h;h=t;}size_t nw=(h.n+63)/64,labels=length(alphabet)+1;if(!nw)return integer(0);
    uint64_t*mask=calloc(labels*nw,sizeof(uint64_t)),*state=calloc(nw,sizeof(uint64_t));if(!mask||!state)fail("LCS length allocation");
    for(size_t j=0;j<h.n;++j)mask[(size_t)h.data[j]*nw+j/64]|=UINT64_C(1)<<(j%64);
    for(size_t i=0;i<r.n;++i){uint64_t carry=1,borrow=0;uint64_t*row=mask+(size_t)r.data[i]*nw;
        for(size_t j=0;j<nw;++j){uint64_t old=state[j],x=old|row[j],y=(old<<1)|carry;carry=old>>63;uint64_t d=x-y,b1=x<y,d2=d-borrow,b2=d<borrow;borrow=b1|b2;state[j]=x&~d2;}
    }
    size_t count=0;for(size_t j=0;j<nw;++j){uint64_t x=state[j];x-=((x>>1)&UINT64_C(0x5555555555555555));x=(x&UINT64_C(0x3333333333333333))+((x>>2)&UINT64_C(0x3333333333333333));x=(x+(x>>4))&UINT64_C(0x0f0f0f0f0f0f0f0f);count+=(size_t)((x*UINT64_C(0x0101010101010101))>>56);}
    free(mask);free(state);return integer(count);
}
static V kernel_lcs(V a,V b){
    V alphabet=map_new();Words r=words(a,alphabet),h=words(b,alphabet);size_t n=r.n,m=h.n;
    uint32_t*current=mem((m+1)*sizeof(uint32_t)),*next=mem((m+1)*sizeof(uint32_t));unsigned char*skip=mem((n*m+7)/8+1);
    for(size_t i=n;i-->0;){for(size_t j=m;j-->0;){if(r.data[i]==h.data[j])current[j]=next[j+1]+1;else if(next[j]>=current[j+1]){current[j]=next[j];size_t bit=i*m+j;skip[bit>>3]|=(unsigned char)(1u<<(bit&7));}else current[j]=current[j+1];}uint32_t*tmp=next;next=current;current=tmp;}
    V out=list_new();size_t i=0,j=0;while(i<n&&j<m){if(r.data[i]==h.data[j]){append(out,pair_value(i,j));++i;++j;}else{size_t bit=i*m+j;if(skip[bit>>3]&(1u<<(bit&7)))++i;else ++j;}}return out;
}
/* Large text gaps follow RapidFuzz's affix trimming and bit-matrix backtrack,
   whose tie choices intentionally differ from the small-gap DP above.
   The corresponding MIT notices are in licenses/RapidFuzz.txt. */
static void large_lcs_slice(Words r,Words h,size_t a0,size_t a1,size_t b0,size_t b1,size_t labels,V out){
    size_t prefix=0,suffix=0,n=a1-a0,m=b1-b0;while(prefix<n&&prefix<m&&r.data[a0+prefix]==h.data[b0+prefix])++prefix;
    while(suffix<n-prefix&&suffix<m-prefix&&r.data[a1-suffix-1]==h.data[b1-suffix-1])++suffix;
    for(size_t i=0;i<prefix;++i)append(out,pair_value(a0+i,b0+i));n-=prefix+suffix;m-=prefix+suffix;
    if(n&&m){size_t nw=(n+63)/64;uint64_t*mask=calloc(labels*nw,sizeof(uint64_t)),*state=malloc(nw*sizeof(uint64_t)),*matrix=malloc(m*nw*sizeof(uint64_t));size_t*matched=malloc((n<m?n:m)*2*sizeof(size_t));if(!mask||!state||!matrix||!matched)fail("large LCS allocation");
        for(size_t i=0;i<nw;++i)state[i]=UINT64_MAX;for(size_t i=0;i<n;++i)mask[(size_t)r.data[a0+prefix+i]*nw+i/64]|=UINT64_C(1)<<(i%64);
        for(size_t j=0;j<m;++j){size_t id=h.data[b0+prefix+j];uint64_t carry=0,borrow=0;for(size_t i=0;i<nw;++i){uint64_t old=state[i],u=id<labels?(old&mask[id*nw+i]):0,sum=old+u,c1=sum<old,sum2=sum+carry,c2=sum2<sum,diff=old-u,bx=old<u,diff2=diff-borrow,by=diff<borrow;carry=c1|c2;borrow=bx|by;state[i]=sum2|diff2;}memcpy(matrix+j*nw,state,nw*sizeof(uint64_t));}
        size_t col=n,row=m,count=0;
        while(col&&row){uint64_t bit=UINT64_C(1)<<((col-1)%64);if(matrix[(row-1)*nw+(col-1)/64]&bit)--col;else{--row;if(row&&!(matrix[(row-1)*nw+(col-1)/64]&bit))continue;--col;matched[2*count]=a0+prefix+col;matched[2*count+1]=b0+prefix+row;++count;}}
        for(size_t i=count;i-->0;)append(out,pair_value(matched[2*i],matched[2*i+1]));free(mask);free(state);free(matrix);free(matched);
    }
    for(size_t i=0;i<suffix;++i)append(out,pair_value(a1-suffix+i,b1-suffix+i));
}
static V kernel_large_lcs(V module,V a,V b){V alphabet=map_new();Words r=words(a,alphabet);size_t labels=length(alphabet)+1;Words h=words(b,alphabet);size_t slices=(size_t)ceil(sqrt((double)r.n*h.n/asnum(attr(module,"LCS_SLICE_CELLS"))));if(!slices)slices=1;V out=list_new();for(size_t s=0;s<slices;++s)large_lcs_slice(r,h,r.n*s/slices,r.n*(s+1)/slices,h.n*s/slices,h.n*(s+1)/slices,labels,out);return out;}
typedef struct {uint64_t hash;uint32_t pos;} Seed;
typedef struct {uint32_t i,j,length;double distance;} Tile;
static int seed_compare(const void*aa,const void*bb){const Seed*a=aa,*b=bb;if(a->hash!=b->hash)return a->hash<b->hash?-1:1;return(a->pos>b->pos)-(a->pos<b->pos);}
static int tile_compare(const void*aa,const void*bb){const Tile*a=aa,*b=bb;if(a->length!=b->length)return a->length>b->length?-1:1;if(a->distance!=b->distance)return a->distance<b->distance?-1:1;if(a->i!=b->i)return a->i<b->i?-1:1;return(a->j>b->j)-(a->j<b->j);}
static int occupied(unsigned char*mask,size_t start,size_t n){for(size_t i=0;i<n;++i)if(mask[start+i])return 1;return 0;}
static void seed_hashes(Words w,size_t k,uint64_t*out){uint64_t base=UINT64_C(1000000007),power=1,v=0;for(size_t i=1;i<k;++i)power*=base;for(size_t i=0;i<w.n;++i){if(i>=k)v-=(uint64_t)w.data[i-k]*power;v=v*base+w.data[i];if(i+1>=k)out[i+1-k]=v;}}
static V kernel_tile(V a,V b,V rm,V hm,size_t k){
    V alphabet=map_new();Words r=words(a,alphabet),h=words(b,alphabet);size_t n=r.n,m=h.n;V out=list_new();if(!k||k>n||k>m)return out;
    unsigned char*ur=mem(n+1),*uh=mem(m+1);for(size_t i=0;i<n;++i)ur[i]=(unsigned char)truth(item(rm,integer(i)));for(size_t j=0;j<m;++j)uh[j]=(unsigned char)truth(item(hm,integer(j)));
    uint64_t*rh=mem((n+1)*8),*hh=mem((m+1)*8);Seed*index=mem((m+1)*sizeof(Seed));seed_hashes(r,k,rh);seed_hashes(h,k,hh);
    Tile*candidates=NULL;size_t capacity=0;
    for(;;){size_t ni=0,nc=0;for(size_t j=0;j+k<=m;++j)if(!occupied(uh,j,k))index[ni++]=(Seed){hh[j],(uint32_t)j};qsort(index,ni,sizeof(Seed),seed_compare);
        for(size_t i=0;i+k<=n;++i){if(occupied(ur,i,k))continue;size_t lo=0,hi=ni;while(lo<hi){size_t mid=lo+(hi-lo)/2;if(index[mid].hash<rh[i])lo=mid+1;else hi=mid;}
            for(size_t z=lo;z<ni&&index[z].hash==rh[i];++z){size_t j=index[z].pos;if(memcmp(r.data+i,h.data+j,k*sizeof(uint32_t)))continue;if(i&&j&&r.data[i-1]==h.data[j-1]&&!ur[i-1]&&!uh[j-1])continue;size_t len=k;while(i+len<n&&j+len<m&&r.data[i+len]==h.data[j+len]&&!ur[i+len]&&!uh[j+len])++len;
                if(nc==capacity){capacity=capacity?capacity*2:128;Tile*next=realloc(candidates,capacity*sizeof(Tile));if(!next)fail("tiling allocation");candidates=next;}candidates[nc++]=(Tile){(uint32_t)i,(uint32_t)j,(uint32_t)len,fabs((double)i/n-(double)j/m)};
            }
        }
        if(nc>1)qsort(candidates,nc,sizeof(Tile),tile_compare);int took=0;
        for(size_t z=0;z<nc;++z){Tile c=candidates[z];if(occupied(ur,c.i,c.length)||occupied(uh,c.j,c.length)){if(took)break;continue;}memset(ur+c.i,1,c.length);memset(uh+c.j,1,c.length);append(out,triple_value(c.i,c.j,c.length));took=1;}
        if(!took)break;
    }
    free(candidates);for(size_t i=0;i<n;++i)if(ur[i])item_set(rm,integer(i),TRUEV);for(size_t j=0;j<m;++j)if(uh[j])item_set(hm,integer(j),TRUEV);return out;
}
static int one_edit_native(V a,V b){
    size_t n=unicode_len(a),m=unicode_len(b);if(n>m){V temp=a;a=b;b=temp;size_t z=n;n=m;m=z;}if(m-n>1)return 0;
    size_t pa=0,pb=0;int different=0;while(pa<obj(a)->n){size_t before=pa;uint32_t ca=decode(cstr(a),obj(a)->n,&pa),cb=decode(cstr(b),obj(b)->n,&pb);if(ca!=cb){if(n!=m)pa=before;different=1;break;}}
    if(!different&&n!=m&&pb<obj(b)->n)decode(cstr(b),obj(b)->n,&pb);
    size_t an=obj(a)->n-pa,bn=obj(b)->n-pb;return an==bn&&!memcmp(cstr(a)+pa,cstr(b)+pb,an);
}
static V kernel_window(V phrase,V weights,V hyp,double fuzzy){
    V alphabet=map_new();Words r=words(phrase,alphabet),h=words(hyp,alphabet);size_t n=r.n,m=h.n,stride=m+1;double*dp=mem((n+1)*stride*sizeof(double)),*sim=mem((n*m+1)*sizeof(double));
    for(size_t i=0;i<n;++i)for(size_t j=0;j<m;++j){V a=item(phrase,integer(i)),b=item(hyp,integer(j));sim[i*m+j]=r.data[i]==h.data[j]?1.0:(length(a)>=5&&length(b)>=5&&one_edit_native(a,b)?fuzzy:0.0);}
    for(size_t i=n;i-->0;)for(size_t j=m;j-->0;){double down=dp[(i+1)*stride+j],right=dp[i*stride+j+1],s=sim[i*m+j],diag=s>0?dp[(i+1)*stride+j+1]+asnum(item(weights,integer(i)))*s:-1.0;dp[i*stride+j]=down>right?down:right;if(diag>dp[i*stride+j])dp[i*stride+j]=diag;}
    V hits=list_new();size_t i=0,j=0;while(i<n&&j<m){double s=sim[i*m+j];if(s>0&&dp[i*stride+j]==dp[(i+1)*stride+j+1]+asnum(item(weights,integer(i)))*s){append(hits,integer(j));++i;++j;}else if(dp[(i+1)*stride+j]>=dp[i*stride+j+1])++i;else ++j;}
    V out=list_new();append(out,number(dp[0]));append(out,hits);obj(out)->type=T_TUPLE;return out;
}
typedef struct {V value;size_t position;uint32_t pending;} Utf16;
static int utf16_next(Utf16*it){if(it->pending){int cp=(int)it->pending;it->pending=0;return cp;}if(it->position==obj(it->value)->n)return -1;uint32_t cp=decode(cstr(it->value),obj(it->value)->n,&it->position);if(cp<=65535)return(int)cp;cp-=65536;it->pending=0xdc00+(cp&1023);return(int)(0xd800+(cp>>10));}
static V native_sorted_pair(V a,V b){Utf16 ai={a,0,0},bi={b,0,0};int x,y;do{x=utf16_next(&ai);y=utf16_next(&bi);}while(x==y&&x>=0);V out=list_new();append(out,x<=y?a:b);append(out,x<=y?b:a);obj(out)->type=T_TUPLE;return out;}
static V native_u16_length(V a){size_t p=0,n=0;while(p<obj(a)->n)n+=decode(cstr(a),obj(a)->n,&p)>65535?2:1;return integer(n);}
static int js_space(uint32_t cp){return cp==32||(cp>=9&&cp<=13)||cp==0xa0||cp==0x1680||(cp>=0x2000&&cp<=0x200a)||cp==0x2028||cp==0x2029||cp==0x202f||cp==0x205f||cp==0x3000||cp==0xfeff;}
static V native_empty(V a){char token[5]={0};size_t n=0,p=0;int trailing=0;while(p<obj(a)->n){uint32_t cp=decode(cstr(a),obj(a)->n,&p);if(cp=='$'||cp==0x20ac||cp==0xa3||cp==0xa5)continue;if(js_space(cp)){if(n)trailing=1;continue;}if(trailing||n>=4||cp>=128)return FALSEV;token[n++]=(char)cp;}return !strcmp(token,"")||!strcmp(token,"-")||!strcmp(token,"--")||!strcmp(token,"---")||!strcmp(token,"n/a")||!strcmp(token,"na")||!strcmp(token,"none")||!strcmp(token,"nan")?TRUEV:FALSEV;}
static V kernel_dispatch(const char*name,V receiver,V args,V kw){
    V a=item(args,integer(0));
    if(!strcmp(name,"__c_u16_len"))return native_u16_length(a);
    if(!strcmp(name,"__c_empty"))return native_empty(a);
    V b=item(args,integer(1));
    if(!strcmp(name,"__c_sorted_pair"))return native_sorted_pair(a,b);
    if(!strcmp(name,"__c_lcs_len"))return kernel_lcs_length(a,b);
    if(!strcmp(name,"__c_large_lcs"))return kernel_large_lcs(receiver,a,b);
    if(!strcmp(name,"__c_lcs")){V original=attr(receiver,"__original_lcs");if(length(a)*length(b)>(size_t)asint(attr(receiver,"LCS_CELL_LIMIT")))return call(original,args,kw);return kernel_lcs(a,b);}
    if(!strcmp(name,"__c_tile")){V rm=item(args,integer(2)),hm=item(args,integer(3));size_t k=asint(item(args,integer(4)));V tiles=kernel_tile(a,b,rm,hm,k);if(length(args)==6){extend(item(args,integer(5)),tiles);return NONE;}return tiles;}
    if(!strcmp(name,"__c_window"))return kernel_window(a,b,item(args,integer(2)),asnum(attr(receiver,"FUZZY_TOKEN")));
    fail("unknown kernel %s",name);return NONE;
}
