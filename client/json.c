#include "json.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
/* Bounded, allocation-free token tree. Object/array traversal uses subtree ends. */
static void space(const char *s, int *p) { while (s[*p] && strchr(" \r\n\t", s[*p])) ++*p; }
static int value(Json *j, int *p, int cap, int depth) {
    if (depth > 32 || j->count >= cap) return -1;
    space(j->text,p);
    int n=j->count++, start=*p; char c=j->text[*p];
    JsonToken *t=&j->tokens[n]; *t=(JsonToken){start,0,0,0};
    if (c=='{' || c=='[') {
        char close=c=='{'?'}':']'; ++*p; space(j->text,p);
        if (j->text[*p]!=close) for (;;) {
            if (c=='{') {
                if (j->text[*p]!='"' || value(j,p,cap,depth+1)<0) return -1;
                ++t->children; space(j->text,p);
                if (j->text[(*p)++]!=':') return -1;
            }
            if (value(j,p,cap,depth+1)<0) return -1;
            ++t->children; space(j->text,p);
            if (j->text[*p]!=',') break;
            ++*p; space(j->text,p);
        }
        if (j->text[(*p)++]!=close) return -1;
    } else if (c=='"') {
        ++*p;
        while (j->text[*p] && j->text[*p]!='"') {
            if ((unsigned char)j->text[*p]<32) return -1;
            if (j->text[*p]=='\\') { ++*p; if (!j->text[*p]) return -1; }
            ++*p;
        }
        if (!j->text[*p]) return -1;
        ++*p;
    } else {
        while (j->text[*p] && !strchr(" \r\n\t,}]",j->text[*p])) ++*p;
        int len=*p-start; char tmp[80];
        if (len<=0 || len >= (int)sizeof(tmp)) return -1;
        memcpy(tmp,j->text+start,len);tmp[len]=0;
        if (strcmp(tmp,"true") && strcmp(tmp,"false") && strcmp(tmp,"null")) {
            char *end; strtod(tmp,&end); if (*end) return -1;
        }
    }
    t->end=*p; t->next=j->count; return n;
}
int json_parse(Json *j,const char *s,JsonToken *t,int cap) {
    *j=(Json){s,t,0};int p=0;
    if (value(j,&p,cap,0)<0) return -1;
    space(s,&p);return s[p]?-1:j->count;
}
int json_key(const Json *j,int object,const char *key) {
    if (object<0 || object>=j->count || j->text[j->tokens[object].start]!='{') return -1;
    for(int n=object+1;n<j->tokens[object].next;) {
        JsonToken t=j->tokens[n]; int v=n+1;
        if(t.end-t.start==(int)strlen(key)+2 && !strncmp(j->text+t.start+1,key,strlen(key))) return v;
        n=j->tokens[v].next;
    } return -1;
}
static unsigned hex4(const char *s) {
    unsigned v=0;for(int n=0;n<4;++n){char c=s[n];unsigned h=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;if(h>15)return 0xfffd;v=v*16+h;}return v;
}
int json_string(const Json *j,int n,char *out,size_t size) {
    if (!size) return -1;
    out[0]=0;
    if(n<0||n>=j->count)return -1;
    JsonToken t=j->tokens[n];if(j->text[t.start]!='"')return -1;
    size_t w=0;
    for(int p=t.start+1;p<t.end-1;){
        unsigned cp=(unsigned char)j->text[p++];char bytes[4];int len=1;
        if(cp=='\\') {
            cp=(unsigned char)j->text[p++];
            if(cp=='u') {
                if(p+4>t.end-1)return -1;
                cp=hex4(j->text+p);p+=4;
                if(cp>=0xd800&&cp<=0xdbff && p+6<=t.end-1 && j->text[p]=='\\'&&j->text[p+1]=='u') {
                    unsigned low=hex4(j->text+p+2);
                    if(low>=0xdc00&&low<=0xdfff){cp=0x10000+((cp-0xd800)<<10)+low-0xdc00;p+=6;}
                }
                if(cp>=0xd800&&cp<=0xdfff)cp=0xfffd;
            } else if(cp=='n')cp='\n';else if(cp=='r')cp='\r';else if(cp=='t')cp='\t';
            else if(cp=='b')cp='\b';else if(cp=='f')cp='\f';else if(cp!='"'&&cp!='\\'&&cp!='/')return -1;
        }
        if(cp<128)bytes[0]=cp;
        else if(cp<256 && (unsigned char)j->text[p-1]>=128)bytes[0]=cp; /* original UTF-8 byte */
        else if(cp<2048){len=2;bytes[0]=0xc0|(cp>>6);bytes[1]=0x80|(cp&63);}
        else if(cp<65536){len=3;bytes[0]=0xe0|(cp>>12);bytes[1]=0x80|((cp>>6)&63);bytes[2]=0x80|(cp&63);}
        else {len=4;bytes[0]=0xf0|(cp>>18);bytes[1]=0x80|((cp>>12)&63);bytes[2]=0x80|((cp>>6)&63);bytes[3]=0x80|(cp&63);}
        if(w+len>=size)break;
        memcpy(out+w,bytes,len);w+=len;
    }
    out[w]=0;return (int)w;
}
long long json_number(const Json *j,int n){return n<0||n>=j->count?0:strtoll(j->text+j->tokens[n].start,0,10);}
