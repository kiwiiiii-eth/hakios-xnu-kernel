#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <stdint.h>

/* The verified native getter returns ctu::SharedRef<CFDictionary const>:
 * one owned CF pointer, nontrivial C++ return through x8 on arm64. Use that
 * ABI and exactly one CFRelease; do not reinterpret this as a C pointer return.
 */
struct NativeDictionaryRef {
    CFDictionaryRef value = nullptr;
    NativeDictionaryRef() = default;
    NativeDictionaryRef(const NativeDictionaryRef &) = delete;
    NativeDictionaryRef(NativeDictionaryRef &&other) : value(other.value) { other.value=nullptr; }
    ~NativeDictionaryRef() { if(value)CFRelease(value); }
};
using NativeGetter=NativeDictionaryRef (*)(void *);

extern "C" void probe_native_activation_dictionary(void *self,uintptr_t address)
{
    auto dictionary=reinterpret_cast<NativeGetter>(address)(self);
    bool valid=dictionary.value && CFGetTypeID(dictionary.value)==CFDictionaryGetTypeID();
    CFTypeRef identity=valid?CFDictionaryGetValue(dictionary.value,CFSTR("InternationalMobileSubscriberIdentity")):nullptr;
    CFTypeRef card=valid?CFDictionaryGetValue(dictionary.value,CFSTR("IntegratedCircuitCardIdentity")):nullptr;
    CFIndex card_length=card && CFGetTypeID(card)==CFStringGetTypeID()?CFStringGetLength(static_cast<CFStringRef>(card)):-1;
    os_log(OS_LOG_DEFAULT,"hakios: cc: subscriber-dictionary-v2 card-present=%d card-string-length=%ld",card!=nullptr,card_length);
    CFIndex string_length=identity && CFGetTypeID(identity)==CFStringGetTypeID()?CFStringGetLength(static_cast<CFStringRef>(identity)):-1;
    CFIndex data_length=identity && CFGetTypeID(identity)==CFDataGetTypeID()?CFDataGetLength(static_cast<CFDataRef>(identity)):-1;
    os_log(OS_LOG_DEFAULT,"hakios: cc: subscriber-dictionary-v1 dictionary=%d keys=%ld identity-present=%d string-length=%ld data-length=%ld",
        valid,valid?CFDictionaryGetCount(dictionary.value):0,identity!=nullptr,string_length,data_length);
}

/* Borrowed SharedRef argument: inspect types/counts only, never retain/release
 * or expose any SIM identifier. Original publisher copies before async work. */
static void inspect_carrier_dictionary(CFDictionaryRef dictionary,int kind,bool borrowed)
{
    bool valid=dictionary && CFGetTypeID(dictionary)==CFDictionaryGetTypeID();
    CFTypeRef sims=valid?CFDictionaryGetValue(dictionary,CFSTR("SubscriberIdentityModules")):nullptr;
    bool mapping=sims && CFGetTypeID(sims)==CFDictionaryGetTypeID();
    CFIndex count=mapping?CFDictionaryGetCount(static_cast<CFDictionaryRef>(sims)):-1;
    CFTypeRef first=mapping?CFDictionaryGetValue(static_cast<CFDictionaryRef>(sims),CFSTR("kOne")):nullptr;
    bool slot=first && CFGetTypeID(first)==CFDictionaryGetTypeID();
    CFTypeRef subscriber=slot?CFDictionaryGetValue(static_cast<CFDictionaryRef>(first),CFSTR("InternationalMobileSubscriberIdentity")):nullptr;
    CFTypeRef card=slot?CFDictionaryGetValue(static_cast<CFDictionaryRef>(first),CFSTR("IntegratedCircuitCardIdentity")):nullptr;
    CFIndex subscriber_length=subscriber && CFGetTypeID(subscriber)==CFStringGetTypeID()?CFStringGetLength(static_cast<CFStringRef>(subscriber)):-1;
    CFIndex card_length=card && CFGetTypeID(card)==CFStringGetTypeID()?CFStringGetLength(static_cast<CFStringRef>(card)):-1;
    os_log(OS_LOG_DEFAULT,"hakios: cc: carrier-publish-v1 borrowed=%d kind=%d dictionary=%d keys=%ld sims-mapping=%d slots=%ld first-dictionary=%d subscriber-length=%ld card-length=%ld",borrowed,kind,valid,valid?CFDictionaryGetCount(dictionary):0,mapping,count,slot,subscriber_length,card_length);
}
extern "C" void probe_native_carrier_dictionary(void *self,uintptr_t address)
{
    auto dictionary=reinterpret_cast<NativeGetter>(address)(self);
    inspect_carrier_dictionary(dictionary.value,-1,false);
}
extern "C" void probe_borrowed_carrier_dictionary(const void *shared_ref,int kind)
{
    CFDictionaryRef dictionary=nullptr;
    if(shared_ref)__builtin_memcpy(&dictionary,shared_ref,sizeof(dictionary));
    inspect_carrier_dictionary(dictionary,kind,true);
}

