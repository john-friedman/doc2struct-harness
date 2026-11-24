/* Sparse table-cell matching with typed label IDs, connected components, and
   the scorer's original position tie break and large-component greedy policy. */
typedef struct {size_t g,p;double score,labels;int positional;} TableEdge;
static size_t table_label(V map,V value){V found=map_find(map,value,UNBOUND);if(found!=UNBOUND)return(size_t)asint(found);size_t id=length(map);map_set(map,value,integer(id));return id;}
static size_t table_root(size_t*parent,size_t i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;}
static V table_edge_value(TableEdge edge){V out=list_new();append(out,integer(edge.g));append(out,integer(edge.p));append(out,number(edge.score));append(out,number(edge.labels));append(out,integer(edge.positional));obj(out)->type=T_TUPLE;return out;}
static int table_edge_order(const void*a,const void*b){const TableEdge*x=a,*y=b;if(x->score!=y->score)return x->score>y->score?-1:1;if(x->g!=y->g)return x->g<y->g?-1:1;return x->p<y->p?-1:x->p>y->p;}
static V table_value_native(V module,V args,V kw){
    V a=item(args,integer(0)),b=item(args,integer(1));if(equal(a,b))return constant_float(1.0);
    V digits=attr(module,"nx_digits"),aa[]={a},bb[]={b};V da=call_pos(digits,1,aa),db=call_pos(digits,1,bb);
    if(!equal(da,db)){V footnotes=attr(module,"strip_footnotes"),fa=call_pos(footnotes,1,aa),fb=call_pos(footnotes,1,bb);if(equal(fa,a)&&equal(fb,b))return constant_float(0.0);}
    return call(attr(module,"__original_value_sim"),args,kw);
}
static TripleCache*table_value_info(V module,V x){TripleCache*cache=triple_cache(x);if(!cache->digits){V a=field(x,3),args[]={a};cache->digits=call_pos(attr(module,"nx_digits"),1,args);cache->footnote_free=equal(a,call_pos(attr(module,"strip_footnotes"),1,args));}return cache;}
static double table_value_pair(V module,V x,V y,V key){V a=field(x,3),b=field(y,3);if(equal(a,b))return 1;TripleCache*ac=table_value_info(module,x),*bc=table_value_info(module,y);if(!equal(ac->digits,bc->digits)&&ac->footnote_free&&bc->footnote_free)return 0;V args[]={a,b,key};return asnum(call_pos(attr(module,"__original_value_sim"),3,args));}
typedef struct {double score;int64_t distance;V g,p,pairs;} RowCandidate;
static int row_candidate_order(const void*a,const void*b){const RowCandidate*x=a,*y=b;if(x->score!=y->score)return x->score<y->score?-1:1;if(x->distance!=y->distance)return x->distance<y->distance?-1:1;int c=ordering(x->g,y->g);return c?c:ordering(x->p,y->p);}
static V table_row_groups(V triples,size_t table_field){V groups=map_new();for(size_t i=0;i<length(triples);++i){V x=item(triples,integer(i)),key=pair_value(asint(field(x,table_field)),asint(field(x,4))),row=map_find(groups,key,UNBOUND);if(row==UNBOUND){row=list_new();map_set(groups,key,row);}append(row,integer(i));}return groups;}
static V table_pair_rows(V module,V G,V P,V matches){
    V grows=table_row_groups(G,7),prows=table_row_groups(P,8),gkeys=map_keys(grows),pkeys=map_keys(prows),linked=map_new(),ghas=map_new(),rks=map_new();size_t ng=length(G),np=length(P);unsigned char*gu=calloc(ng+1,1),*pu=calloc(np+1,1);if(!gu||!pu)fail("row pairing allocation");
    for(size_t i=0;i<length(matches);++i){V mt=item(matches,integer(i));size_t g=asint(item(mt,integer(0))),p=asint(item(mt,integer(1)));gu[g]=pu[p]=1;V gi=field(item(G,integer(g)),7),ti=field(item(P,integer(p)),8);map_set(ghas,gi,TRUEV);map_set(linked,pair_value(asint(gi),asint(ti)),TRUEV);}
    V norm=attr(module,"nx_norm"),vs=attr(module,"value_sim"),open_g=list_new(),open_p=list_new();
    for(size_t i=0;i<length(gkeys);++i){V key=item(gkeys,integer(i)),row=map_find(grows,key,NONE);int used=0;for(size_t j=0;j<length(row);++j)used|=gu[asint(item(row,integer(j)))];if(!used)append(open_g,key);}
    for(size_t i=0;i<ng;++i){V args[]={field(item(G,integer(i)),1)};map_set(rks,call_pos(norm,1,args),TRUEV);}
    for(size_t i=0;i<length(pkeys);++i){V key=item(pkeys,integer(i)),row=map_find(prows,key,NONE);int used=0;for(size_t j=0;j<length(row);++j)used|=pu[asint(item(row,integer(j)))];V args[]={field(item(P,item(row,integer(0))),1)};if(!used&&!contains(rks,call_pos(norm,1,args)))append(open_p,key);}
    free(gu);free(pu);RowCandidate*candidates=NULL;size_t count=0,capacity=0;
    for(size_t g=0;g<length(open_g);++g){V gk=item(open_g,integer(g)),gidx=map_find(grows,gk,NONE);for(size_t p=0;p<length(open_p);++p){V pk=item(open_p,integer(p));if(contains(ghas,item(gk,integer(0)))&&!contains(linked,pair_value(asint(item(gk,integer(0))),asint(item(pk,integer(0))))))continue;V pidx=map_find(prows,pk,NONE),pairs=list_new();
            for(size_t i=0;i<length(gidx);++i){V gi=item(gidx,integer(i)),x=item(G,gi);for(size_t j=0;j<length(pidx);++j){V pj=item(pidx,integer(j)),y=item(P,pj);if(!equal(field(x,2),field(y,2)))continue;if(table_value_pair(module,x,y,table_pct_native(x))==1){append(pairs,pair_value(asint(gi),asint(pj)));break;}}}
            size_t n=length(pairs),gn=length(gidx),pn=length(pidx);if(n>=2&&n>=.8*gn&&n>=.8*pn){if(count==capacity){capacity=capacity?capacity*2:64;RowCandidate*next=realloc(candidates,capacity*sizeof(*next));if(!next)fail("row candidates allocation");candidates=next;}candidates[count++]=(RowCandidate){-(double)n/(gn>pn?gn:pn),llabs(asint(item(gk,integer(1)))-asint(item(pk,integer(1)))),gk,pk,pairs};}
    }}
    qsort(candidates,count,sizeof(*candidates),row_candidate_order);V taken_g=map_new(),taken_p=map_new(),added=map_new();obj(added)->type=T_SET;
    for(size_t i=0;i<count;++i){RowCandidate c=candidates[i];if(contains(taken_g,c.g)||contains(taken_p,c.p))continue;map_set(taken_g,c.g,TRUEV);map_set(taken_p,c.p,TRUEV);for(size_t j=0;j<length(c.pairs);++j){V pair=item(c.pairs,integer(j));append(matches,table_edge_value((TableEdge){asint(item(pair,integer(0))),asint(item(pair,integer(1))),.5,.5,0}));map_set(added,pair,TRUEV);}}
    free(candidates);return added;
}
static V table_matches(V module,V G,V P,V pc,V PT,int trm,V inv){
    size_t ng=length(G),np=length(P);V out=list_new();if(!ng||!np)return out;
    V glabels=map_new(),plabels=map_new();size_t*ga=mem(ng*3*sizeof(size_t)),*gb=ga+ng,*gr=gb+ng,*pa=mem(np*3*sizeof(size_t)),*pb=pa+np,*pr=pb+np;
    for(size_t i=0;i<ng;++i){V x=item(G,integer(i)),k=field(x,0);ga[i]=table_label(glabels,item(k,integer(0)));gb[i]=table_label(glabels,item(k,integer(1)));if(trm)gr[i]=table_label(glabels,field(x,1));}
    for(size_t j=0;j<np;++j){V k=item(pc,integer(j));pa[j]=table_label(plabels,item(k,integer(0)));pb[j]=table_label(plabels,item(k,integer(1)));if(trm){V rk=field(item(P,integer(j)),1);pr[j]=table_label(plabels,map_find(inv,rk,rk));}}
    size_t gn=length(glabels),pn=length(plabels);double*sim=malloc(gn*pn*sizeof(double));if(!sim)fail("table similarities allocation");V gkeys=map_keys(glabels),pkeys=map_keys(plabels),keysim=attr(module,"key_sim"),pct_key=attr(module,"_pct_key"),digits=attr(module,"nx_digits");
    V*gdigits=mem(gn*sizeof(V)),*pdigits=mem(pn*sizeof(V));for(size_t i=0;i<gn;++i){V args[]={item(gkeys,integer(i))};gdigits[i]=call_pos(digits,1,args);}for(size_t j=0;j<pn;++j){V args[]={item(pkeys,integer(j))};pdigits[j]=call_pos(digits,1,args);}
    for(size_t i=0;i<gn;++i)for(size_t j=0;j<pn;++j){V a=item(gkeys,integer(i)),b=item(pkeys,integer(j));if(equal(a,b))sim[i*pn+j]=1;else if(!equal(gdigits[i],pdigits[j]))sim[i*pn+j]=0;else{V args[]={a,b};sim[i*pn+j]=asnum(call_pos(keysim,2,args));}}
    TableEdge*edges=NULL;size_t ne=0,capacity=0;size_t*parent=mem((ng+np)*sizeof(size_t));for(size_t i=0;i<ng+np;++i)parent[i]=i;
    for(size_t i=0;i<ng;++i){V x=item(G,integer(i)),args[]={x};V key=NONE;double*sa=sim+ga[i]*pn,*sb=sim+gb[i]*pn;
        for(size_t j=0;j<np;++j){V y=item(P,integer(j));double direct=sa[pa[j]]*sb[pb[j]],swap=sa[pb[j]]*sb[pa[j]],kn=direct>swap?direct:swap,pos=0;
            if(trm&&kn==0&&equal(field(x,2),field(y,2))&&!truth(attr(field(item(PT,field(y,8)),1),"transposed")))pos=sim[gr[i]*pn+pr[j]];
            double ks=kn>pos?kn:pos;if(ks<=0)continue;if(key==NONE)key=call_pos(pct_key,1,args);double score=ks*table_value_pair(module,x,y,key);if(score<=0)continue;
            if(ne==capacity){capacity=capacity?capacity*2:256;TableEdge*next=realloc(edges,capacity*sizeof(*edges));if(!next)fail("table edges allocation");edges=next;}edges[ne++]=(TableEdge){i,j,score,ks,pos>kn};size_t a=table_root(parent,i),b=table_root(parent,ng+j);if(a!=b)parent[a]=b;
        }
    }
    free(sim);if(!ne){free(edges);return out;}
    size_t*head=mem((ng+np)*sizeof(size_t)),*tail=mem((ng+np)*sizeof(size_t)),*next=mem(ne*sizeof(size_t));for(size_t i=0;i<ng+np;++i)head[i]=tail[i]=SIZE_MAX;
    for(size_t e=0;e<ne;++e){size_t root=table_root(parent,edges[e].g);next[e]=SIZE_MAX;if(head[root]==SIZE_MAX)head[root]=e;else next[tail[root]]=e;tail[root]=e;}
    unsigned char*gseen=calloc(ng,1),*pseen=calloc(np,1);size_t*gi=malloc(ng*sizeof(size_t)),*pj=malloc(np*sizeof(size_t)),*gix=malloc(ng*sizeof(size_t)),*pix=malloc(np*sizeof(size_t));if(!gseen||!pseen||!gi||!pj||!gix||!pix)fail("table components allocation");
    size_t limit=(size_t)asint(attr(module,"BIG_COMPONENT"));
    for(size_t root=0;root<ng+np;++root){size_t first=head[root];if(first==SIZE_MAX)continue;if(next[first]==SIZE_MAX){append(out,table_edge_value(edges[first]));continue;}
        memset(gseen,0,ng);memset(pseen,0,np);size_t n=0,m=0,count=0;for(size_t e=first;e!=SIZE_MAX;e=next[e]){gseen[edges[e].g]=pseen[edges[e].p]=1;++count;}
        for(size_t i=0;i<ng;++i)if(gseen[i]){gix[i]=n;gi[n++]=i;}for(size_t j=0;j<np;++j)if(pseen[j]){pix[j]=m;pj[m++]=j;}
        if(n>limit||m>limit){TableEdge*sorted=malloc(count*sizeof(*sorted));size_t z=0;for(size_t e=first;e!=SIZE_MAX;e=next[e])sorted[z++]=edges[e];qsort(sorted,count,sizeof(*sorted),table_edge_order);memset(gseen,0,ng);memset(pseen,0,np);for(size_t e=0;e<count;++e){TableEdge edge=sorted[e];if(!gseen[edge.g]&&!pseen[edge.p]){gseen[edge.g]=pseen[edge.p]=1;append(out,table_edge_value(edge));}}free(sorted);continue;}
        V cost=array_new(n,m,2);size_t*by_pair=malloc(n*m*sizeof(size_t));if(!by_pair)fail("table assignment allocation");for(size_t i=0;i<n;++i)for(size_t j=0;j<m;++j){double d=(double)i/n-(double)j/m;((double*)obj(cost)->ptr)[i*m+j]=1+1e-10*d*d;by_pair[i*m+j]=SIZE_MAX;}
        for(size_t e=first;e!=SIZE_MAX;e=next[e]){TableEdge edge=edges[e];size_t i=gix[edge.g],j=pix[edge.p];double d=(double)i/n-(double)j/m;((double*)obj(cost)->ptr)[i*m+j]=1-edge.score+1e-10*d*d;by_pair[i*m+j]=e;}
        V chosen=assignment(cost),rows=item(chosen,integer(0)),cols=item(chosen,integer(1));for(size_t i=0;i<length(rows);++i){size_t e=by_pair[asint(item(rows,integer(i)))*m+asint(item(cols,integer(i)))];if(e!=SIZE_MAX)append(out,table_edge_value(edges[e]));}free(by_pair);
    }
    free(edges);free(gseen);free(pseen);free(gi);free(pj);free(gix);free(pix);return out;
}
