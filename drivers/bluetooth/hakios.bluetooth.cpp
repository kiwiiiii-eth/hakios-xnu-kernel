/*
 * The Bluetooth controller as iOS looks for it in the registry. MobileBluetooth's BTSessionAttachWithQueue (the way
 * BluetoothManager, Settings' Bluetooth pane, gets to bluetoothd) answers error 9 -- and BluetoothManager.init returns nil,
 * the switch stays grey -- unless IOServiceGetMatchingService finds a service named "marconi-bt" or, failing that,
 * "bluetooth"; and bluetoothd's FindLocalMacAddress reads the phone's address from its "local-mac-address" (without it:
 * "No bluetooth on this device."). An iPhone's device tree has the node; this phone's loader gives none.
 * Made by hakios.bthci.c once the controller has answered as a controller.
 *
 * Taken from HakiOS's MediaTek branch (oosexgang/hakios-xnu-kernel, branch mtk: iokit/Kernel/RedwoodBluetooth.cpp).
 * Here the address is given as the Bluetooth address itself (the WCN3990 driver writes the same one into the controller).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>

class HakiosBluetooth : public IOService
{
	OSDeclareDefaultStructors(HakiosBluetooth);
};

#define super IOService
OSDefineMetaClassAndStructors(HakiosBluetooth, IOService);

extern "C" bool HakiosBluetoothNode(void);
extern "C" void HakiosBluetoothAddress(const uint8_t * bt);

static HakiosBluetooth * node;
static uint8_t address[6];
static bool have_address;

static void
publish_address(void)
{
	if (node != NULL && have_address) {
		OSData * data = OSData::withBytes(address, 6);

		if (data != NULL) {
			node->setProperty("local-mac-address", data);        /* what bluetoothd's FindLocalMacAddress reads */
			data->release();
		}
	}
}

/* The phone's Bluetooth address, in the order it is written (the first byte is the first of "aa:bb:..."). */
void
HakiosBluetoothAddress(const uint8_t * bt)
{
	memcpy(address, bt, 6);
	have_address = true;
	publish_address();
}

bool
HakiosBluetoothNode(void)
{
	IOService * platform = IOService::getPlatform();
	HakiosBluetooth * made;

	if (node != NULL) {
		return true;
	}
	if (platform == NULL || (made = OSTypeAlloc(HakiosBluetooth)) == NULL) {
		return false;
	}
	if (!made->init() || !made->attach(platform)) {
		made->release();
		return false;
	}
	made->setName("bluetooth");
	if (!made->start(platform)) {
		made->detach(platform);
		made->release();
		return false;
	}
	made->registerService();
	node = made;
	publish_address();
	IOLog("HakiosBluetooth: the service \"bluetooth\" is in the registry\n");
	return true;
}
