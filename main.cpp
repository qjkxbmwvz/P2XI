#define POSITIONAL 0
#define DRIVING 0

#undef USB

#pragma comment(lib, "hid")
#pragma comment(lib, "dxguid")
#pragma comment(lib, "dinput8")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "Setupapi")

#define STRICT
#define DIRECTINPUT_VERSION 0x0800
#define _CRT_SECURE_NO_DEPRECATE
#ifndef _WIN32_DCOM
#define _WIN32_DCOM
#endif

#include <iostream> // cout
#include <thread> // this_thread::sleep_for, this_thread::yield
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <conio.h> // _kbhit, _getch_nolock
#include <Dbt.h>
#include "hidsdi.h"
#include <SetupAPI.h>

#include <tchar.h>
#include <basetyps.h>
#include <strsafe.h>
#include <INITGUID.H>
#include <commctrl.H>

#pragma warning(push)
#pragma warning(disable:6000 28251)
#include <dinput.h>
#pragma warning(pop)

#include <dinputd.h>

#include "XOutput.hpp"

#include "resource.h"
#include "hidapi.h"

#define SAFE_DELETE(p)  { if(p) { delete (p);     (p)=nullptr; } }
#define SAFE_RELEASE(p) { if(p) { (p)->Release(); (p)=nullptr; } }

using tstring = std::wstring;
using bytes = std::vector<std::uint8_t>;

std::mutex controller_map_mutex;
LPDIRECTINPUT8          g_p_di = nullptr;
LPDIRECTINPUTDEVICE8    g_p_joystick = nullptr;

XINPUT_GAMEPAD xinState;

struct DI_ENUM_CONTEXT {
	DIJOYCONFIG* pPreferredJoyCfg;
	bool bPreferredJoyCfgValid;
};

struct {
	std::string path;
	std::uint8_t counter;
	HANDLE handle;
	USHORT output_size;
	UCHAR large_motor, small_motor, led;
	bool vibrate, led_changed;
	unsigned char max;
	bool connected;
} controller;

#if DRIVING
LPDIRECTINPUTDEVICE8 g_p_joystick2 = nullptr;
#endif

namespace {
	bool has_broken {false};

	void unset_break_handler();
	BOOL __stdcall break_handler(const DWORD type) {
		switch (type) {
		case CTRL_C_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_BREAK_EVENT:
			has_broken = true;
			unset_break_handler();
			return TRUE;
		default:
			return FALSE;
		}
	}

	void unset_break_handler() {
		SetConsoleCtrlHandler(break_handler, FALSE);
	}

	constexpr bool success(const DWORD e) {
		return e == XOutput::XOUTPUT_SUCCESS;
	}
}

GUID hid_guid;

