/*
 * IOHIDFamily's IOHIDResource and its user client, as much as bluetoothd uses to make a HID device of a Bluetooth
 * keyboard or mouse (IOHIDUserDeviceCreate, HID over GATT). This kernel has no IOHIDFamily; what the user client is
 * handed goes to /dev/hakios-hid (hakios.hidres.c) for backboardd to post as events.
 *
 * From Apple's open source IOHIDFamily (1633.100.36: IOHIDResourceUserClient.cpp, the dispatch table at 63-94; the
 * lane's lane-b-notes/bt-hid-userdevice.md): matched by class "IOHIDResource", opened with type 0; selectors (scalars
 * in / structure in), none with anything out:
 *   0 create             1 / the device's properties, an XML property list with its ReportDescriptor
 *   1 terminate          0 / 0
 *   2 handle report      1 (a timestamp) / the report's bytes
 *   3 post report response, 4 register service, 5 release token: taken, nothing done
 * The entitlements IOHIDFamily asks for (com.apple.hid.manager.user-access-device, com.apple.developer.hid.virtual.device)
 * are not asked for here: whoever opens it is said in the log.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSUnserialize.h>
#include <sys/proc.h>

extern "C" void hakios_hidres_record(uint16_t type, uint16_t device, const void *data, uint32_t length);
extern "C" void hakios_hidres_say(const char *format, ...) __printflike(1, 2);
extern "C" void HakiosHIDResourceStart(void);

class IOHIDResource : public IOService
{
	OSDeclareDefaultStructors(IOHIDResource);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOHIDResource, IOService);

bool
IOHIDResource::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "IOHIDResourceDeviceUserClient");
	registerService();
	return true;
}

class IOHIDResourceDeviceUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOHIDResourceDeviceUserClient);

	uint16_t        fDevice;
	bool            fMade;
	unsigned        fReports;
	char            fProcess[32];

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOHIDResourceDeviceUserClient, IOUserClient);

static uint16_t gNextDevice = 1;

bool
IOHIDResourceDeviceUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	proc_selfname(fProcess, sizeof(fProcess));
	fDevice = gNextDevice++;
	hakios_hidres_say("hidres: IOHIDResource opened by %s (pid %d), type %u: device %u\n", fProcess, proc_selfpid(), (unsigned)type,
	    fDevice);
	return true;
}

IOReturn
IOHIDResourceDeviceUserClient::clientClose(void)
{
	if (fMade) {
		hakios_hidres_record(3, fDevice, NULL, 0);
		hakios_hidres_say("hidres: device %u gone (%u reports)\n", fDevice, fReports);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

IOReturn
IOHIDResourceDeviceUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    __unused IOExternalMethodDispatch * dispatch, __unused OSObject * target, __unused void * reference)
{
	const uint8_t * bytes = (const uint8_t *)args->structureInput;
	uint32_t length = args->structureInputSize;
	uint8_t * copied = NULL;
	IOReturn ret = kIOReturnSuccess;

	/* a large structure comes as a memory descriptor */
	if (args->structureInputDescriptor != NULL) {
		IOMemoryDescriptor * md = args->structureInputDescriptor;

		length = (uint32_t)md->getLength();
		if (length > 65536 || (copied = (uint8_t *)IOMalloc(length + 1)) == NULL) {
			return kIOReturnNoMemory;
		}
		if (md->prepare() != kIOReturnSuccess) {
			IOFree(copied, length + 1);
			return kIOReturnVMError;
		}
		md->readBytes(0, copied, length);
		md->complete();
		bytes = copied;
	}
	switch (selector) {
	case 0: {       /* create: the properties, XML */
		char * xml = (char *)IOMalloc(length + 1);
		OSObject * parsed = NULL;
		OSDictionary * properties;
		OSData * descriptor = NULL;
		OSString * product = NULL;
		OSNumber * vendor = NULL, * productID = NULL;

		if (xml == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		memcpy(xml, bytes, length);
		xml[length] = 0;
		parsed = OSUnserializeXML(xml);
		IOFree(xml, length + 1);
		properties = OSDynamicCast(OSDictionary, parsed);
		if (properties != NULL) {
			descriptor = OSDynamicCast(OSData, properties->getObject("ReportDescriptor"));
			product = OSDynamicCast(OSString, properties->getObject("Product"));
			vendor = OSDynamicCast(OSNumber, properties->getObject("VendorID"));
			productID = OSDynamicCast(OSNumber, properties->getObject("ProductID"));
		}
		hakios_hidres_say("hidres: device %u made by %s: \"%s\", vendor %#x product %#x, a report descriptor of %u bytes\n", fDevice,
		    fProcess, product ? product->getCStringNoCopy() : "?", vendor ? vendor->unsigned32BitValue() : 0,
		    productID ? productID->unsigned32BitValue() : 0, descriptor ? descriptor->getLength() : 0);
		if (descriptor != NULL) {
			hakios_hidres_record(1, fDevice, descriptor->getBytesNoCopy(), descriptor->getLength());
			fMade = true;
		} else {
			hakios_hidres_say("hidres: device %u: NO REPORT DESCRIPTOR in %u bytes of properties\n", fDevice, length);
		}
		OSSafeReleaseNULL(parsed);
		break;
	}
	case 1:         /* terminate */
		if (fMade) {
			hakios_hidres_record(3, fDevice, NULL, 0);
			fMade = false;
		}
		hakios_hidres_say("hidres: device %u terminated by %s\n", fDevice, fProcess);
		break;
	case 2:         /* an input report */
		if (fReports++ < 12) {
			char hex[3 * 16 + 1];
			unsigned n = 0;

			for (uint32_t i = 0; i < length && i < 16; i++) {
				n += (unsigned)snprintf(hex + n, sizeof(hex) - n, " %02x", bytes[i]);
			}
			hakios_hidres_say("hidres: device %u report %u, %u bytes:%s\n", fDevice, fReports, length, hex);
		}
		hakios_hidres_record(2, fDevice, bytes, length);
		break;
	case 3: case 4: case 5:
		break;
	default:
		hakios_hidres_say("hidres: device %u: selector %u from %s, not known (scalars %u, structure %u)\n", fDevice, selector, fProcess,
		    args->scalarInputCount, length);
		ret = kIOReturnUnsupported;
		break;
	}
	if (copied != NULL) {
		IOFree(copied, length + 1);
	}
	return ret;
}

void
HakiosHIDResourceStart(void)
{
	IOService * resources = IOService::getResourceService();
	IOHIDResource * made;

	if (resources == NULL || (made = OSTypeAlloc(IOHIDResource)) == NULL) {
		return;
	}
	if (!made->init() || !made->attach(resources)) {
		made->release();
		return;
	}
	if (!made->start(resources)) {
		made->detach(resources);
		made->release();
		return;
	}
	hakios_hidres_say("hidres: the service IOHIDResource is in the registry (for bluetoothd's HID devices)\n");
}
