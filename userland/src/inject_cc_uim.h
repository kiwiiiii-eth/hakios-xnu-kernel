#ifndef HAKIOS_UIM_SLOT_MAP_H
#define HAKIOS_UIM_SLOT_MAP_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* QMI UIM Card Status TLV 0x10. Only reorder two fully parsed real cards.
 * Card/application states, retries and AIDs are copied verbatim. */
static int uim_card_end(const uint8_t *v,size_t n,size_t start,size_t *end) {
    if(start>n || n-start<6) return 0;
    unsigned apps=v[start+5];size_t at=start+6;
    for(unsigned a=0;a<apps;a++) {
        if(at>n || n-at<7) return 0;
        size_t bytes=14u+v[at+6];
        if(bytes>n-at) return 0;
        at+=bytes;
    }
    *end=at;return 1;
}
static int uim_swap_real_card(uint8_t *v,size_t n) {
    if(n<21 || n>8192 || v[8]!=2) return 0;
    size_t first_end,second_end;
    if(!uim_card_end(v,n,9,&first_end) || !uim_card_end(v,n,first_end,&second_end) || second_end!=n) return 0;
    if(v[9]==1 || v[first_end]!=1) return 0;
    for(unsigned i=0;i<8;i+=2) if(!(v[i]==255 && v[i+1]==255) && v[i+1]>1) return 0;
    uint8_t *copy=malloc(n);if(!copy)return 0;
    memcpy(copy,v,n);
    size_t first=first_end-9,second=second_end-first_end;
    memcpy(v+9,copy+first_end,second);
    memcpy(v+9+second,copy+9,first);
    for(unsigned i=0;i<8;i+=2) if(!(v[i]==255 && v[i+1]==255)) v[i+1]^=1;
    free(copy);return 1;
}
#endif