void get_initial_plugged_devices() {
	using std::unique_ptr;

	const auto devices = SetupDiGetClassDevs(&hid_guid, nullptr,
			nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	
	if (devices != INVALID_HANDLE_VALUE) {
		DWORD i = 0;
		SP_DEVICE_INTERFACE_DATA data;
		data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

		while (SetupDiEnumDeviceInterfaces(devices,
				nullptr, &hid_guid, i++, &data)) {
			DWORD required_buffer_size;
			const auto result = SetupDiGetDeviceInterfaceDetail(devices, &data,
					nullptr, 0, &required_buffer_size, nullptr);

			if (result == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
				unique_ptr<SP_DEVICE_INTERFACE_DETAIL_DATA>
						interface_detail(static_cast<
								PSP_DEVICE_INTERFACE_DETAIL_DATA>(
										operator new(required_buffer_size)));
				interface_detail->cbSize
						= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
				
				if (SetupDiGetDeviceInterfaceDetail(devices, &data,
													interface_detail.get(),
						required_buffer_size, nullptr, nullptr)) {
					std::lock_guard<std::mutex> lk(controller_map_mutex);
					controller.counter = 0;
					controller.path = interface_detail->DevicePath;
					controller.handle = CreateFile(
							controller.path.c_str(),
							GENERIC_WRITE | GENERIC_READ,
							FILE_SHARE_WRITE | FILE_SHARE_READ,
							nullptr, OPEN_EXISTING,
							FILE_FLAG_OVERLAPPED,
							nullptr);
					
					if (controller.handle == INVALID_HANDLE_VALUE) {
						const auto err = GetLastError();

						if (err != ERROR_ACCESS_DENIED) {
							std::cerr << "error opening " << controller.path;
							std::cerr << " (" << GetLastError() << ")"
									  << std::endl;
						}
						continue;
					}
					
					HIDD_ATTRIBUTES attributes;
					
					attributes.Size = sizeof attributes;
					auto ok = HidD_GetAttributes(
							controller.handle, &attributes);
					
					if (!ok) {
						std::cerr << "Error calling HidD_GetAttributes ("
								  << GetLastError() << ")" << std::endl;
						continue;
					}

					if (attributes.ProductID != 0x2009
					 || attributes.VendorID != 0x057E) {
						// not a pro controller, fail silently
						continue;
					}

					PHIDP_PREPARSED_DATA preparsed_data;
					
					ok = HidD_GetPreparsedData(controller.handle,
											   &preparsed_data);
					
					if (!ok) {
						std::cerr << "Error calling HidD_GetPreparsedData ("
								  << GetLastError() << ")" << std::endl;
						return;
					}

					HIDP_CAPS caps;
					const auto status = HidP_GetCaps(preparsed_data, &caps);
					
					HidD_FreePreparsedData(preparsed_data);

					if (status != HIDP_STATUS_SUCCESS) {
						std::cerr << "Error calling HidP_GetCaps ("
								  << status << ")" << std::endl;
						return;
					}

					controller.output_size = caps.OutputReportByteLength;
					controller.connected = true;
					
					XOutput::XOutputPlugIn(0);
				}
			}
			return;
		}
	}
	SetupDiDestroyDeviceInfoList(devices);
}

bool check_io_error(const DWORD err) {
	auto ret = true;

	switch (err) {
	case ERROR_DEVICE_NOT_CONNECTED:
	case ERROR_OPERATION_ABORTED:
		// not fatal
		ret = false;
		break;
	default:
		break;
	}

	return ret;
}

void write_data(const bytes& data) {
	bytes buf;

	if (data.size() < controller.output_size) {
		buf.resize(controller.output_size);
		copy(data.begin(), data.end(), buf.begin());
	} else
		buf = data;

	DWORD tmp;
	OVERLAPPED ol = {0};
	ol.hEvent = CreateEvent(nullptr,
			FALSE, FALSE, TEXT(""));

	WriteFile(controller.handle, buf.data(),
			static_cast<DWORD>(buf.size()), &tmp, &ol);

	CloseHandle(ol.hEvent);
}

void handle_rumble() {
	using std::uint8_t;
	if (controller.led_changed) {
		const bytes buf = {0x01, static_cast<uint8_t>(controller.counter++ & 0x0F),
					 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40,
					 0x30, static_cast<unsigned char>(1 << controller.led - 1)};
		
		write_data(buf);
		controller.led_changed = false;
	}
	if (controller.vibrate) {
		bytes buf = {0x10, static_cast<uint8_t>(controller.counter++ & 0x0F),
					 0x80, 0x00, 0x40, 0x40, 0x80, 0x00, 0x40, 0x40};
		
		buf[2] = 0x08;
		buf[3] = controller.large_motor;
		buf[6] = 0x08;
		buf[7] = controller.large_motor;
		
		write_data(buf);
		
		buf[1] = static_cast<uint8_t>(controller.counter++ & 0x0F);
		buf[2] = 0x10;
		buf[3] = controller.small_motor;
		buf[6] = 0x10;
		buf[7] = controller.small_motor;
		
		write_data(buf);
	}
}

BOOL WINAPI ctrl_handler(const DWORD _In_ event) {
	using std::exit;

	if (event == CTRL_CLOSE_EVENT ||
		event == CTRL_C_EVENT ||
		event == CTRL_BREAK_EVENT) {
		exit(0);
	}

	return FALSE;
}

BOOL CALLBACK enum_joysticks_callback(const DIDEVICEINSTANCE* pdid_instance,
									VOID* p_context) {
#if DRIVING
	static bool pedals_found {false};
	static bool procon_found {false};
#endif
	HRESULT hr;

#if DRIVING
	if (pdid_instance->guidProduct.Data1 == 0xBEAD1234) {
		hr = g_p_di->CreateDevice(pdid_instance->guidInstance,
								  &g_p_joystick2, nullptr);
		pedals_found = true;
		
		if (procon_found)
			return DIENUM_STOP;
		else
			return DIENUM_CONTINUE;
	}
#endif

	if (pdid_instance->guidProduct.Data1 != 0x2009057E)
		return DIENUM_CONTINUE;

	// Obtain an interface to the enumerated joystick.
	hr = g_p_di->CreateDevice(pdid_instance->guidInstance,
							  &g_p_joystick, nullptr);
#if DRIVING
	procon_found = true;
#endif
	
	// If it failed, then we can't use this joystick. (Maybe the user unplugged
	// it while we were in the middle of enumerating it.)
	if (FAILED(hr))
		return DIENUM_CONTINUE;

	// Stop enumeration. Note: we're just taking the first joystick we get. You
	// could store all the enumerated joysticks and let the user pick.
#if DRIVING
	if (!pedals_found || !procon_found)
		return DIENUM_CONTINUE;
	else
#endif
	return DIENUM_STOP;
}

BOOL CALLBACK enum_objects_callback(const DIDEVICEOBJECTINSTANCE* pdidoi,
								  VOID* p_context) {
	const auto h_dlg = static_cast<HWND>(p_context);
	
	static int n_slider_count = 0;  // Number of returned slider controls
	static int n_pov_count = 0;     // Number of returned POV controls

	// Set the UI to reflect what objects the joystick supports
	if (pdidoi->guidType == GUID_XAxis) {
		EnableWindow(GetDlgItem(h_dlg, IDC_X_AXIS), TRUE);
		EnableWindow(GetDlgItem(h_dlg, IDC_X_AXIS_TEXT), TRUE);
	}
	if (pdidoi->guidType == GUID_YAxis) {
		EnableWindow(GetDlgItem(h_dlg, IDC_Y_AXIS), TRUE);
		EnableWindow(GetDlgItem(h_dlg, IDC_Y_AXIS_TEXT), TRUE);
	}
	if (pdidoi->guidType == GUID_ZAxis) {
		EnableWindow(GetDlgItem(h_dlg, IDC_Z_AXIS), TRUE);
		EnableWindow(GetDlgItem(h_dlg, IDC_Z_AXIS_TEXT), TRUE);
	}
	if (pdidoi->guidType == GUID_RxAxis) {
		EnableWindow(GetDlgItem(h_dlg, IDC_X_ROT), TRUE);
		EnableWindow(GetDlgItem(h_dlg, IDC_X_ROT_TEXT), TRUE);
	}
	if (pdidoi->guidType == GUID_RyAxis) {
		EnableWindow(GetDlgItem(h_dlg, IDC_Y_ROT), TRUE);
		EnableWindow(GetDlgItem(h_dlg, IDC_Y_ROT_TEXT), TRUE);
	}
	if (pdidoi->guidType == GUID_RzAxis) {
		EnableWindow(GetDlgItem(h_dlg, IDC_Z_ROT), TRUE);
		EnableWindow(GetDlgItem(h_dlg, IDC_Z_ROT_TEXT), TRUE);
	}
	if (pdidoi->guidType == GUID_Slider) {
		switch (n_slider_count++) {
		case 0:
			EnableWindow(GetDlgItem(h_dlg, IDC_SLIDER0), TRUE);
			EnableWindow(GetDlgItem(h_dlg, IDC_SLIDER0_TEXT), TRUE);
			break;

		case 1:
			EnableWindow(GetDlgItem(h_dlg, IDC_SLIDER1), TRUE);
			EnableWindow(GetDlgItem(h_dlg, IDC_SLIDER1_TEXT), TRUE);
			break;
		default: ;
		}
	}
	if (pdidoi->guidType == GUID_POV) {
		switch (n_pov_count++) {
		case 0:
			EnableWindow(GetDlgItem(h_dlg, IDC_POV0), TRUE);
			EnableWindow(GetDlgItem(h_dlg, IDC_POV0_TEXT), TRUE);
			break;

		case 1:
			EnableWindow(GetDlgItem(h_dlg, IDC_POV1), TRUE);
			EnableWindow(GetDlgItem(h_dlg, IDC_POV1_TEXT), TRUE);
			break;

		case 2:
			EnableWindow(GetDlgItem(h_dlg, IDC_POV2), TRUE);
			EnableWindow(GetDlgItem(h_dlg, IDC_POV2_TEXT), TRUE);
			break;

		case 3:
			EnableWindow(GetDlgItem(h_dlg, IDC_POV3), TRUE);
			EnableWindow(GetDlgItem(h_dlg, IDC_POV3_TEXT), TRUE);
			break;
		default: ;
		}
	}

	return DIENUM_CONTINUE;
}

HRESULT init_direct_input(const HWND h_dlg) {
	HRESULT hr;

	// Register with the DirectInput subsystem and get a pointer
	// to a IDirectInput interface we can use.
	// Create a DInput object
	if (FAILED(hr = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
		IID_IDirectInput8, (VOID**)&g_p_di, nullptr)))
		return hr;

	DI_ENUM_CONTEXT enum_context;
	IDirectInputJoyConfig8* p_joy_config = nullptr;
	if (FAILED(hr = g_p_di->QueryInterface(IID_IDirectInputJoyConfig8, (void**)&p_joy_config)))
		return hr;
	
	SAFE_RELEASE(p_joy_config);

	// Look for a simple joystick we can use for this sample program.
	if (FAILED(hr = g_p_di->EnumDevices(DI8DEVCLASS_GAMECTRL,
										enum_joysticks_callback,
										&enum_context, DIEDFL_ATTACHEDONLY)))
		return hr;

	// Make sure we got a joystick
	if (!g_p_joystick) {
		MessageBox(nullptr, TEXT("Joystick not found. The sample will now exit."),
			TEXT("DirectInput Sample"),
			MB_ICONERROR | MB_OK);
		EndDialog(h_dlg, 0);
		return S_OK;
	}

	// Set the data format to "simple joystick" - a predefined data format 
	//
	// A data format specifies which controls on a device we are interested in,
	// and how they should be reported. This tells DInput that we will be
	// passing a DIJOYSTATE2 structure to IDirectInputDevice::GetDeviceState().
	if (FAILED(hr = g_p_joystick->SetDataFormat(&c_dfDIJoystick2)))
		return hr;
#if DRIVING
	if (FAILED(hr = g_p_joystick2->SetDataFormat(&c_dfDIJoystick2)))
		return hr;
#endif

	// Set the cooperative level to let DInput know how this device should
	// interact with the system and with other DInput applications.
	if (FAILED(hr = g_p_joystick->SetCooperativeLevel(
			h_dlg, DISCL_EXCLUSIVE | DISCL_BACKGROUND)))
		return hr;
#if DRIVING
	if (FAILED(hr = g_p_joystick2->SetCooperativeLevel(
			hDlg, DISCL_EXCLUSIVE | DISCL_BACKGROUND)))
		return hr;
#endif

	// Enumerate the joystick objects. The callback function enabled user
	// interface elements for objects that are found, and sets the min/max
	// values property for discovered axes.
	if (FAILED(hr = g_p_joystick->EnumObjects(enum_objects_callback,
		(VOID*)h_dlg, DIDFT_ALL)))
		return hr;
#if DRIVING
	if (FAILED(hr = g_p_joystick2->EnumObjects(EnumObjectsCallback,
		(VOID*)hDlg, DIDFT_ALL)))
		return hr;
#endif

	return S_OK;
}

