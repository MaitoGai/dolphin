// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include <stdio.h>
#include <stdlib.h>

#include <windows.h>
#include <dbt.h>
#include <setupapi.h>

#include "Common.h"
#include "WiimoteReal.h"

// Used for pair up
#undef NTDDI_VERSION
#define NTDDI_VERSION	NTDDI_WINXPSP2
#include <bthdef.h>
#include <BluetoothAPIs.h>
#pragma comment(lib, "Bthprops.lib")

typedef struct _HIDD_ATTRIBUTES
{
	ULONG   Size;
	USHORT  VendorID;
	USHORT  ProductID;
	USHORT  VersionNumber;
} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;

typedef VOID (__stdcall *PHidD_GetHidGuid)(LPGUID);
typedef BOOLEAN (__stdcall *PHidD_GetAttributes)(HANDLE, PHIDD_ATTRIBUTES);
typedef BOOLEAN (__stdcall *PHidD_SetOutputReport)(HANDLE, PVOID, ULONG);

PHidD_GetHidGuid HidD_GetHidGuid = NULL;
PHidD_GetAttributes HidD_GetAttributes = NULL;
PHidD_SetOutputReport HidD_SetOutputReport = NULL;

HINSTANCE hid_lib = NULL;

static int initialized = 0;

inline void init_lib()
{
	if (!initialized)
	{
		hid_lib = LoadLibrary(_T("hid.dll"));
		if (!hid_lib)
		{
			PanicAlert("Failed to load hid.dll");
			exit(EXIT_FAILURE);
		}

		HidD_GetHidGuid = (PHidD_GetHidGuid)GetProcAddress(hid_lib, "HidD_GetHidGuid");
		HidD_GetAttributes = (PHidD_GetAttributes)GetProcAddress(hid_lib, "HidD_GetAttributes");
		HidD_SetOutputReport = (PHidD_SetOutputReport)GetProcAddress(hid_lib, "HidD_SetOutputReport");
		if (!HidD_GetHidGuid || !HidD_GetAttributes || !HidD_SetOutputReport)
		{
			PanicAlert("Failed to load hid.dll");
			exit(EXIT_FAILURE);
		}

		initialized = true;
	}
}

// VID = Nintendo, PID = Wiimote
static int VIDLength = 3;
static int VID[3] = {0x057E, 0x0001, 0x0002};
static int PID[3] = {0x0306, 0x0002, 0x00F7};

