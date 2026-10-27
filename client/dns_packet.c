#include "dns_packet.h"
#include <string.h>
static unsigned word(const unsigned char *p){return (unsigned)p[0]*256+p[1];}
int dns_query_build(const char *host,unsigned id,unsigned char *out,size_t capacity){
    size_t len=strlen(host),n=12;
    if(!len || len>253 || capacity<len+18)return -1;
    memset(out,0,12);out[0]=id>>8;out[1]=id;out[2]=1;out[5]=1;
    const char *p=host;
    while(*p){
        const char *end=strchr(p,'.');size_t count=end?(size_t)(end-p):strlen(p);
        if(!count || count>63)return -1;
        out[n++]=(unsigned char)count;
        for(size_t i=0;i<count;i++){
            unsigned char c=(unsigned char)p[i];
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'))return -1;
            out[n++]=c;
        }
        if(!end)break;
        p=end+1;
        if(!*p)return -1;
    }
    out[n++]=0;out[n++]=0;out[n++]=1;out[n++]=0;out[n++]=1;
    return (int)n;
}
static int skip_name(const unsigned char *data,size_t size,size_t *offset){
    size_t n=*offset;
    while(n<size){
        unsigned count=data[n++];
        if(!count){*offset=n;return 0;}
        if((count&0xc0)==0xc0){
            if(n>=size || (((count&63)<<8)|data[n])>=size)return -1;
            *offset=n+1;return 0;
        }
        if(count>63 || count>size-n)return -1;
        n+=count;
    }
    return -1;
}
int dns_answer_ipv4(const unsigned char *reply,size_t size,const unsigned char *query,size_t query_size,uint32_t *address){
    if(query_size<17 || size<query_size || memcmp(reply,query,2) ||
       (reply[2]&0xfa)!=0x80 || (reply[3]&15) || word(reply+4)!=1 ||
       memcmp(reply+12,query+12,query_size-12))return -1;
    size_t n=query_size;unsigned answers=word(reply+6);
    for(unsigned i=0;i<answers;i++){
        if(skip_name(reply,size,&n)<0 || size-n<10)return -1;
        unsigned type=word(reply+n),cls=word(reply+n+2),len=word(reply+n+8);
        n+=10;if(len>size-n)return -1;
        if(type==1 && cls==1 && len==4){
            memcpy(address,reply+n,4);
            return *address && *address!=UINT32_MAX?0:-1;
        }
        n+=len;
    }
    return -1;
}