bytes read_data() {
	bytes buf;
	buf.resize(49);

	DWORD tmp;
	OVERLAPPED ol = {0};
	ol.hEvent = CreateEvent(nullptr, FALSE, FALSE, TEXT(""));

	ReadFile(controller.handle, buf.data(), static_cast<DWORD>(buf.size()), &tmp, &ol);
	
	CloseHandle(ol.hEvent);
	
	return buf;
}

HRESULT update_input_state(const HWND h_dlg) {
	TCHAR str_text[512] = {0}; // Device state text
	DIJOYSTATE2 js;           // DInput joystick state

	if (!g_p_joystick)
		return S_OK;

	// Poll the device to read the current state
	auto hr = g_p_joystick->Poll();
	if (FAILED(hr)) {
		// DInput is telling us that the input stream has been
		// interrupted. We aren't tracking any state between polls, so
		// we don't have any special reset that needs to be done. We
		// just re-acquire and try again.
		hr = g_p_joystick->Acquire();
		while (hr == DIERR_INPUTLOST)
			hr = g_p_joystick->Acquire();

		// hr may be DIERR_OTHERAPPHASPRIO or other errors.  This
		// may occur when the app is minimized or in the process of 
		// switching, so just try again later 
		return S_OK;
	}

	// Get the input's device state
	if (FAILED(hr = g_p_joystick->GetDeviceState(sizeof(DIJOYSTATE2), &js)))
		return hr; // The device should have been acquired during the Poll()
	
	// Display joystick state to dialog

	// Axes
	_stprintf_s(str_text, 512, TEXT("%04hX"), static_cast<short>(js.lX));
	SetWindowText(GetDlgItem(h_dlg, IDC_X_AXIS), str_text);
	_stprintf_s(str_text, 512, TEXT("%04hX"), static_cast<short>(js.lY));
	SetWindowText(GetDlgItem(h_dlg, IDC_Y_AXIS), str_text);
	_stprintf_s(str_text, 512, TEXT("%04hX"), static_cast<short>(js.lZ));
	SetWindowText(GetDlgItem(h_dlg, IDC_Z_AXIS), str_text);
	_stprintf_s(str_text, 512, TEXT("%04hX"), static_cast<short>(js.lRx));
	SetWindowText(GetDlgItem(h_dlg, IDC_X_ROT), str_text);
	_stprintf_s(str_text, 512, TEXT("%04hX"), static_cast<short>(js.lRy));
	SetWindowText(GetDlgItem(h_dlg, IDC_Y_ROT), str_text);
	_stprintf_s(str_text, 512, TEXT("%04hX"), static_cast<short>(js.lRz));
	SetWindowText(GetDlgItem(h_dlg, IDC_Z_ROT), str_text);

	// Slider controls
	_stprintf_s(str_text, 512, TEXT("%04X"), static_cast<short>(js.rglSlider[0]));
	SetWindowText(GetDlgItem(h_dlg, IDC_SLIDER0), str_text);
	_stprintf_s(str_text, 512, TEXT("%04hX"), static_cast<short>(js.rglSlider[1]));
	SetWindowText(GetDlgItem(h_dlg, IDC_SLIDER1), str_text);

	// Points of view
	_stprintf_s(str_text, 512, TEXT("%lu"), js.rgdwPOV[0]);
	SetWindowText(GetDlgItem(h_dlg, IDC_POV0), str_text);
	_stprintf_s(str_text, 512, TEXT("%lu"), js.rgdwPOV[1]);
	SetWindowText(GetDlgItem(h_dlg, IDC_POV1), str_text);
	_stprintf_s(str_text, 512, TEXT("%lu"), js.rgdwPOV[2]);
	SetWindowText(GetDlgItem(h_dlg, IDC_POV2), str_text);
	_stprintf_s(str_text, 512, TEXT("%lu"), js.rgdwPOV[3]);
	SetWindowText(GetDlgItem(h_dlg, IDC_POV3), str_text);

	// Fill up text with which buttons are pressed
	_tcscpy_s(str_text, 512, TEXT(""));
	for (int i = 0; i < 128; i++) {
		if (js.rgbButtons[i] & 0x80) {
			TCHAR sz[128];
			_stprintf_s(sz, 128, TEXT("%02d "), i);
			_tcscat_s(str_text, 512, sz);
		}
	}

	SetWindowText(GetDlgItem(h_dlg, IDC_BUTTONS), str_text);

	xinState.wButtons = 0;

#ifndef USB
	switch (js.rgdwPOV[0]) {
	case     0:
		xinState.wButtons |= 0x1;
		break;
	case  4500:
		xinState.wButtons |= 0x9;
		break;
	case  9000:
		xinState.wButtons |= 0x8;
		break;
	case 13500:
		xinState.wButtons |= 0xA;
		break;
	case 18000:
		xinState.wButtons |= 0x2;
		break;
	case 22500:
		xinState.wButtons |= 0x6;
		break;
	case 27000:
		xinState.wButtons |= 0x4;
		break;
	case 31500:
		xinState.wButtons |= 0x5;
		break;
	default:
		break;
	}
	if (js.rgbButtons[9])
		xinState.wButtons |= 0x0010;
	if (js.rgbButtons[8])
		xinState.wButtons |= 0x0020;
	if (js.rgbButtons[10])
		xinState.wButtons |= 0x0040;
	if (js.rgbButtons[11])
		xinState.wButtons |= 0x0080;
	if (js.rgbButtons[4])
		xinState.wButtons |= 0x0100;
	if (js.rgbButtons[5])
		xinState.wButtons |= 0x0200;
	if (js.rgbButtons[12])
		xinState.wButtons |= 0x0400;
#if POSITIONAL
	if (js.rgbButtons[0])
		xinState.wButtons |= 0x1000;
	if (js.rgbButtons[1])
		xinState.wButtons |= 0x2000;
	if (js.rgbButtons[2])
		xinState.wButtons |= 0x4000;
	if (js.rgbButtons[3])
		xinState.wButtons |= 0x8000;
#else
	if (js.rgbButtons[1])
		xinState.wButtons |= 0x1000;
	if (js.rgbButtons[0])
		xinState.wButtons |= 0x2000;
	if (js.rgbButtons[3])
		xinState.wButtons |= 0x4000;
	if (js.rgbButtons[2])
		xinState.wButtons |= 0x8000;
#endif

#if DRIVING
	HRESULT hr2 = g_p_joystick2->Poll();
	if (FAILED(hr2)) {
		hr2 = g_p_joystick2->Acquire();
		while (hr2 == DIERR_INPUTLOST)
			hr2 = g_p_joystick2->Acquire();
		
		return S_OK;
	}
	DIJOYSTATE2 js2;

	hr2 = g_p_joystick2->GetDeviceState(sizeof(DIJOYSTATE2), &js2);
	double x = js2.lX - 32767, y = js2.lY - 32767;
	double l_1, l_2, l_3;

	l_1 = (32767 - y) / 32767;
	l_2 = (y - x) / 16383;
	l_3 = 1 - l_1 - l_2;
	
	if (l_2 > 0.5625)
		xinState.bLeftTrigger = 255;
	else if (l_2 < 0)
		xinState.bLeftTrigger = 0;
	else
		xinState.bLeftTrigger = l_2 * 453;
	if (l_3 > 0.5625)
		xinState.bRightTrigger = 255;
	else if (l_3 < 0)
		xinState.bRightTrigger = 0;
	else
		xinState.bRightTrigger = l_3 * 453;
	
	static bool zl_pressed {false};
	
	if (js.rgbButtons[6])
		if (!zl_pressed) {
			INPUT ip[1];

			ip[0].type = INPUT_KEYBOARD;
			ip[0].ki.wScan = 0;
			ip[0].ki.time = 0;
			ip[0].ki.dwExtraInfo = 0;
			ip[0].ki.wVk = 0x52;
			ip[0].ki.dwFlags = 0;
			SendInput(1, ip, sizeof(INPUT));
			
			zl_pressed = true;
		}
	else
		if (zl_pressed) {
			INPUT ip[1];
			
			ip[0].ki.dwFlags = KEYEVENTF_KEYUP;
			SendInput(1, ip, sizeof(INPUT));
			
			zl_pressed = false;
		}
#else
	if (js.rgbButtons[ 6])
		xinState.bLeftTrigger = 255;
	else
		xinState.bLeftTrigger = 0;
	if (js.rgbButtons[7])
		xinState.bRightTrigger = 255;
	else
		xinState.bRightTrigger = 0;
#endif
	
	static short capturing {0};
	if (js.rgbButtons[13])
		++capturing;
	else if (capturing >= 30) {
		capturing = 0;
		INPUT ip[2];
		
		ip[0].type = INPUT_KEYBOARD;
		ip[0].ki.wScan = 0;
		ip[0].ki.time = 0;
		ip[0].ki.dwExtraInfo = 0;
		ip[0].ki.wVk = VK_MENU;
		ip[0].ki.dwFlags = 0;
		ip[1].type = INPUT_KEYBOARD;
		ip[1].ki.wScan = 0;
		ip[1].ki.time = 0;
		ip[1].ki.dwExtraInfo = 0;
		ip[1].ki.wVk = VK_F10;
		ip[1].ki.dwFlags = 0;
		SendInput(2, ip, sizeof(INPUT));
		ip[0].ki.dwFlags = KEYEVENTF_KEYUP;
		ip[1].ki.dwFlags = KEYEVENTF_KEYUP;
		SendInput(2, ip, sizeof(INPUT));
	} else if (capturing > 0) {
		capturing = 0;
		INPUT ip[2];

		ip[0].type = INPUT_KEYBOARD;
		ip[0].ki.wScan = 0;
		ip[0].ki.time = 0;
		ip[0].ki.dwExtraInfo = 0;
		ip[0].ki.wVk = VK_MENU;
		ip[0].ki.dwFlags = 0;
		ip[1].type = INPUT_KEYBOARD;
		ip[1].ki.wScan = 0;
		ip[1].ki.time = 0;
		ip[1].ki.dwExtraInfo = 0;
		ip[1].ki.wVk = VK_F1;
		ip[1].ki.dwFlags = 0;
		SendInput(2, ip, sizeof(INPUT));
		ip[0].ki.dwFlags = KEYEVENTF_KEYUP;
		ip[1].ki.dwFlags = KEYEVENTF_KEYUP;
		SendInput(2, ip, sizeof(INPUT));
	}
	
	static unsigned min {0}, max {0};
	
	xinState.sThumbLX = 0x8000 + static_cast<short>(js.lX);
	xinState.sThumbLY = 0x7FFF - static_cast<short>(js.lY);
	xinState.sThumbRX = 0x8000 + static_cast<short>(js.lRx);
	xinState.sThumbRY = 0x7FFF - static_cast<short>(js.lRy);
#else
	if (js.lY & 0x02)
		xinState.wButtons |= 0x0001;
	if (js.lY & 0x01)
		xinState.wButtons |= 0x0002;
	if (js.lY & 0x08)
		xinState.wButtons |= 0x0004;
	if (js.lY & 0x04)
		xinState.wButtons |= 0x0008;
	if ((js.lX - 0x7FFF) & 0x0200)
		xinState.wButtons |= 0x0010;
	if ((js.lX - 0x7FFF) & 0x0100)
		xinState.wButtons |= 0x0020;
	if ((js.lX - 0x7FFF) & 0x0800)
		xinState.wButtons |= 0x0040;
	if ((js.lX - 0x7FFF) & 0x0400)
		xinState.wButtons |= 0x0080;
	if (js.lY & 0x40)
		xinState.wButtons |= 0x0100;
	if ((js.lX - 0x7FFF) & 0x0040)
		xinState.wButtons |= 0x0200;
	if ((js.lX - 0x7FFF) & 0x1000)
		xinState.wButtons |= 0x0400;
	if ((js.lX - 0x7FFF) & 0x0008)
		xinState.wButtons |= 0x1000;
	if ((js.lX - 0x7FFF) & 0x0004)
		xinState.wButtons |= 0x2000;
	if ((js.lX - 0x7FFF) & 0x0002)
		xinState.wButtons |= 0x4000;
	if ((js.lX - 0x7FFF) & 0x0001)
		xinState.wButtons |= 0x8000;

	if (js.lY & 0x80)
		xinState.bLeftTrigger = 65535;
	else
		xinState.bLeftTrigger = 0;
	if ((js.lX - 0x7FFF) & 0x0080)
		xinState.bRightTrigger = 65535;
	else
		xinState.bRightTrigger = 0;
#endif
	
	XOutput::XOutputSetState(0, &xinState);

	UCHAR a {0}, b {0}, c {0}, d {0};
	
	XOutput::XOutputGetState(0, &a, &b, &c, &d);

	if (controller.max != 255) {
		b = static_cast<unsigned>(b) * static_cast<unsigned>(controller.max) / 255;
		c = static_cast<unsigned>(c) * static_cast<unsigned>(controller.max) / 255;
	}
	
	controller.vibrate = a;
	controller.large_motor = b;
	controller.small_motor = c;
	if (controller.led != d + 1) {
		controller.led = d + 1;
		controller.led_changed = true;
	}
	handle_rumble();

	auto buf = read_data();
	
	return S_OK;
}