#include <memory>
#include <mutex>
#include <cstring>
/* The native object's enable_shared_from_this weak pair is at +8/+16:
 * constructor initializes both, base destructor releases weak control at+16.
 * Capture only inside the original registration call with matching vtable,
 * self-pointer, nonzero control and successful native-compatible weak lock.
 * Hold a strong lease during each synchronous native publisher call. */
static_assert(sizeof(std::weak_ptr<void>)==16,"verified arm64 libc++ weak ABI");
static std::mutex carrier_mutex;
static std::weak_ptr<void> carrier_weak;
static uintptr_t carrier_vtable,carrier_publish_address;
static CFDictionaryRef last_carrier_dictionary;
static thread_local bool publishing_carrier;
extern "C" void configure_real_sim_carrier_publisher(uintptr_t table,uintptr_t publisher)
{
    std::lock_guard<std::mutex> guard(carrier_mutex);
    carrier_vtable=table;carrier_publish_address=publisher;carrier_weak.reset();
    if(last_carrier_dictionary){CFRelease(last_carrier_dictionary);last_carrier_dictionary=nullptr;}
}
extern "C" void remember_native_carrier_publisher(void *self)
{
    if(!self)return;
    uintptr_t table=0,object=0,control=0;
    std::memcpy(&table,self,8);std::memcpy(&object,static_cast<char *>(self)+8,8);std::memcpy(&control,static_cast<char *>(self)+16,8);
    std::lock_guard<std::mutex> guard(carrier_mutex);
    if(table!=carrier_vtable || object!=reinterpret_cast<uintptr_t>(self) || !control)return;
    const auto &weak=*reinterpret_cast<const std::weak_ptr<void> *>(static_cast<char *>(self)+8);
    auto lease=weak.lock();if(lease.get()!=self)return;
    carrier_weak=weak;
    os_log(OS_LOG_DEFAULT,"hakios: cc: real-sim-carrier-v1 native-weak-owner-captured=1");
}
struct CarrierCompleteness {unsigned complete=0;};
static void count_complete_carrier_slot(const void *,const void *value,void *context)
{
    if(!value || CFGetTypeID(value)!=CFDictionaryGetTypeID())return;
    auto slot=static_cast<CFDictionaryRef>(value);
    CFTypeRef subscriber=CFDictionaryGetValue(slot,CFSTR("InternationalMobileSubscriberIdentity"));
    CFTypeRef card=CFDictionaryGetValue(slot,CFSTR("IntegratedCircuitCardIdentity"));
    if(subscriber && card && CFGetTypeID(subscriber)==CFStringGetTypeID() && CFGetTypeID(card)==CFStringGetTypeID()
       && CFStringGetLength(static_cast<CFStringRef>(subscriber))==15 && CFStringGetLength(static_cast<CFStringRef>(card))==20)
        static_cast<CarrierCompleteness *>(context)->complete++;
}
extern "C" void publish_real_sim_carrier(void *model,uintptr_t getter,bool registered,bool pdp)
{
    if(!model || !getter || !registered || !pdp || publishing_carrier)return;
    std::shared_ptr<void> lease;uintptr_t publisher=0;
    {std::lock_guard<std::mutex> guard(carrier_mutex);lease=carrier_weak.lock();publisher=carrier_publish_address;}
    if(!lease || !publisher)return;
    auto dictionary=reinterpret_cast<NativeGetter>(getter)(model);
    if(!dictionary.value || CFGetTypeID(dictionary.value)!=CFDictionaryGetTypeID())return;
    CFTypeRef sims=CFDictionaryGetValue(dictionary.value,CFSTR("SubscriberIdentityModules"));
    if(!sims || CFGetTypeID(sims)!=CFDictionaryGetTypeID())return;
    CarrierCompleteness completeness;CFDictionaryApplyFunction(static_cast<CFDictionaryRef>(sims),count_complete_carrier_slot,&completeness);
    if(!completeness.complete)return;
    {std::lock_guard<std::mutex> guard(carrier_mutex);
     if(last_carrier_dictionary && CFEqual(last_carrier_dictionary,dictionary.value))return;
     if(last_carrier_dictionary)CFRelease(last_carrier_dictionary);
     last_carrier_dictionary=static_cast<CFDictionaryRef>(CFRetain(dictionary.value));}
    publishing_carrier=true;
    /* 2=kSimCarrier. Submit the original native-generated dictionary unchanged,
     * preserving NotProvisioned activation state and all original fields.
     * Original10057a554 copies the dictionary before queuing native work. */
    reinterpret_cast<void (*)(void *,int,const void *)>(publisher)(lease.get(),2,&dictionary);
    publishing_carrier=false;
    os_log(OS_LOG_DEFAULT,"hakios: cc: real-sim-carrier-v1 published=1 kind=2 native-owner-held=1 original-dictionary-retained=1 activation-status-unchanged=1");
}
