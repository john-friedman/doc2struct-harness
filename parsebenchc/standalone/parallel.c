/* Native directory input, worker scheduling, JSON output and aggregation. */
#ifndef _WIN32
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#endif
typedef struct {char*data;size_t n,cap;} Buffer;
static void buffer_add(Buffer*b,const char*s,size_t n){if(b->n+n+1>b->cap){b->cap=(b->n+n+1)*2+256;char*p=realloc(b->data,b->cap);if(!p)fail("output allocation");b->data=p;}memcpy(b->data+b->n,s,n);b->n+=n;b->data[b->n]=0;}
static void buffer_text(Buffer*b,const char*s){buffer_add(b,s,strlen(s));}
static void buffer_quote(Buffer*b,V value){buffer_text(b,"\"");const char*s=cstr(value);for(size_t i=0;i<obj(value)->n;++i){unsigned char c=s[i];if(c=='"'||c=='\\'){buffer_text(b,"\\");buffer_add(b,(char*)&c,1);}else if(c<32){char escape[8];snprintf(escape,sizeof(escape),"\\u%04x",c);buffer_text(b,escape);}else buffer_add(b,(char*)&c,1);}buffer_text(b,"\"");}
static void buffer_json(Buffer*b,V value){int t=type(value);char num[64];if(t==0)buffer_text(b,"null");else if(t==-2)buffer_text(b,truth(value)?"true":"false");else if(t==-1){snprintf(num,sizeof(num),"%lld",(long long)asint(value));buffer_text(b,num);}else if(t==T_FLOAT){if(!isfinite(asnum(value)))buffer_text(b,"null");else{snprintf(num,sizeof(num),"%.17g",asnum(value));buffer_text(b,num);}}else if(t==T_STR)buffer_quote(b,value);else if(t==T_MAP){V keys=map_keys(value);buffer_text(b,"{");for(size_t i=0;i<length(keys);++i){if(i)buffer_text(b,",");V key=item(keys,integer(i));buffer_quote(b,key);buffer_text(b,":");buffer_json(b,map_find(value,key,NONE));}buffer_text(b,"}");}else if(t==T_LIST||t==T_TUPLE){buffer_text(b,"[");for(size_t i=0;i<length(value);++i){if(i)buffer_text(b,",");buffer_json(b,item(value,integer(i)));}buffer_text(b,"]");}else if(t==T_INSTANCE)buffer_json(b,instance_map(value));else buffer_text(b,"null");}
static char*heap_copy(const char*s){size_t n=strlen(s);char*out=malloc(n+1);if(!out)fail("path allocation");memcpy(out,s,n+1);return out;}
#ifdef _WIN32
static wchar_t*wide_path(const char*path){int n=MultiByteToWideChar(CP_UTF8,0,path,-1,NULL,0);wchar_t*out=malloc((size_t)n*sizeof(wchar_t));if(!out)fail("path allocation");MultiByteToWideChar(CP_UTF8,0,path,-1,out,n);return out;}
static FILE*open_path(const char*path,const char*mode){wchar_t*w=wide_path(path),*m=wide_path(mode);DWORD length=GetFullPathNameW(w,0,NULL,NULL);wchar_t*absolute=malloc(((size_t)length+1)*sizeof(wchar_t));if(!absolute)fail("path allocation");GetFullPathNameW(w,length+1,absolute,NULL);size_t n=wcslen(absolute);wchar_t*extended=malloc((n+9)*sizeof(wchar_t));if(!extended)fail("path allocation");if(absolute[0]==L'\\'&&absolute[1]==L'\\')swprintf(extended,n+9,L"\\\\?\\UNC\\%s",absolute+2);else swprintf(extended,n+9,L"\\\\?\\%s",absolute);FILE*f=_wfopen(extended,m);free(extended);free(absolute);free(w);free(m);return f;}
#else
static FILE*open_path(const char*path,const char*mode){return fopen(path,mode);}
#endif
static char*load_path(const char*path,size_t*size){FILE*f=open_path(path,"rb");if(!f)fail("cannot open %s",path);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char*src=malloc((size_t)n+1);if(!src)fail("input allocation");*size=fread(src,1,n,f);src[*size]=0;fclose(f);return src;}
typedef struct {char*path;const char*json;size_t length;char*record;char*identifier;char subset[96];unsigned present;double scores[4];int failed,pad_failure;} Job;
static Job*jobs;static size_t job_count,job_capacity;static const char*reference_dir;
#ifdef _WIN32
static volatile LONG64 next_job=-1;
#else
static _Atomic size_t next_job;
#endif
static void add_job(const char*path,const char*json,size_t n){if(job_count==job_capacity){job_capacity=job_capacity?job_capacity*2:256;Job*next=realloc(jobs,job_capacity*sizeof(Job));if(!next)fail("jobs allocation");jobs=next;}Job*j=jobs+job_count++;memset(j,0,sizeof(*j));j->path=path?heap_copy(path):NULL;j->json=json;j->length=n;}
#ifdef _WIN32
static void discover(const char*directory){
    size_t n=strlen(directory);char*pattern=malloc(n+4);sprintf(pattern,"%s/*",directory);wchar_t*w=wide_path(pattern);WIN32_FIND_DATAW data;HANDLE h=FindFirstFileW(w,&data);free(pattern);free(w);if(h==INVALID_HANDLE_VALUE)fail("cannot scan %s",directory);
    do{if(data.cFileName[0]==L'.')continue;int len=WideCharToMultiByte(CP_UTF8,0,data.cFileName,-1,NULL,0,NULL,NULL);char*name=malloc(len);WideCharToMultiByte(CP_UTF8,0,data.cFileName,-1,name,len,NULL,NULL);char*path=malloc(n+strlen(name)+2);sprintf(path,"%s/%s",directory,name);
        if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){if(strcmp(name,"references"))discover(path);}else{size_t size=strlen(name);if(size>12&&!strcmp(name+size-12,".result.json"))add_job(path,NULL,0);}free(name);free(path);
    }while(FindNextFileW(h,&data));FindClose(h);
}
#else
#include <dirent.h>
#include <sys/stat.h>
static void discover(const char*directory){DIR*d=opendir(directory);if(!d)fail("cannot scan %s",directory);struct dirent*entry;while((entry=readdir(d))){if(entry->d_name[0]=='.')continue;size_t n=strlen(directory)+strlen(entry->d_name)+2;char*path=malloc(n);snprintf(path,n,"%s/%s",directory,entry->d_name);struct stat st;if(!stat(path,&st)){if(S_ISDIR(st.st_mode)){if(strcmp(entry->d_name,"references"))discover(path);}else{size_t z=strlen(path);if(z>12&&!strcmp(path+z-12,".result.json"))add_job(path,NULL,0);}}free(path);}closedir(d);}
#endif
static int path_order(const void*a,const void*b){return strcmp(((const Job*)a)->path,((const Job*)b)->path);}
static void score_job(Job*j){
    arena_reset();runtime_init();Trap outer;trap_push(&outer);
    if(!setjmp(outer.jump)){
        V input;char*raw=NULL;
        if(j->path){size_t n;raw=load_path(j->path,&n);input=json_parse(raw,n,NULL);free(raw);}else input=json_parse(j->json,j->length,NULL);
        V metrics,id;
        if(reference_dir){V request=map_find(input,S("request"),map_new());id=map_find(request,S("example_id"),NONE);if(type(id)!=T_STR)fail("result has no request.example_id");j->identifier=heap_copy(cstr(id));const char*slash=strchr(cstr(id),'/');size_t sn=slash?(size_t)(slash-cstr(id)):obj(id)->n;if(sn>=sizeof(j->subset))sn=sizeof(j->subset)-1;memcpy(j->subset,cstr(id),sn);
            size_t n=strlen(reference_dir)+obj(id)->n+13;char*path=mem(n);snprintf(path,n,"%s/%s.test.json",reference_dir,cstr(id));size_t size;raw=load_path(path,&size);V testcase=json_parse(raw,size,NULL);free(raw);metrics=score_document(testcase,input);
        }else{id=map_find(input,S("id"),NONE);metrics=score_pair(input);}
        V record=map_new();if(id!=NONE){map_set(record,S("test_id"),id);const char*slash=strchr(cstr(id),'/');size_t len=slash?(size_t)(slash-cstr(id)):obj(id)->n;if(len>=sizeof(j->subset))len=sizeof(j->subset)-1;memcpy(j->subset,cstr(id),len);}
        if(reference_dir&&!length(metrics)){j->pad_failure=1;fail("No extended metric applies to this output");}
        map_set(record,S("metrics"),metrics);map_set(record,S("success"),TRUEV);
        const char*names[]={"text_next","formatting_v2","table_next","forms_v2"};for(int i=0;i<4;++i){V score=map_find(metrics,str(names[i]),NONE);if(score!=NONE){j->present|=1u<<i;j->scores[i]=asnum(score);}}
        Buffer buffer={0};buffer_json(&buffer,record);j->record=buffer.data;
        trap_pop(&outer);
    }else{trap_pop(&outer);j->failed=1;V record=map_new();if(j->identifier)map_set(record,S("test_id"),str(j->identifier));map_set(record,S("success"),FALSEV);map_set(record,S("path"),str(j->path?j->path:"jsonl"));map_set(record,S("error"),str(error_message));Buffer b={0};buffer_json(&b,record);j->record=b.data;}
}
#ifdef _WIN32
static DWORD WINAPI worker_main(void*unused){
    (void)unused;profile_allocations=getenv("PARSEBENCHC_PROFILE")!=NULL;int trace=getenv("PARSEBENCHC_TRACE")!=NULL;
    for(;;){LONG64 index=InterlockedIncrement64(&next_job);if(index<0||(size_t)index>=job_count)break;ULONGLONG began=GetTickCount64();if(trace)fprintf(stderr,"START %lld %s\n",(long long)index,jobs[index].path?jobs[index].path:"jsonl");score_job(jobs+index);if(trace)fprintf(stderr,"DONE %lld %.3f %s\n",(long long)index,(GetTickCount64()-began)/1000.0,jobs[index].path?jobs[index].path:"jsonl");}
    if(profile_allocations){LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);for(int i=0;i<30;++i){size_t best=0;for(size_t j=1;j<2048;++j)if(allocation_stats[j].ticks>allocation_stats[best].ticks)best=j;if(allocation_stats[best].ticks<=0)break;AllocationStat*s=allocation_stats+best;fprintf(stderr,"CPU: %-30s %8.4fs %8.1f MB %llu calls\n",s->name?s->name:"runtime",(double)s->ticks/frequency.QuadPart,s->bytes/1048576.0,(unsigned long long)s->calls);s->ticks=0;}}
    for(Block*b=blocks;b;){Block*next=b->next;free(b);b=next;}blocks=current=NULL;return 0;
}
#endif
#ifndef _WIN32
static void*posix_worker(void*unused){
    (void)unused;for(;;){size_t index=atomic_fetch_add(&next_job,1);if(index>=job_count)break;score_job(jobs+index);}
    for(Block*b=blocks;b;){Block*next=b->next;free(b);b=next;}blocks=current=NULL;return NULL;
}
#endif
typedef struct {const char*subset;int metric;double weight;int dimension;} ScoreSlot;
static const ScoreSlot score_slots[]={
    {"text",0,1,0},{"text",1,.75,1},{"headings",1,.125,1},{"strikeout",1,.0625,1},{"underline",1,.0625,1},
    {"tables",2,.5,2},{"complex_table_headers",2,.125,2},{"financial_tables",2,.125,2},{"insurance_filing_tables",2,.125,2},{"dense_tables",2,.125,2},
    {"forms",3,.875,3},{"fillable_forms",3,.125,3}
};
static void report_run(FILE*out,double seconds){
    const char*dim_names[]={"text","formatting","tables","forms"};double dimensions[4]={0};int missing[4]={0};
    for(size_t s=0;s<sizeof(score_slots)/sizeof(*score_slots);++s){ScoreSlot slot=score_slots[s];double hi=0,lo=0;size_t count=0,observed=0;
        for(size_t j=0;j<job_count;++j)if(!strcmp(jobs[j].subset,slot.subset)){if(jobs[j].pad_failure){++count;continue;}if(!jobs[j].failed&&(jobs[j].present&(1u<<slot.metric))){double x=jobs[j].scores[slot.metric],t=hi+x;lo+=fabs(hi)>=fabs(x)?(hi-t)+x:(x-t)+hi;hi=t;++count;++observed;}}
        if(!observed)missing[slot.dimension]=1;else dimensions[slot.dimension]+=((hi+lo)/count)*slot.weight;
    }
    size_t failures=0;for(size_t i=0;i<job_count;++i)failures+=jobs[i].failed;
    fprintf(out,"{\"documents\":%zu,\"failures\":%zu,\"seconds\":%.6f,\"dimensions\":{",job_count,failures,seconds);
    for(int i=0;i<4;++i){if(i)fputc(',',out);fprintf(out,"\"%s\":",dim_names[i]);if(missing[i])fputs("null",out);else fprintf(out,"%.17g",dimensions[i]);}
    fputs(",\"charts\":null,\"layout\":null},\"next_score\":null,\"missing_dimensions\":[\"charts\",\"layout\"",out);for(int i=0;i<4;++i)if(missing[i])fprintf(out,",\"%s\"",dim_names[i]);fputs("],\"per_example_results\":[",out);for(size_t i=0;i<job_count;++i){if(i)fputc(',',out);fputs(jobs[i].record,out);}fputs("]}\n",out);
}
static int native_main(int argc,char**argv){
    const char*usage="usage: parsebenchc score RESULTS_DIR REFERENCES_DIR OUTPUT.json [WORKERS]\n       parsebenchc pairs INPUT.jsonl [OUTPUT.jsonl] [WORKERS]\n";
    if(argc==2&&(!strcmp(argv[1],"--help")||!strcmp(argv[1],"-h"))){fputs(usage,stdout);return 0;}
    if(argc<3||(strcmp(argv[1],"score")&&strcmp(argv[1],"pairs"))){fputs(usage,stderr);return 2;}
    regex_load();regex_is_loaded=1;char*input_bytes=NULL;int native_dir=!strcmp(argv[1],"score");const char*outpath=NULL;int workers=1;
    if(native_dir){if(argc<5)return 2;reference_dir=argv[3];outpath=argv[4];
#ifdef _WIN32
        workers=argc>5?atoi(argv[5]):(int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
        workers=argc>5?atoi(argv[5]):(int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
        discover(argv[2]);qsort(jobs,job_count,sizeof(Job),path_order);}
    else{size_t size;input_bytes=load_path(argv[2],&size);outpath=argc>3?argv[3]:NULL;workers=argc>4?atoi(argv[4]):1;size_t p=0;while(p<size){size_t start=p;while(p<size&&input_bytes[p]!='\n')++p;if(p>start)add_job(NULL,input_bytes+start,p-start);if(p<size)++p;}}
    if(workers<1)workers=1;if(workers>32)workers=32;if((size_t)workers>job_count)workers=(int)job_count;
#ifdef _WIN32
    LARGE_INTEGER frequency,start,stop;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    HANDLE threads[32];for(int i=0;i<workers;++i){threads[i]=CreateThread(NULL,0,worker_main,NULL,0,NULL);if(!threads[i])fail("worker creation");}if(workers)WaitForMultipleObjects(workers,threads,TRUE,INFINITE);for(int i=0;i<workers;++i)CloseHandle(threads[i]);QueryPerformanceCounter(&stop);double seconds=(double)(stop.QuadPart-start.QuadPart)/frequency.QuadPart;
#else
    struct timespec start,stop;clock_gettime(CLOCK_MONOTONIC,&start);pthread_t threads[32];pthread_attr_t attributes;pthread_attr_init(&attributes);pthread_attr_setstacksize(&attributes,64u<<20);
    for(int i=0;i<workers;++i)if(pthread_create(threads+i,&attributes,posix_worker,NULL))fail("worker creation");for(int i=0;i<workers;++i)pthread_join(threads[i],NULL);pthread_attr_destroy(&attributes);clock_gettime(CLOCK_MONOTONIC,&stop);double seconds=(double)(stop.tv_sec-start.tv_sec)+(stop.tv_nsec-start.tv_nsec)/1e9;
#endif
    FILE*out=outpath?open_path(outpath,"wb"):stdout;if(!out)fail("cannot open output");
    if(native_dir)report_run(out,seconds);else for(size_t i=0;i<job_count;++i){/* Pair protocol retains its original metric-only output. */arena_reset();runtime_init();V record=json_parse(jobs[i].record,strlen(jobs[i].record),NULL);V metrics=map_find(record,S("metrics"),record);json_write(out,metrics);fputc('\n',out);}
    if(out!=stdout)fclose(out);size_t failed=0;for(size_t i=0;i<job_count;++i){failed+=jobs[i].failed;free(jobs[i].record);free(jobs[i].path);free(jobs[i].identifier);}free(jobs);free(input_bytes);
    fprintf(stderr,"Scored %zu documents in %.3fs using %d native C workers (%zu failures)\n",job_count,seconds,workers,failed);return failed?1:0;
}