VOID free_direct_input() {
	// Unacquire the device one last time just in case 
	// the app tried to exit while the device is still acquired.
	if (g_p_joystick)
		g_p_joystick->Unacquire();

	// Release any DirectInput objects.
	SAFE_RELEASE(g_p_joystick);
	SAFE_RELEASE(g_p_di);
}

INT_PTR CALLBACK main_dlg_proc(const HWND h_dlg, const UINT msg, const WPARAM w_param, LPARAM l_param) {
	UNREFERENCED_PARAMETER(l_param);
	
	switch (msg) {
	case WM_INITDIALOG: {
		//if (FAILED(init_direct_input(h_dlg))) {
		//	MessageBox(nullptr, TEXT("Error Initializing DirectInput"),
		//		TEXT("DirectInput Sample"), MB_ICONERROR | MB_OK);
		//	EndDialog(h_dlg, 0);
		//}

		// Get the driver attributes (Vendor ID, Product ID, Version Number)
		SetTimer(h_dlg, 0, 1000 / 120, nullptr);
		return TRUE;
	}
	case WM_TIMER:
		// Update the input device every timer message
		if (FAILED(update_input_state(h_dlg))) {
			KillTimer(h_dlg, 0);
			MessageBox(nullptr, TEXT("Error Reading Input State. ") \
				TEXT("The sample will now exit."), TEXT("DirectInput Sample"),
				MB_ICONERROR | MB_OK);
			EndDialog(h_dlg, TRUE);
		}
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(w_param)) {
		case IDCANCEL:
			EndDialog(h_dlg, 0);
			return TRUE;
		default: ;
		}
	case WM_DESTROY:
		// Cleanup everything
		KillTimer(h_dlg, 0);
		free_direct_input();
	default:
		return FALSE;
	}
}

