#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <strsafe.h>
#include <stdio.h>
#include <locale.h>

#pragma comment(lib, "avrt.lib")

bool GetDeviceName(IMMDeviceCollection *DeviceCollection, UINT DeviceIndex, LPWSTR deviceName, size_t size);
DWORD WINAPI WASAPICaptureThread(LPVOID Context);
LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void ShowError(HRESULT hr);

struct Rect : RECT
{
	LONG width() { return right - left; }
	LONG height() { return bottom - top; }
};

IAudioCaptureClient *CaptureClient = NULL;

HANDLE ShutdownEvent;
HANDLE AudioSamplesReadyEvent;

HWND hDrawWnd;

Rect rcDraw;
HDC hdcDraw;

HPEN hpenGreen;

CRITICAL_SECTION critical_section;

// リングバッファ
int ringbuf_cur = 0;
short *ringbuf;

template <class T> inline void SafeRelease(T *ppT)
{
	if (*ppT)
	{
		(*ppT)->Release();
		*ppT = NULL;
	}
}

int wmain(int argc, wchar_t* argv[])
{
	setlocale(LC_ALL, ".OCP");

	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(hr))
	{
		printf("Unable to initialize COM: %x\n", hr);
		return 1;
	}

	// デバイス列挙
	IMMDeviceEnumerator *deviceEnumerator = NULL;
	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator));
	if (FAILED(hr))
	{
		printf("Unable to instantiate device enumerator: %x\n", hr);
		goto Exit;
	}

	IMMDeviceCollection *deviceCollection = NULL;
	hr = deviceEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &deviceCollection);
	if (FAILED(hr))
	{
		printf("Unable to retrieve device collection: %x\n", hr);
		goto Exit;
	}

	printf("Select an output device:\n");
	printf("    0:  Default Console Device\n");
	printf("    1:  Default Communications Device\n");
	printf("    2:  Default Multimedia Device\n");

	UINT deviceCount;
	hr = deviceCollection->GetCount(&deviceCount);
	if (FAILED(hr))
	{
		printf("Unable to get device collection length: %x\n", hr);
		goto Exit;
	}

	for (UINT i = 0 ; i < deviceCount ; i += 1)
	{
		wchar_t deviceName[128];

		if (GetDeviceName(deviceCollection, i, deviceName, sizeof(deviceName)))
		{
			printf("    %d:  %S\n", i + 3, deviceName);
		}
	}

	// デバイス選択
	wchar_t choice[10];
	_getws_s(choice);   // Note: Using the safe CRT version of _getws.

	long deviceIndex;
	wchar_t *endPointer;

	deviceIndex = wcstoul(choice, &endPointer, 0);
	if (deviceIndex == 0 && endPointer == choice)
	{
		printf("unrecognized device index: %S\n", choice);
		goto Exit;
	}

	IMMDevice *device = NULL;

	bool UseConsoleDevice;
	bool UseCommunicationsDevice;
	bool UseMultimediaDevice;

	switch (deviceIndex)
	{
	case 0:
		UseConsoleDevice = 1;
		break;
	case 1:
		UseCommunicationsDevice = 1;
		break;
	case 2:
		UseMultimediaDevice = 1;
		break;
	default:
		hr = deviceCollection->Item(deviceIndex - 3, &device);
		if (FAILED(hr))
		{
			printf("Unable to retrieve device %d: %x\n", deviceIndex - 3, hr);
			goto Exit;
		}
		break;
	}

	if (device == NULL)
	{
		ERole deviceRole = eConsole;    // Assume we're using the console role.
		if (UseConsoleDevice)
		{
			deviceRole = eConsole;
		}
		else if (UseCommunicationsDevice)
		{
			deviceRole = eCommunications;
		}
		else if (UseMultimediaDevice)
		{
			deviceRole = eMultimedia;
		}
		hr = deviceEnumerator->GetDefaultAudioEndpoint(eCapture, deviceRole, &device);
		if (FAILED(hr))
		{
			printf("Unable to get default device for role %d: %x\n", deviceRole, hr);
			goto Exit;
		}
	}

	// イベント作成
	//
	//  Create our shutdown and samples ready events- we want auto reset events that start in the not-signaled state.
	//
	ShutdownEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
	if (ShutdownEvent == NULL)
	{
		printf("Unable to create shutdown event: %d.\n", GetLastError());
		goto Exit;
	}

	AudioSamplesReadyEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
	if (AudioSamplesReadyEvent == NULL)
	{
		printf("Unable to create samples ready event: %d.\n", GetLastError());
		goto Exit;
	}

	// AudioClient準備
	IAudioClient *AudioClient = NULL;

	//
	//  Now activate an IAudioClient object on our preferred endpoint and retrieve the mix format for that endpoint.
	//
	hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, NULL, reinterpret_cast<void **>(&AudioClient));
	if (FAILED(hr))
	{
		printf("Unable to activate audio client: %x.\n", hr);
		goto Exit;
	}

	// デバイスのレイテンシ取得
	REFERENCE_TIME DefaultDevicePeriod;
	REFERENCE_TIME MinimumDevicePeriod;
	hr = AudioClient->GetDevicePeriod(&DefaultDevicePeriod, &MinimumDevicePeriod);
	if (FAILED(hr))
	{
		printf("Unable to get device period: %x.\n", hr);
		goto Exit;
	}
	printf("DefaultDevicePeriod = %I64d\n", DefaultDevicePeriod);
	printf("MinimumDevicePeriod = %I64d\n", MinimumDevicePeriod);

	//
	// Load the MixFormat.  This may differ depending on the shared mode used
	//
	/*WAVEFORMATEX *MixFormat;
	hr = AudioClient->GetMixFormat(&MixFormat);
	if (FAILED(hr))
	{
		printf("Unable to get mix format on audio client: %x.\n", hr);
		goto Exit;
	}
	size_t FrameSize = (MixFormat->wBitsPerSample / 8) * MixFormat->nChannels;
	printf("FrameSize = %d\n", FrameSize);*/
	
	//WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)MixFormat;

	WAVEFORMATEXTENSIBLE WaveFormat;
	WaveFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	WaveFormat.Format.nChannels = 2;
	WaveFormat.Format.nSamplesPerSec = 44100;
	WaveFormat.Format.wBitsPerSample = 16;
	WaveFormat.Format.nBlockAlign = WaveFormat.Format.wBitsPerSample / 8 * WaveFormat.Format.nChannels;
	WaveFormat.Format.nAvgBytesPerSec = WaveFormat.Format.nSamplesPerSec * WaveFormat.Format.nBlockAlign;
	WaveFormat.Format.cbSize = 22;
	WaveFormat.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	WaveFormat.Samples.wValidBitsPerSample = WaveFormat.Format.wBitsPerSample;
	WaveFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

	size_t FrameSize = WaveFormat.Format.nBlockAlign;
	printf("FrameSize = %d\n", FrameSize);

	//
	//  Initialize WASAPI in event driven mode, associate the audio client with our samples ready event handle, retrieve 
	//  a capture client for the transport, create the capture thread and start the audio engine.
	//
	//hr = AudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, DefaultDevicePeriod, 0, MixFormat, NULL);
	hr = AudioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, DefaultDevicePeriod, DefaultDevicePeriod, (WAVEFORMATEX*)&WaveFormat, NULL);
	if (FAILED(hr))
	{
		printf("Unable to initialize audio client: %x.\n", hr);
		ShowError(hr);
		goto Exit;
	}

	//
	//  Retrieve the buffer size for the audio client.
	//
	UINT32 BufferSize;
	hr = AudioClient->GetBufferSize(&BufferSize);
	if(FAILED(hr))
	{
		printf("Unable to get audio client buffer: %x. \n", hr);
		goto Exit;
	}

	hr = AudioClient->SetEventHandle(AudioSamplesReadyEvent);
	if (FAILED(hr))
	{
		printf("Unable to set ready event: %x.\n", hr);
		goto Exit;
	}

	hr = AudioClient->GetService(IID_PPV_ARGS(&CaptureClient));
	if (FAILED(hr))
	{
		printf("Unable to get new capture client: %x.\n", hr);
		goto Exit;
	}

	// ウィンドウ作成
	const wchar_t szClassName[] = L"MainWnd";

	WNDCLASSEX wcx;
	wcx.cbSize = sizeof(WNDCLASSEX);
	wcx.style = 0;
	wcx.lpfnWndProc = WindowProc;
	wcx.cbClsExtra = 0;
	wcx.cbWndExtra = 0;
	wcx.hInstance = GetModuleHandle(NULL);
	wcx.hIcon = NULL;
	wcx.hCursor = LoadCursor(NULL, IDC_ARROW);
	wcx.hbrBackground = NULL;
	wcx.lpszMenuName = NULL;
	wcx.lpszClassName = szClassName;
	wcx.hIconSm = NULL;

	if (!RegisterClassEx(&wcx))
	{
		MessageBox(NULL, L"RegisterClassEx failed.", L"error", NULL);
		goto Exit;
	}

	hDrawWnd = CreateWindow(
		szClassName, L"wasapirec1",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		640, 480,
		NULL, NULL, GetModuleHandle(NULL), NULL);
	if (!hDrawWnd)
	{
		MessageBox(NULL, L"CreateWindow failed.", L"error", NULL);
		goto Exit;
	}

	// バックバッファーの作成
	// 描画領域
	GetClientRect(hDrawWnd, &rcDraw);
	HBITMAP hBmp = CreateCompatibleBitmap(GetDC(hDrawWnd), rcDraw.width(), rcDraw.height());
	hdcDraw = CreateCompatibleDC(GetDC(hDrawWnd));
	SelectObject(hdcDraw, hBmp);

	hpenGreen = CreatePen(PS_SOLID, 1, RGB(0, 255, 0));

	ShowWindow(hDrawWnd, SW_SHOW);
	UpdateWindow(hDrawWnd);

	// クリティカルセクション初期化
	InitializeCriticalSection(&critical_section);

	// 描画用タイマー
	SetTimer(hDrawWnd, 1, 1000/60, NULL);

	// リングバッファ作成
	ringbuf = new short[rcDraw.width()];

	//
	//  Start capturing...
	//

	//
	//  Now create the thread which is going to drive the capture.
	//
	HANDLE CaptureThread = CreateThread(NULL, 0, WASAPICaptureThread, NULL, 0, NULL);
	if (CaptureThread == NULL)
	{
		printf("Unable to create transport thread: %x.", GetLastError());
		goto Exit;
	}

	//
	//  We're ready to go, start capturing!
	//
	hr = AudioClient->Start();
	if (FAILED(hr))
	{
		printf("Unable to start capture client: %x.\n", hr);
		goto Exit;
	}

	//メッセージループ
	MSG msg;
	while(GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	//
	//  Stop the capturer.
	//
	//
	//  Tell the capture thread to shut down, wait for the thread to complete then clean up all the stuff we 
	//  allocated in Start().
	//
	SetEvent(ShutdownEvent);

	hr = AudioClient->Stop();
	if (FAILED(hr))
	{
		printf("Unable to stop audio client: %x\n", hr);
	}

	WaitForSingleObject(CaptureThread, INFINITE);
	CloseHandle(CaptureThread);
	CloseHandle(ShutdownEvent);
	CloseHandle(AudioSamplesReadyEvent);
	DeleteCriticalSection(&critical_section);

	//
	//  Shut down the capture code and free all the resources.
	//

Exit:
	SafeRelease(&CaptureClient);
	SafeRelease(&AudioClient);
	/*if (MixFormat)
	{
	CoTaskMemFree(MixFormat);
	}*/
	SafeRelease(&device);
	SafeRelease(&deviceCollection);
	SafeRelease(&deviceEnumerator);
	CoUninitialize();

	return 0;
}

