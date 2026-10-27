#ifndef BILI_DNS_PACKET_H
#define BILI_DNS_PACKET_H
#include <stddef.h>
#include <stdint.h>
int dns_query_build(const char *host,unsigned id,unsigned char *out,size_t capacity);
int dns_answer_ipv4(const unsigned char *reply,size_t size,const unsigned char *query,size_t query_size,uint32_t *address);
#endif