int APIENTRY WinMain(_In_ const HINSTANCE h_inst, _In_opt_ HINSTANCE,
					 _In_ LPSTR, _In_ int) {
	using std::cout;
	using std::this_thread::yield;
	
	InitCommonControls();
	
	SetConsoleCtrlHandler(ctrl_handler, TRUE);
	
	atexit([] {
		// trigger deconstructors for all controllers
		std::lock_guard<std::mutex> lk(controller_map_mutex);	
	});

	try {
		XOutput::XOutputInitialize();
	} catch (XOutput::XOutputError &e) {
		cout << e.what() << '\n';
		return -1;
	}
	
	DWORD unused;
	if (!success(XOutput::XOutputGetRealUserIndex(0, &unused))) {
		cout << "Unable to connect to ScpVBus.\n";
		return -1;
	}
	
	HidD_GetHidGuid(&hid_guid);
	get_initial_plugged_devices();
	
	{
		using std::uint8_t;

		const bytes buf = {0x01, static_cast<uint8_t>(controller.counter++ & 0x0F),
					 0x00, 0x01, 0x40, 0x40, 0x00,
					 0x01, 0x40, 0x40, 0x30, 0x01};
		
		write_data(buf);
		controller.led = 1;
		controller.led_changed = false;
	}
	
	if (__argc > 1)
		controller.max = atoi(__argv[1]);
	else
		controller.max = 255;
	
	DialogBox(h_inst, MAKEINTRESOURCE(IDD_JOYST_IMM), nullptr, main_dlg_proc);

	return 0;
}