//
//  Retrieves the device friendly name for a particular device in a device collection.  
//
//  The returned string was allocated using malloc() so it should be freed using free();
//
bool GetDeviceName(IMMDeviceCollection *DeviceCollection, UINT DeviceIndex, LPWSTR deviceName, size_t size)
{
	IMMDevice *device;
	HRESULT hr;

	hr = DeviceCollection->Item(DeviceIndex, &device);
	if (FAILED(hr))
	{
		printf("Unable to get device %d: %x\n", DeviceIndex, hr);
		return false;
	}

	IPropertyStore *propertyStore;
	hr = device->OpenPropertyStore(STGM_READ, &propertyStore);
	SafeRelease(&device);
	if (FAILED(hr))
	{
		printf("Unable to open device %d property store: %x\n", DeviceIndex, hr);
		return false;
	}

	PROPVARIANT friendlyName;
	PropVariantInit(&friendlyName);
	hr = propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName);
	SafeRelease(&propertyStore);

	if (FAILED(hr))
	{
		printf("Unable to retrieve friendly name for device %d : %x\n", DeviceIndex, hr);
		return false;
	}

	hr = StringCbPrintf(deviceName, size, L"%s", friendlyName.vt != VT_LPWSTR ? L"Unknown" : friendlyName.pwszVal);
	if (FAILED(hr))
	{
		printf("Unable to format friendly name for device %d : %x\n", DeviceIndex, hr);
		return false;
	}

	PropVariantClear(&friendlyName);

	return true;
}

