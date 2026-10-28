/* The firmware resolver can block its caller indefinitely when the WLAN link
 * changes state. Resolve through a bounded UDP query instead; curl's timeouts
 * cannot interrupt a synchronous gethostbyname() stuck inside sceNet. */
#include "dns_packet.h"
#include "bilibili.h"
#include <pspkernel.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
static int dns_server(char *server,size_t size){
    FILE *f=fopen(CLIENT_PATH("/dns-server.txt"),"r");
    if(f){
        if(!fgets(server,size,f))server[0]=0;
        fclose(f);
        server[strcspn(server,"\r\n")]=0;
        return server[0]!=0;
    }
    SceNetApctlInfo info;
    if(sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PRIMDNS,&info)>=0){
        snprintf(server,size,"%s",info.primaryDns);
        struct in_addr address;
        if(inet_aton(server,&address)==1 && address.s_addr)return 1;
    }
    if(sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SECDNS,&info)>=0){
        snprintf(server,size,"%s",info.secondaryDns);
        struct in_addr address;
        if(inet_aton(server,&address)==1 && address.s_addr)return 1;
    }
    snprintf(server,size,"223.5.5.5");
    return 1;
}

struct hostent *__wrap_gethostbyname(const char *host){
    if(transfer_cancel)return NULL;
    char server[32];
    if(!dns_server(server,sizeof(server))){h_errno=HOST_NOT_FOUND;return NULL;}
    struct sockaddr_in dest={0};
    dest.sin_len=sizeof(dest);dest.sin_family=AF_INET;dest.sin_port=htons(53);
    if(inet_aton(server,&dest.sin_addr)!=1)return NULL;
    unsigned char query[272],reply[1500];
    int length=dns_query_build(host,sceKernelGetSystemTimeLow(),query,sizeof(query));
    if(length<0)return NULL;
    int fd=sceNetInetSocket(AF_INET,SOCK_DGRAM,0);if(fd<0)return NULL;
    unsigned timeout=250000;uint32_t ip=0;
    if(sceNetInetSetsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))>=0 &&
       sceNetInetSetsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))>=0 &&
       sceNetInetConnect(fd,(struct sockaddr*)&dest,sizeof(dest))>=0){
        for(int attempt=0;attempt<2&&!transfer_cancel;attempt++){
            if(sceNetInetSend(fd,query,length,0)!=(size_t)length)break;
            SceInt64 deadline=sceKernelGetSystemTimeWide()+2000000;
            while(!transfer_cancel && sceKernelGetSystemTimeWide()<deadline){
                SceNetInetPollfd wait={.fd=fd,.events=SCE_NET_INET_POLLIN};
                int ready=sceNetInetPoll(&wait,1,100);
                if(ready<=0)continue;
                if(wait.revents&(SCE_NET_INET_POLLERR|SCE_NET_INET_POLLHUP|SCE_NET_INET_POLLNVAL))break;
                int bytes=(int)sceNetInetRecv(fd,reply,sizeof(reply),0);
                if(bytes>0 && dns_answer_ipv4(reply,(size_t)bytes,query,(size_t)length,&ip)==0)break;
            }
            if(ip)break;
            ip=0;
        }
    }
    sceNetInetClose(fd);
    if(!ip){h_errno=HOST_NOT_FOUND;return NULL;}
    /* curl is built with a synchronous resolver; only the network worker calls
     * it. Match libc's static hostent lifetime without heap allocations. */
    static struct hostent result;static char name[254];static uint32_t address;
    static char *addresses[2];
    snprintf(name,sizeof(name),"%s",host);address=ip;
    addresses[0]=(char*)&address;addresses[1]=NULL;
    memset(&result,0,sizeof(result));result.h_name=name;result.h_addrtype=AF_INET;
    result.h_length=4;result.h_addr_list=addresses;result.h_addr=(char*)&address;
    return &result;
}
