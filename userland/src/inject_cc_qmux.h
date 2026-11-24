#ifndef HAKIOS_QMUX_ENDPOINT_MAP_H
#define HAKIOS_QMUX_ENDPOINT_MAP_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
/* Supply the actual IPA endpoint to an otherwise valid WDS Bind Mux request
 * which omitted endpoint info. The caller's mux, transaction and TLVs remain
 * byte-for-byte unchanged. Never changes any response or result code. */
static uint8_t *qmux_bind_ipa_endpoint(const uint8_t *b,size_t n,size_t *out) {
    *out=n;
    if(n<17 || n>8181 || b[0]!=1 || b[4]!=1 || b[6]!=0 || b[9]!=0xa2 || b[10]!=0 ||
       (size_t)(b[1]|b[2]<<8)+1!=n || (size_t)(b[11]|b[12]<<8)+13!=n) return NULL;
    unsigned muxes=0;size_t at=13;
    while(at<n) {
        if(n-at<3) return NULL;
        size_t len=b[at+1]|b[at+2]<<8;
        if(len>n-at-3 || b[at]==0x10) return NULL;
        if(b[at]==0x11){if(len!=1)return NULL;muxes++;}
        at+=3+len;
    }
    if(muxes!=1) return NULL;
    uint8_t *copy=malloc(n+11);if(!copy)return NULL;
    memcpy(copy,b,n);
    const uint8_t endpoint[11]={0x10,8,0,4,0,0,0,1,0,0,0};
    memcpy(copy+n,endpoint,11);
    size_t total=n+11,body=total-13;
    copy[1]=(total-1)&255;copy[2]=(total-1)>>8;
    copy[11]=body&255;copy[12]=body>>8;
    *out=total;return copy;
}
#endif