//
//  Capture thread - processes samples from the audio engine
//
DWORD WINAPI WASAPICaptureThread(LPVOID Context)
{
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(hr))
	{
		printf("Unable to initialize COM in render thread: %x\n", hr);
		return hr;
	}

	DWORD mmcssTaskIndex = 0;
	HANDLE mmcssHandle = AvSetMmThreadCharacteristics(L"Audio", &mmcssTaskIndex);
	if (mmcssHandle == NULL)
	{
	printf("Unable to enable MMCSS on capture thread: %d\n", GetLastError());
	}

	HANDLE waitArray[2] = {ShutdownEvent, AudioSamplesReadyEvent};

	bool stillPlaying = true;
	while (stillPlaying)
	{
		DWORD waitResult = WaitForMultipleObjects(2, waitArray, FALSE, INFINITE);
		switch (waitResult)
		{
		case WAIT_OBJECT_0 + 0:     // ShutdownEvent
			stillPlaying = false;       // We're done, exit the loop.
			break;
		case WAIT_OBJECT_0 + 1:     // AudioSamplesReadyEvent
			//
			//  We need to retrieve the next buffer of samples from the audio capturer.
			//
			short *pData;
			UINT32 framesAvailable;
			DWORD  flags;

			//
			//  Find out how much capture data is available.  We need to make sure we don't run over the length
			//  of our capture buffer.  We'll discard any samples that don't fit in the buffer.
			//
			hr = CaptureClient->GetBuffer((BYTE**)&pData, &framesAvailable, &flags, NULL, NULL);
			if (SUCCEEDED(hr))
			{
				EnterCriticalSection(&critical_section);
				if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
				{
					for (UINT32 i = 0; i < framesAvailable; i++)
					{
						ringbuf[ringbuf_cur] = 0;
						ringbuf_cur++;
						ringbuf_cur %= rcDraw.width();
					}
				}
				else
				{
					for (UINT32 i = 0; i < framesAvailable; i++)
					{
						ringbuf[ringbuf_cur] = pData[i*2];
						ringbuf_cur ++;
						ringbuf_cur %= rcDraw.width();
					}
				}
				LeaveCriticalSection(&critical_section);

				hr = CaptureClient->ReleaseBuffer(framesAvailable);
				if (FAILED(hr))
				{
					printf("Unable to release capture buffer: %x!\n", hr);
				}
			}
			break;
		}
	}

	return 0;
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_TIMER:
		{
			// 波形描画
			EnterCriticalSection(&critical_section);
			PatBlt(hdcDraw, 0, 0, rcDraw.width(), rcDraw.height(), BLACKNESS);
			SelectObject(hdcDraw, hpenGreen);
			const int y0 = rcDraw.height() / 2;
			MoveToEx(hdcDraw, 0, y0, NULL);
			for (int x = 0; x < rcDraw.width(); x++)
			{
				int pos = (ringbuf_cur + x) % rcDraw.width();
				short v = ringbuf[pos];
				int y = y0 - rcDraw.height() * v / 2 / 32768;
				LineTo(hdcDraw, x, y);
			}
			LeaveCriticalSection(&critical_section);
			InvalidateRect(hDrawWnd, NULL, FALSE);
			return 0;
		}
	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hWnd, &ps);
			BitBlt(hdc, 0, 0, rcDraw.width(), rcDraw.height(), hdcDraw, 0, 0, SRCCOPY);
			EndPaint(hWnd, &ps);
			return 0;
		}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void ShowError(HRESULT hr)
{
	switch(hr)
	{
	case AUDCLNT_E_ALREADY_INITIALIZED:printf("AUDCLNT_E_ALREADY_INITIALIZED\n");break;
	case AUDCLNT_E_WRONG_ENDPOINT_TYPE:printf("AUDCLNT_E_WRONG_ENDPOINT_TYPE\n");break;
	case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:printf("AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED\n");break;
	case AUDCLNT_E_BUFFER_SIZE_ERROR:printf("AUDCLNT_E_BUFFER_SIZE_ERROR\n");break;
	case AUDCLNT_E_CPUUSAGE_EXCEEDED:printf("AUDCLNT_E_CPUUSAGE_EXCEEDED\n");break;
	case AUDCLNT_E_DEVICE_INVALIDATED:printf("AUDCLNT_E_DEVICE_INVALIDATED\n");break;
	case AUDCLNT_E_DEVICE_IN_USE:printf("AUDCLNT_E_DEVICE_IN_USE\n");break;
	case AUDCLNT_E_ENDPOINT_CREATE_FAILED:printf("AUDCLNT_E_ENDPOINT_CREATE_FAILED\n");break;
	case AUDCLNT_E_INVALID_DEVICE_PERIOD:printf("AUDCLNT_E_INVALID_DEVICE_PERIOD\n");break;
	case AUDCLNT_E_UNSUPPORTED_FORMAT:printf("AUDCLNT_E_UNSUPPORTED_FORMAT\n");break;
	case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:printf("AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED\n");break;
	case AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL:printf("AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL\n");break;
	case AUDCLNT_E_SERVICE_NOT_RUNNING:printf("AUDCLNT_E_SERVICE_NOT_RUNNING\n");break;
	case E_POINTER:printf("E_POINTER\n");break;
	case E_INVALIDARG:printf("E_INVALIDARG\n");break;
	case E_OUTOFMEMORY:printf("E_OUTOFMEMORY\n");break;
	}
}