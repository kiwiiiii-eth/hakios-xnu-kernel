/* Experimental raw cellular interfaces for the SDM845 IPA. Included by
 * hakios.cell.c only. Opt in with modem_native_pdp=1; legacy en1 is untouched
 * without it. Addresses/routes/DNS remain the native controller's job.
 * This attaches real IFT_CELLULAR interfaces; it does not assert that a PDP
 * context is active or manufacture network or CoreTelephony state.
 */
#include <kern/thread.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
#include <sys/sockio.h>

#include "hakios.native-pdp-rxqueue.h"

#define HAKIOS_PDP_COUNT 16
#define HAKIOS_PDP_MTU 1500
struct hakios_pdp_if { ifnet_t ifp; unsigned unit; };
static struct hakios_pdp_if pdp_ifs[HAKIOS_PDP_COUNT];
static bool pdp_attached;

static errno_t pdp_input_protocol(ifnet_t ifp, protocol_family_t family,
    mbuf_t packet, char *header)
{
    (void)ifp;(void)header;
    if(proto_input(family,packet)!=0)mbuf_freem(packet);
    return 0;
}
static errno_t pdp_add_protocol(ifnet_t ifp, protocol_family_t family,
    const struct ifnet_demux_desc *desc, u_int32_t count)
{
    (void)ifp;(void)desc;(void)count;
    return family==PF_INET || family==PF_INET6 ? 0:EPROTONOSUPPORT;
}
static errno_t pdp_del_protocol(ifnet_t ifp, protocol_family_t family)
{
    (void)ifp;(void)family;return 0;
}
static errno_t pdp_attach_protocol(ifnet_t ifp, protocol_family_t family)
{
    struct ifnet_attach_proto_param params={0};
    if(family!=PF_INET && family!=PF_INET6)return EPROTONOSUPPORT;
    params.input=pdp_input_protocol;
    return ifnet_attach_protocol(ifp,family,&params);
}
static errno_t pdp_demux(ifnet_t ifp, mbuf_t packet, char *header,
    protocol_family_t *family)
{
    uint8_t first=0;(void)ifp;(void)header;
    if(mbuf_copydata(packet,0,1,&first)!=0)return EINVAL;
    if((first>>4)==4)*family=PF_INET;
    else if((first>>4)==6)*family=PF_INET6;
    else return EPROTONOSUPPORT;
    return 0;
}
static errno_t pdp_output(ifnet_t ifp, mbuf_t packet)
{
    struct hakios_pdp_if *p=ifnet_softc(ifp);
    size_t n=mbuf_pkthdr_len(packet);uint8_t bytes[HAKIOS_PDP_MTU];
    bool sent=false;
    if(p && n>=20 && n<=sizeof(bytes) && (ifnet_flags(ifp)&IFF_UP) &&
       mbuf_copydata(packet,0,n,bytes)==0 && ((bytes[0]>>4)==4 || (bytes[0]>>4)==6))
        sent=hakios_ipa_transmit((uint8_t)(p->unit+1),bytes,n);
    if(sent)ifnet_stat_increment_out(ifp,1,(u_int32_t)n,0);
    else ifnet_stat_increment_out(ifp,0,0,1);
    mbuf_freem(packet);
    return 0;
}
static errno_t pdp_ioctl(ifnet_t ifp,u_long command,void *data)
{
    struct ifreq *request=data;
    static unsigned said;
    if(said++<48)printf("native-pdp-v1: pdp_ip%u ioctl=%#lx\n",ifnet_unit(ifp),command);
    switch(command) {
    case SIOCSIFADDR:
        return ifnet_set_flags(ifp,IFF_UP|IFF_RUNNING,IFF_UP|IFF_RUNNING);
    case SIOCSIFFLAGS:
        return ifnet_set_flags(ifp,(ifnet_flags(ifp)&IFF_UP)?IFF_RUNNING:0,IFF_RUNNING);
    case SIOCSIFMTU:
        if(request->ifr_mtu<1280 || request->ifr_mtu>HAKIOS_PDP_MTU)return EINVAL;
        return ifnet_set_mtu(ifp,request->ifr_mtu);
    case SIOCGIFDEVMTU:
        request->ifr_devmtu.ifdm_current=ifnet_mtu(ifp);
        request->ifr_devmtu.ifdm_min=1280;
        request->ifr_devmtu.ifdm_max=HAKIOS_PDP_MTU;
        return 0;
    case SIOCADDMULTI:case SIOCDELMULTI:return 0;
    default:return EOPNOTSUPP;
    }
}
static void pdp_deliver(uint8_t mux,const uint8_t *bytes,size_t length)
{
    mbuf_t packet=NULL;unsigned chunks=1;
    if(mux==0 || mux>HAKIOS_PDP_COUNT || length<20 || length>HAKIOS_PDP_MTU)return;
    ifnet_t ifp=pdp_ifs[mux-1].ifp;
    if(!ifp || !(ifnet_flags(ifp)&IFF_UP))return;
    if((bytes[0]>>4)!=4 && (bytes[0]>>4)!=6)return;
    if(mbuf_allocpacket(MBUF_DONTWAIT,length,&chunks,&packet)!=0)return;
    if(mbuf_copyback(packet,0,length,bytes,MBUF_DONTWAIT)!=0){mbuf_freem(packet);return;}
    mbuf_pkthdr_setrcvif(packet,ifp);mbuf_pkthdr_setheader(packet,NULL);
    struct ifnet_stat_increment_param stats={.packets_in=1,.bytes_in=(u_int32_t)length};
    ifnet_input(ifp,packet,&stats);
}
static struct hakios_pdp_rx_queue pdp_rx_queue;
static void pdp_capture(uint8_t mux,const uint8_t *bytes,size_t length)
{
    /* IPA owns its lock here. Copy DMA data only; no network-stack calls. */
    (void)hakios_pdp_rx_capture(&pdp_rx_queue,mux,bytes,length);
}
static void pdp_poll_thread(void *argument,wait_result_t result)
{
    (void)argument;(void)result;
    uint64_t reported_drops=0;unsigned reports=0;
    for(;;){
        hakios_ipa_poll(pdp_capture);
        /* IPA lock has been released. Network input may now trigger output. */
        hakios_pdp_rx_deliver(&pdp_rx_queue,pdp_deliver);
        if(pdp_rx_queue.dropped!=reported_drops && reports++<4) {
            printf("native-pdp-rxqueue-v2: bounded RX batch drops=%llu\n",(unsigned long long)pdp_rx_queue.dropped);
            reported_drops=pdp_rx_queue.dropped;
        }
        delay_for_interval(2,NSEC_PER_MSEC);
    }
}
static void hakios_native_pdp_attach(void)
{
    if(pdp_attached || !hakios_ipa_data_ready())return;
    int e4=proto_register_plumber(PF_INET,IFNET_FAMILY_CELLULAR,pdp_attach_protocol,NULL);
    int e6=proto_register_plumber(PF_INET6,IFNET_FAMILY_CELLULAR,pdp_attach_protocol,NULL);
    if((e4 && e4!=EEXIST) || (e6 && e6!=EEXIST)) {
        printf("native-pdp-v1: protocol plumber errors=%d,%d\n",e4,e6);return;
    }
    unsigned count=0;
    for(unsigned i=0;i<HAKIOS_PDP_COUNT;i++) {
        struct hakios_pdp_if *p=&pdp_ifs[i];char id[32];
        struct ifnet_init_eparams init={0};
        if(p->ifp){count++;continue;}
        snprintf(id,sizeof(id),"hakios-native-pdp-%u",i);
        p->unit=i;init.ver=IFNET_INIT_CURRENT_VERSION;init.len=sizeof(init);
        init.flags=IFNET_INIT_LEGACY|IFNET_INIT_NX_NOAUTO;
        init.name="pdp_ip";init.unit=i;init.uniqueid=id;init.uniqueid_len=strlen(id);
        init.family=IFNET_FAMILY_CELLULAR;init.type=IFT_CELLULAR;init.softc=p;
        init.output=pdp_output;init.demux=pdp_demux;init.ioctl=pdp_ioctl;
        init.add_proto=pdp_add_protocol;init.del_proto=pdp_del_protocol;
        ifnet_t ifp=NULL;int error=ifnet_allocate_extended(&init,&ifp);
        if(error){printf("native-pdp-v1: allocate unit=%u error=%d\n",i,error);break;}
        ifnet_set_mtu(ifp,HAKIOS_PDP_MTU);ifnet_set_hdrlen(ifp,0);ifnet_set_addrlen(ifp,0);
        ifnet_set_flags(ifp,IFF_POINTOPOINT|IFF_MULTICAST|IFF_NOARP,0xffff);
        ifnet_set_offload(ifp,0);
        error=ifnet_attach(ifp,NULL);
        if(error){ifnet_release(ifp);printf("native-pdp-v1: attach unit=%u error=%d\n",i,error);break;}
        p->ifp=ifp;count++;
        e4=pdp_attach_protocol(ifp,PF_INET);e6=pdp_attach_protocol(ifp,PF_INET6);
        printf("native-pdp-v1: pdp_ip%u index=%u mux=%u family=%u type=%u protocols=%d,%d\n",
            i,ifnet_index(ifp),i+1,ifnet_family(ifp),ifnet_type(ifp),e4,e6);
    }
    /* One IPA consumer only: this mode replaces the Ethernet consumer. */
    if(count) {
        thread_t thread;
        if(kernel_thread_start(pdp_poll_thread,NULL,&thread)==KERN_SUCCESS) {
            thread_deallocate(thread);pdp_attached=true;
            printf("native-pdp-rxqueue-v2: interfaces=%u poller-started=1 batch=256 delivery-after-IPA-unlock=1\n",count);
        }
    }
}
