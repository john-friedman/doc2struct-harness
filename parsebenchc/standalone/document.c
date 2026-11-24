/* Per-document applicability and wiring. The algorithms themselves live in the
   generated C modules; this layer reads the native ParseBench JSON envelope. */
static V invoke_args(const char*module,const char*name,V args,V kw){return call(attr(module_get(module),name),args,kw);}
static void result_lanes(V metrics,V result,const char*headline,const char*lanes_name){
    map_set(metrics,str(headline),attr(result,"score"));V lanes=attr(result,lanes_name),keys=map_keys(lanes);
    for(size_t i=0;i<length(keys);++i){V key=item(keys,integer(i)),value=map_find(lanes,key,NONE);if(value!=NONE)map_set(metrics,binary(OP_ADD,binary(OP_ADD,str(headline),S("_")),key),value);}
}
static V score_document(V testcase,V envelope){
    V output=map_find(envelope,S("output"),envelope);
    V markdown=map_find(output,S("markdown"),S(""));if(markdown==NONE)markdown=S("");
    if(map_find(output,S("pages"),UNBOUND)==UNBOUND)map_set(output,S("pages"),list_new());
    if(map_find(output,S("layout_pages"),UNBOUND)==UNBOUND)map_set(output,S("layout_pages"),list_new());
    V metadata=map_find(testcase,S("metadata"),map_new());if(metadata==NONE)metadata=map_new();
    V rules=map_find(testcase,S("test_rules"),list_new());if(rules==NONE)rules=list_new();
    if(truth(map_find(metadata,S("chart_tables"),NONE)))fail("chart scoring is outside this local text/formatting/tables/forms suite");
    for(size_t i=0;i<length(rules);++i){V kind=map_find(item(rules,integer(i)),S("type"),NONE);if(equal(kind,S("chart_data_point"))||equal(kind,S("layout")))fail("chart/layout scoring is outside this local text/formatting/tables/forms suite");}
    V expected=map_find(testcase,S("expected_markdown"),NONE),reference=invoke("text_next","reference_markdown",2,expected,rules);
    V transparent=map_find(metadata,S("sub_page_handling"),NONE);int strip=transparent!=NONE;
    if(strip&&!equal(transparent,S("transparent-v1")))fail("unsupported sub_page_handling");
    if(reference!=NONE&&strip)reference=invoke("text_next","strip_sub_page_wrappers",1,reference);
    V hypothesis=invoke("text_next","output_hypothesis",1,output);
    if(strip)hypothesis=invoke("text_next","strip_sub_page_wrappers",1,hypothesis);
    V metrics=map_new();
    int text_applies=reference!=NONE&&truth(invoke("text_next","text_next_applies",3,reference,rules,metadata));
    if(text_applies){
        V result=invoke("text_next","score_text_next",2,reference,hypothesis);map_set(metrics,S("text_next"),attr(result,"score"));
        const char*lanes[]={"words","critical","integrity","order",NULL};
        for(int i=0;lanes[i];++i){V value=attr(result,lanes[i]);if(value!=NONE){char key[64];snprintf(key,sizeof(key),"text_next_%s",lanes[i]);map_set(metrics,str(key),value);}}
    }
    V fmtref=NONE,neutral=list_new();
    if(!truth(reference))fmtref=invoke("formatting_v2","rules_reference",1,rules);
    else if(text_applies&&truth(invoke("formatting_v2","formatting_v2_applies",1,reference))){fmtref=list_new();append(fmtref,reference);append(fmtref,attr(module_get("formatting_v2"),"FULL"));}
    V decorations=invoke("formatting_v2","decoration_reference",1,metadata);
    if(decorations!=NONE){fmtref=decorations;neutral=item(decorations,integer(2));}
    if(fmtref!=NONE){
        V fmtmd=item(fmtref,integer(0)),scope=item(fmtref,integer(1));if(strip)fmtmd=invoke("text_next","strip_sub_page_wrappers",1,fmtmd);
        V refdoc=invoke("formatting_v2","parse",1,fmtmd),outdoc=invoke("formatting_v2","parse",1,hypothesis);
        V skip=invoke("formatting_v2","_neutral_tokens",2,refdoc,neutral);
        V result=invoke("formatting_v2","score_docs",4,refdoc,outdoc,scope,skip);
        if(!truth(attr(scope,"decoration"))&&!truth(invoke("formatting_v2","has_formatting",1,refdoc)))result=invoke("formatting_v2","_cap_unlocated_plain_text",3,refdoc,outdoc,result);
        result_lanes(metrics,result,"formatting_v2","lanes");
    }
    V formsref=invoke("forms_v2","forms_reference",2,metadata,rules);
    if(truth(invoke("forms_v2","forms_v2_applies",1,formsref))){V result=invoke("forms_v2","score_forms",2,formsref,markdown);result_lanes(metrics,result,"forms_v2","lanes");}
    if(expected!=NONE&&truth(invoke("table_next","has_reference_tables",1,expected))){
        V args=list_new(),flags=map_new();append(args,expected);append(args,markdown);
        const char*names[]={"max_top_title_rows","allow_splitting_ambiguous_merged_tables","trm_unsupported","allow_merged_sub_rows",NULL};
        for(int i=0;names[i];++i)map_set(flags,str(names[i]),map_find(testcase,str(names[i]),i==0?integer(1):FALSEV));
        V result=invoke_args("table_next","score_table_next",args,flags);map_set(metrics,S("table_next"),attr(result,"score"));map_set(metrics,S("table_next_content"),attr(result,"content"));map_set(metrics,S("table_next_row_order"),attr(result,"row_order"));
    }
    return metrics;
}