namespace WiimoteReal
{

// Find and connect wiimotes.
// Does not replace already found wiimotes even if they are disconnected.
// wm is an array of max_wiimotes wiimotes
// Returns the total number of found and connected wiimotes.
int FindWiimotes(Wiimote** wm, int max_wiimotes)
{
	GUID device_id;
	HANDLE dev;
	HDEVINFO device_info;
	int found_wiimotes = 0;
	DWORD len;
	SP_DEVICE_INTERFACE_DATA device_data;
	PSP_DEVICE_INTERFACE_DETAIL_DATA detail_data = NULL;
	HIDD_ATTRIBUTES attr;

	init_lib();

	// Count the number of already found wiimotes
	for (int i = 0; i < MAX_WIIMOTES; ++i)
	{
		if (wm[i])
			found_wiimotes++;
	}

	device_data.cbSize = sizeof(device_data);

	// Get the device id
	HidD_GetHidGuid(&device_id);

	// Get all hid devices connected
	device_info = SetupDiGetClassDevs(&device_id, NULL, NULL, (DIGCF_DEVICEINTERFACE | DIGCF_PRESENT));

	for (int index = 0; found_wiimotes < max_wiimotes; ++index)
	{
		if (detail_data)
		{
			free(detail_data);
			detail_data = NULL;
		}

		// Query the next hid device info
		if (!SetupDiEnumDeviceInterfaces(device_info, NULL, &device_id, index, &device_data))
			break;

		// Get the size of the data block required
		SetupDiGetDeviceInterfaceDetail(device_info, &device_data, NULL, 0, &len, NULL);
		detail_data = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(len);
		detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		// Query the data for this device
		if (!SetupDiGetDeviceInterfaceDetail(device_info, &device_data, detail_data, len, NULL, NULL))
			continue;

		// Determine if this wiimote has already been found.
		bool found = false;
		for(int i = 0; i < MAX_WIIMOTES; i++)
		{
			if(wm[i] && memcmp(wm[i]->devicepath, detail_data->DevicePath, 197) == 0)
			{
				found = true;
				break;
			}
		}
		if (found)
			continue;

		// Open new device
		dev = CreateFile(detail_data->DevicePath,
				(GENERIC_READ | GENERIC_WRITE),
				(FILE_SHARE_READ | FILE_SHARE_WRITE),
				NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (dev == INVALID_HANDLE_VALUE)
			continue;

		// Get device attributes 
		attr.Size = sizeof(attr);
		HidD_GetAttributes(dev, &attr);

		bool foundWiimote = false;
		for (int i = 0; i < VIDLength; i++)
		{
			if (attr.VendorID == VID[i] && attr.ProductID == PID[i])
			{
				foundWiimote = true;
				break;
			}
		}

		if (foundWiimote)
		{
			// This is a wiimote
			// Find an unused slot
			unsigned int k = 0;
			for (; k < MAX_WIIMOTES && !(WIIMOTE_SRC_REAL & g_wiimote_sources[k] && !wm[k]); ++k);
			wm[k] = new Wiimote(k);
			wm[k]->dev_handle = dev;
			memcpy(wm[k]->devicepath, detail_data->DevicePath, 197);

			if (!wm[k]->Connect())
			{
				ERROR_LOG(WIIMOTE, "Unable to connect to wiimote %i.", wm[k]->index + 1);
				delete wm[k];
				wm[k] = NULL;
			}
			else
				++found_wiimotes;
		}
		else
		{
			// Not a wiimote 
			CloseHandle(dev);
		}
	}

	if (detail_data)
		free(detail_data);

	SetupDiDestroyDeviceInfoList(device_info);

	return found_wiimotes;
}

// Connect to a wiimote with a known device path.
bool Wiimote::Connect()
{
	if (IsConnected()) return false;

	if (!dev_handle)
	{
		dev_handle = CreateFile(devicepath,
				(GENERIC_READ | GENERIC_WRITE),
				(FILE_SHARE_READ | FILE_SHARE_WRITE),
				NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (dev_handle == INVALID_HANDLE_VALUE)
			return false;
	}

	hid_overlap.hEvent = CreateEvent(NULL, 1, 1, _T(""));
	hid_overlap.Offset = 0;
	hid_overlap.OffsetHigh = 0;

	m_connected = true;

	// Try a handshake to see if the device is actually connected
	if (!Handshake())
	{
		m_connected = false;
		return false;
	}

	// Set LEDs
	SetLEDs(WIIMOTE_LED_1 << index);

	NOTICE_LOG(WIIMOTE, "Connected to wiimote %i.", index + 1);

	return true;
}

void Wiimote::RealDisconnect()
{
	if (!IsConnected())
		return;

	m_connected = false;

	CloseHandle(dev_handle);
	dev_handle = 0;

	ResetEvent(&hid_overlap);
}

unsigned char *Wiimote::IORead()
{
	DWORD b, r;

	init_lib();

	if (!IsConnected())
		return NULL;

	unsigned char *buffer = new unsigned char[MAX_PAYLOAD];
	*buffer = 0;
	if (!ReadFile(dev_handle, buffer, sizeof(unsigned char) * MAX_PAYLOAD, &b, &hid_overlap))
	{
		// Partial read
		b = GetLastError();

		if ((b == ERROR_HANDLE_EOF) || (b == ERROR_DEVICE_NOT_CONNECTED))
		{
			// Remote disconnect
			RealDisconnect();
			delete[] buffer;
			return NULL;
		}

		r = WaitForSingleObject(hid_overlap.hEvent, WIIMOTE_DEFAULT_TIMEOUT);
		if (r == WAIT_TIMEOUT)
		{
			// Timeout - cancel and continue

			if (*buffer)
				WARN_LOG(WIIMOTE, "Packet ignored.  This may indicate a problem (timeout is %i ms).",
						WIIMOTE_DEFAULT_TIMEOUT);

			CancelIo(dev_handle);
			ResetEvent(hid_overlap.hEvent);
			delete[] buffer;
			return NULL;
		}
		else if (r == WAIT_FAILED)
		{
			WARN_LOG(WIIMOTE, "A wait error occured on reading from wiimote %i.", index + 1);
			delete[] buffer;
			return NULL;
		}

		if (!GetOverlappedResult(dev_handle, &hid_overlap, &b, 0))
		{
			delete[] buffer;
			return NULL;
		}
	}

	// This needs to be done even if ReadFile fails, essential during init
	// Move the data over one, so we can add back in data report indicator byte (here, 0xa1)
	memmove(buffer + 1, buffer, sizeof(unsigned char) * MAX_PAYLOAD - 1);
	buffer[0] = 0xa1;

	ResetEvent(hid_overlap.hEvent);
	return buffer;
}

int Wiimote::IOWrite(unsigned char* buf, int len)
{
	DWORD bytes, dw;
	int i;

	init_lib();

	if (!IsConnected())
		return NULL;

	switch (stack)
	{
		case MSBT_STACK_UNKNOWN:
			{
				// Try to auto-detect the stack type
				if (i = WriteFile(dev_handle, buf + 1, 22, &bytes, &hid_overlap))
				{
					// Bluesoleil will always return 1 here, even if it's not connected
					stack = MSBT_STACK_BLUESOLEIL;
					return i;
				}

				if (i = HidD_SetOutputReport(dev_handle, buf + 1, len - 1))
				{
					stack = MSBT_STACK_MS;
					return i;
				}

#if 0
				dw = GetLastError();
				// Checking for 121 = timeout on semaphore/device off/disconnected to
				// avoid trouble with other stacks toshiba/widcomm 
				// 995 = The I/O operation has been aborted because of a thread exit or
				// an application request.

				if ( (dw == 121) || (dw == 995) )
				{
					NOTICE_LOG(WIIMOTE, "IOWrite[MSBT_STACK_UNKNOWN]");
					RealDisconnect();
				}
				else ERROR_LOG(WIIMOTE,
						"IOWrite[MSBT_STACK_UNKNOWN]: ERROR: %08x", dw); 
#endif


				// If the part below causes trouble on WIDCOMM/TOSHIBA stack uncomment
				// the lines above, and comment out the 3 lines below instead.

				NOTICE_LOG(WIIMOTE, "IOWrite[MSBT_STACK_UNKNOWN]");
				RealDisconnect();
				return 0;
			}

		case MSBT_STACK_MS:
			i = HidD_SetOutputReport(dev_handle, buf + 1, len - 1);
			dw = GetLastError();

			if (dw == 121)
			{
				// Semaphore timeout
				NOTICE_LOG(WIIMOTE, "WiimoteIOWrite[MSBT_STACK_MS]:  Unable to send data to wiimote");
				RealDisconnect();
				return 0;
			}
			return i;

		case MSBT_STACK_BLUESOLEIL:
			return WriteFile(dev_handle, buf + 1, 22, &bytes, &hid_overlap);
	}

	return 0;
}

int UnPair()
{
	// TODO:
	return 0;
}

// WiiMote Pair-Up, function will return amount of either new paired or unpaired devices
// negative number on failure
int PairUp(bool unpair)
{
	int nPaired = 0;

	BLUETOOTH_DEVICE_SEARCH_PARAMS srch;
	srch.dwSize = sizeof(srch);
	srch.fReturnAuthenticated = true;
	srch.fReturnRemembered = true;
	// Does not filter properly somehow, so we need to do an additional check on
	// fConnected BT Devices
	srch.fReturnConnected = true;
	srch.fReturnUnknown = true;
	srch.fIssueInquiry = true;
	srch.cTimeoutMultiplier = 2;	// == (2 * 1.28) seconds

	BLUETOOTH_FIND_RADIO_PARAMS radioParam;
	radioParam.dwSize = sizeof(radioParam);

	HANDLE hRadio;

	// Enumerate BT radios
	HBLUETOOTH_RADIO_FIND hFindRadio = BluetoothFindFirstRadio(&radioParam, &hRadio);

	if (NULL == hFindRadio)
		return -1;

	while (hFindRadio)
	{
		BLUETOOTH_RADIO_INFO radioInfo;
		radioInfo.dwSize = sizeof(radioInfo);

		// TODO: check for SUCCEEDED()
		BluetoothGetRadioInfo(hRadio, &radioInfo);

		srch.hRadio = hRadio;

		BLUETOOTH_DEVICE_INFO btdi;
		btdi.dwSize = sizeof(btdi);

		// Enumerate BT devices
		HBLUETOOTH_DEVICE_FIND hFindDevice = BluetoothFindFirstDevice(&srch, &btdi);
		while (hFindDevice)
		{
			// btdi.szName is sometimes missings it's content - it's a bt feature..
			DEBUG_LOG(WIIMOTE, "authed %i connected %i remembered %i ",
					btdi.fAuthenticated, btdi.fConnected, btdi.fRemembered);

			// TODO: Probably could just check for "Nintendo RVL"
			if (0 == wcscmp(btdi.szName, L"Nintendo RVL-WBC-01") ||
					0 == wcscmp(btdi.szName, L"Nintendo RVL-CNT-01"))
			{
				if (unpair)
				{
					if (SUCCEEDED(BluetoothRemoveDevice(&btdi.Address)))
					{
						NOTICE_LOG(WIIMOTE,
								"Pair-Up: Automatically removed BT Device on shutdown: %08x",
								GetLastError());
						++nPaired;
					}
				}
				else
				{
					if (false == btdi.fConnected)
					{
						// TODO: improve the read of the BT driver, esp. when batteries
						// of the wiimote are removed while being fConnected
						if (btdi.fRemembered)
						{
							// Make Windows forget old expired pairing.  We can pretty
							// much ignore the return value here.  It either worked
							// (ERROR_SUCCESS), or the device did not exist
							// (ERROR_NOT_FOUND).  In both cases, there is nothing left.
							BluetoothRemoveDevice(&btdi.Address);
						}

						// Activate service
						const DWORD hr = BluetoothSetServiceState(hRadio, &btdi,
								&HumanInterfaceDeviceServiceClass_UUID, BLUETOOTH_SERVICE_ENABLE);
						if (SUCCEEDED(hr))
							++nPaired;
						else
							ERROR_LOG(WIIMOTE, "Pair-Up: BluetoothSetServiceState() returned %08x", hr);
					}
				}
			}

			if (false == BluetoothFindNextDevice(hFindDevice, &btdi))
			{
				BluetoothFindDeviceClose(hFindDevice);
				hFindDevice = NULL;
			}
		}

		if (false == BluetoothFindNextRadio(hFindRadio, &hRadio))
		{
			BluetoothFindRadioClose(hFindRadio);
			hFindRadio = NULL;
		}
	}

	return nPaired;
}

};
