/* Typed form-caption matching. Each output builds its token index and window
   boundaries once; DP scratch space is reused across all candidate windows. */
typedef struct {size_t*bound;V index;} FormIndex;
typedef struct {double weight;int64_t first,last;} FormMatch;
typedef struct {int64_t start,end;double q;} FormCandidate;
static V field(V record,size_t index){return obj(record)->values[index];}
static FormIndex*form_index(V out){
    if(obj(out)->ptr)return obj(out)->ptr;
    FormIndex*index=mem(sizeof(*index));V cells=field(out,2),tokens=field(out,0);size_t n=length(cells);
    index->bound=mem((n+1)*sizeof(size_t));index->index=map_new();
    for(size_t i=n;i-->0;){V c=item(cells,integer(i));size_t end=(size_t)(asint(field(c,9))+1);
        if(asint(field(c,2))<0&&i+1<n&&asint(field(item(cells,integer(i+1)),2))<0)end=index->bound[i+1];index->bound[i]=end;
    }
    for(size_t i=0;i<length(tokens);++i){V token=item(tokens,integer(i));if(truth(field(token,3)))continue;V key=field(token,0),positions=map_find(index->index,key,UNBOUND);if(positions==UNBOUND){positions=list_new();map_set(index->index,key,positions);}append(positions,integer(i));}
    obj(out)->ptr=index;return index;
}
static FormMatch form_window(V*phrase,const double*weights,size_t n,V*window,size_t m,double fuzzy,double*dp,double*sim){
    size_t stride=m+1;memset(dp+n*stride,0,stride*sizeof(double));
    for(size_t i=n;i-->0;){dp[i*stride+m]=0;for(size_t j=m;j-->0;){
        V a=phrase[i],b=window[j];double s=equal(a,b)?1.0:(length(a)>=5&&length(b)>=5&&one_edit_native(a,b)?fuzzy:0.0);sim[i*m+j]=s;
        double down=dp[(i+1)*stride+j],right=dp[i*stride+j+1],diag=s>0?dp[(i+1)*stride+j+1]+weights[i]*s:-1;
        dp[i*stride+j]=down>right?down:right;if(diag>dp[i*stride+j])dp[i*stride+j]=diag;
    }}
    FormMatch result={dp[0],-1,-1};size_t i=0,j=0;
    while(i<n&&j<m){double s=sim[i*m+j];if(s>0&&dp[i*stride+j]==dp[(i+1)*stride+j+1]+weights[i]*s){if(result.first<0)result.first=(int64_t)j;result.last=(int64_t)j;++i;++j;}else if(dp[(i+1)*stride+j]>=dp[i*stride+j+1])++i;else ++j;}
    return result;
}
static int compare_size(const void*a,const void*b){size_t x=*(const size_t*)a,y=*(const size_t*)b;return x<y?-1:x>y;}
static int compare_candidate_quality(const void*a,const void*b){const FormCandidate*x=a,*y=b;if(x->q!=y->q)return x->q>y->q?-1:1;int64_t xl=x->end-x->start,yl=y->end-y->start;if(xl!=yl)return xl<yl?-1:1;return x->start<y->start?-1:x->start>y->start;}
static int compare_candidate_start(const void*a,const void*b){const FormCandidate*x=a,*y=b;return x->start<y->start?-1:x->start>y->start;}
static V form_occurrence(V cls,V item_index,FormCandidate candidate){V out=fresh(T_INSTANCE);Obj*o=obj(out);o->a=cls;o->slots=4;o->values=mem(4*sizeof(V));o->values[0]=item_index;o->values[1]=integer(candidate.start);o->values[2]=integer(candidate.end);o->values[3]=number(candidate.q);return out;}
static V form_candidates(V module,V item_index,V phrase,V out,V index,int at_start){
    V result=list_new();size_t n=length(phrase);if(!n)return result;
    V cells=field(out,2),tokens=field(out,0),tcell=field(out,1);FormIndex*fi=form_index(out);
    size_t nt=length(tokens),maxm=n+(size_t)asint(attr(module,"WINDOW_SLACK"));
    double*weights=malloc(n*sizeof(double));size_t*starts=malloc((nt+1)*sizeof(size_t));unsigned char*seen=calloc(nt+1,1);
    double*scratch=malloc(((n+1)*(maxm+1)+n*maxm)*sizeof(double));V*window=malloc((maxm+1)*sizeof(V));FormCandidate*candidates=malloc((nt+1)*sizeof(FormCandidate));
    if(!weights||!starts||!seen||!scratch||!window||!candidates)fail("form matching allocation");
    double hi=0,lo=0;int strong=0;
    for(size_t i=0;i<n;++i){V key=item(phrase,integer(i));size_t p=0;uint32_t cp=decode(cstr(key),obj(key)->n,&p);weights[i]=length(key)<=2&&!truth(forms_digits(key))&&!form_cjk(cp)?asnum(attr(module,"WEAK_WEIGHT")):1.0;strong|=weights[i]==1.0;double t=hi+weights[i];lo+=fabs(hi)>=fabs(weights[i])?(hi-t)+weights[i]:(weights[i]-t)+hi;hi=t;}
    double total=hi+lo;size_t ns=0;
    for(size_t i=0;i<n;++i){if(strong&&weights[i]!=1.0)continue;V positions=map_find(index,item(phrase,integer(i)),NONE);if(positions==NONE)continue;
        for(size_t j=0;j<length(positions);++j){size_t pos=(size_t)asint(item(positions,integer(j))),cell=(size_t)asint(item(tcell,integer(pos)));int64_t s=(int64_t)pos-(int64_t)i,first=asint(field(item(cells,integer(cell)),8));if(s<first)s=first;if(s>=0&&(size_t)s<nt&&!seen[s]){seen[s]=1;starts[ns++]=(size_t)s;}}
    }
    qsort(starts,ns,sizeof(*starts),compare_size);size_t nc=0;double fuzzy=asnum(attr(module,"FUZZY_TOKEN")),threshold=asnum(attr(module,"LOCATE_Q"))-1e-9;
    V matches=map_new();Obj window_key={0};window_key.type=T_TUPLE;window_key.values=window;
    for(size_t i=0;i<ns;++i){size_t s=starts[i],e=s+maxm,bound=fi->bound[asint(item(tcell,integer(s)))];if(e>bound)e=bound;size_t m=e>s?e-s:0;
        for(size_t j=0;j<m;++j){V t=item(tokens,integer(s+j));window[j]=truth(field(t,3))?S("\001"):field(t,0);}window_key.n=m;window_key.hash=0;
        V saved=map_find(matches,(V)&window_key,UNBOUND);FormMatch match;
        if(saved==UNBOUND){match=form_window(obj(phrase)->values,weights,n,window,m,fuzzy,scratch,scratch+(n+1)*(maxm+1));V key=list_new();for(size_t j=0;j<m;++j)append(key,window[j]);obj(key)->type=T_TUPLE;V val=list_new();append(val,number(match.weight));append(val,integer(match.first));append(val,integer(match.last));map_set(matches,key,val);}
        else{match.weight=asnum(item(saved,integer(0)));match.first=asint(item(saved,integer(1)));match.last=asint(item(saved,integer(2)));}
        double q=total?match.weight/total:0;
        if(match.first<0||q<threshold)continue;
        if(at_start){size_t pos=s+(size_t)match.first;V cell=item(cells,item(tcell,integer(pos)));int ok=1;for(size_t j=(size_t)asint(field(cell,8));j<pos;++j)if(!truth(field(item(tokens,integer(j)),3))){ok=0;break;}if(!ok)continue;}
        candidates[nc++]=(FormCandidate){(int64_t)s+match.first,(int64_t)s+match.last,q};
    }
    qsort(candidates,nc,sizeof(*candidates),compare_candidate_quality);size_t kept=0;
    for(size_t i=0;i<nc;++i){FormCandidate c=candidates[i];int ok=1;for(size_t j=0;j<kept;++j)if(c.end>=candidates[j].start&&c.start<=candidates[j].end){ok=0;break;}if(ok)candidates[kept++]=c;}
    qsort(candidates,kept,sizeof(*candidates),compare_candidate_start);V cls=attr(module,"Occ");for(size_t i=0;i<kept;++i)append(result,form_occurrence(cls,item_index,candidates[i]));
    free(weights);free(starts);free(seen);free(scratch);free(window);free(candidates);return result;
}
static V form_copies(V module,V ref,V out,V x,V placed,V used){
    V it=item(field(ref,0),x),args[]={attr(it,"label")};V full=item(call_pos(attr(module,"_phrase"),1,args),integer(0));
    V cands=form_candidates(module,x,full,out,form_index(out)->index,0),result=list_new();
    for(size_t i=0;i<length(cands);++i){V o=item(cands,integer(i));if(asnum(field(o,3))<1-1e-9)continue;int64_t start=asint(field(o,1)),end=asint(field(o,2));if(start==asint(field(placed,1))&&end==asint(field(placed,2)))continue;
        int ok=1;for(int64_t j=start;j<=end;++j)if(truth(item(used,integer(j)))){ok=0;break;}if(!ok)continue;
        V a[]={out,o,used,integer(length(full))};if(truth(call_pos(attr(module,"_field_line"),4,a)))append(result,o);
    }return result;
}
