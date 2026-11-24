#ifndef HAKIOS_NATIVE_PDP_RXQUEUE_H
#define HAKIOS_NATIVE_PDP_RXQUEUE_H
#ifdef KERNEL
#include <sys/types.h>
#include <libkern/libkern.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#endif
/* One poll-thread owner. Capture copies DMA bytes while IPA owns its lock;
 * deliver only after hakios_ipa_poll has returned and released that lock.
 * No ifnet, mbuf, allocation, logging or network callback is permitted here. */
#define HAKIOS_PDP_RX_BATCH 256
#define HAKIOS_PDP_RX_MTU 1500
struct hakios_pdp_rx_packet {uint16_t length;uint8_t mux;uint8_t data[HAKIOS_PDP_RX_MTU];};
struct hakios_pdp_rx_queue {unsigned count;uint64_t dropped;struct hakios_pdp_rx_packet packets[HAKIOS_PDP_RX_BATCH];};
static bool hakios_pdp_rx_capture(struct hakios_pdp_rx_queue *queue,uint8_t mux,const uint8_t *bytes,size_t length)
{
    if(!bytes || !mux || mux>16 || length<20 || length>HAKIOS_PDP_RX_MTU || ((bytes[0]>>4)!=4 && (bytes[0]>>4)!=6))return false;
    if(queue->count==HAKIOS_PDP_RX_BATCH){queue->dropped++;return false;}
    struct hakios_pdp_rx_packet *packet=&queue->packets[queue->count++];
    packet->mux=mux;packet->length=(uint16_t)length;memcpy(packet->data,bytes,length);return true;
}
static unsigned hakios_pdp_rx_deliver(struct hakios_pdp_rx_queue *queue,void (*receive)(uint8_t,const uint8_t *,size_t))
{
    unsigned count=queue->count;
    for(unsigned i=0;i<count;i++){const struct hakios_pdp_rx_packet *packet=&queue->packets[i];receive(packet->mux,packet->data,packet->length);}
    queue->count=0;return count;
}
#endif
