/*
 * hakios.baseband.cpp: Apple's baseband drivers as Darwin's telephony looks them up, stand-ins (boot-arg abb=1, a test).
 *
 *   AppleBaseband      the modem's controller: what libTelephonyBasebandDynamic's TelephonyBaseband* calls reach
 *                      (_TelephonyBasebandControllerCallMethod: 0x00 reset, 0x01 power on, 0x02 mux, 0x03 get reset,
 *                      0x1a/0x1b the baseband's state, 0x1c/0x1d its properties, 0x16/0x17 the manager's state, ...).
 *                      Without it CommCenter waits for the driver, then aborts: "Unable to communicate with AppleBaseband
 *                      driver" (r294).
 *   AppleIPAppender    the cellular data path's (ApplePDPHelperInterface): without it CommCenter dies a moment after
 *                      "Could not find AppleIPAppender service" (r294, SIGSEGV).
 *
 * This phone's modem is not Apple's: it is up already, on the SoC, and speaks QMI over QRTR (hakios.mpss845.c). For now each
 * stand-in answers every call with success and zeros and says what it was asked, so that what CommCenter wants of them can
 * be read off a run; the answers that matter come next.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOKitKeys.h>

extern "C" {
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <stdarg.h>
extern void loader_log_putc(char c);
}

/* a line for the log the loader hands back (kern printf alone stays in the kernel's own), as hakios.mpss845.c's tell() */
static void
bb_say(const char * format, ...) __printflike(1, 2);

static void
bb_say(const char * format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char * c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

/* what a call was: who, which, and what it brought */
static void
hakios_bb_say(const char * who, uint32_t selector, IOExternalMethodArguments * args)
{
	char name[32] = "?";
	uint8_t head[16] = { 0 };
	uint32_t shown = 0;

	proc_selfname(name, sizeof(name));
	if (args->structureInput != NULL && args->structureInputSize != 0) {
		shown = args->structureInputSize < sizeof(head) ? args->structureInputSize : (uint32_t) sizeof(head);
		memcpy(head, args->structureInput, shown);
	}
	bb_say("%s: %s[%d] call %#x: %u scalars in (%#llx %#llx %#llx), %u out; structure in %u (%02x%02x%02x%02x %02x%02x%02x%02x), "
	    "out %u%s\n", who, name, proc_selfpid(), selector, args->scalarInputCount,
	    args->scalarInputCount > 0 ? args->scalarInput[0] : 0ull, args->scalarInputCount > 1 ? args->scalarInput[1] : 0ull,
	    args->scalarInputCount > 2 ? args->scalarInput[2] : 0ull, args->scalarOutputCount, args->structureInputSize,
	    head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7], args->structureOutputSize,
	    args->structureInputDescriptor != NULL || args->structureOutputDescriptor != NULL ? " (by descriptor)" : "");
}

/* every output zero, and success */
static IOReturn
hakios_bb_zeros(IOExternalMethodArguments * args)
{
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		memset(args->structureOutput, 0, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}


/* ---- a user client that answers everything, saying so -------------------------------------------------------------- */

class HakiosBasebandUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosBasebandUserClient);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosBasebandUserClient, IOUserClient);

bool
HakiosBasebandUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type)
{
	char name[32] = "?";

	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	proc_selfname(name, sizeof(name));
	bb_say("baseband: %s[%d] opens a connection of type %u\n", name, proc_selfpid(), (unsigned) type);
	return true;
}

IOReturn
HakiosBasebandUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

IOReturn
HakiosBasebandUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	bb_say("baseband: a notification port of type %u\n", (unsigned) type);
	releaseNotificationPort(port);
	return kIOReturnSuccess;
}

IOReturn
HakiosBasebandUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	IOService * provider = getProvider();

	hakios_bb_say(provider != NULL ? provider->getName() : "baseband", selector, args);
	return hakios_bb_zeros(args);
}

IOReturn
HakiosBasebandUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	bb_say("baseband: memory of type %u asked for: none\n", (unsigned) type);
	return kIOReturnUnsupported;
}


/* ---- the services, by the class names user space matches ---------------------------------------------------------- */

class AppleBaseband : public IOService
{
	OSDeclareDefaultStructors(AppleBaseband);
};

OSDefineMetaClassAndStructors(AppleBaseband, IOService);

class AppleIPAppender : public IOService
{
	OSDeclareDefaultStructors(AppleIPAppender);
};

OSDefineMetaClassAndStructors(AppleIPAppender, IOService);

static IOService *
hakios_bb_publish(IOService * made, const char * name)
{
	IOService * platform = IOService::getPlatform();

	if (made == NULL || platform == NULL || !made->init()) {
		bb_say("baseband: %s NOT published: %s\n", name, made == NULL ? "no object" : platform == NULL ? "no platform" : "init");
		OSSafeReleaseNULL(made);
		return NULL;
	}
	made->setProperty(kIOUserClientClassKey, "HakiosBasebandUserClient");
	if (!made->attach(platform)) {
		bb_say("baseband: %s NOT published: attach\n", name);
		made->release();
		return NULL;
	}
	made->setName(name);
	if (!made->start(platform)) {
		bb_say("baseband: %s NOT published: start\n", name);
		made->detach(platform);
		made->release();
		return NULL;
	}
	made->registerService();
	bb_say("baseband: %s published (a stand-in: every call answered with zeros)\n", name);
	return made;
}

/* hakios.mpss845.c, once the modem's QMI services are up */
extern "C" void
HakiosBasebandPublish(void)
{
	static bool done;
	uint32_t wanted = 0;

	if (done) {
		return;
	}
	if (!PE_parse_boot_argn("abb", &wanted, sizeof(wanted)) || wanted == 0) {
		bb_say("baseband: the stand-ins are not wanted (abb=%u)\n", wanted);
		done = true;
		return;
	}
	done = true;
	(void) hakios_bb_publish(OSTypeAlloc(AppleBaseband), "AppleBaseband");
	(void) hakios_bb_publish(OSTypeAlloc(AppleIPAppender), "AppleIPAppender");
}
