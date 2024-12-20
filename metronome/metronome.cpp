#include <windows.h>
#include <strsafe.h>
#include <commctrl.h>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#define _USE_MATH_DEFINES
#include <math.h>

#include "resource.h"

#pragma comment(lib, "avrt.lib")

#define SAFE_RELEASE(p)	{if (p) {(p)->Release(); (p)=NULL;}}

#define WIDEN2(x) L ## x
#define WIDEN(x) WIDEN2(x)
#define __WFILE__ WIDEN(__FILE__)
#define ShowError(hr) ShowError_Imp(hr, __WFILE__, __LINE__)

void ShowError_Imp(HRESULT hr, wchar_t *file, int line);
INT CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
void Start();
void Stop();
void PrepareWave();
bool Play();
DWORD WINAPI WASAPIRenderThread(LPVOID Context);

IAudioClient *AudioClient;
IAudioRenderClient *RenderClient;
UINT32 BufferSize;

HANDLE ShutdownEvent;
HANDLE AudioSamplesReadyEvent;
HANDLE RenderThread;

const int SAMPLE_RATE = 44100;

int tempo = 120;
int frequency = 1000;

bool bPlaying = false;

short vol = 20000;

bool bSaw = true;
bool bSquare = false;
bool bSin = false;

const int SUSTAIN_TIME_MS = 50;
const size_t WAVE_SIZE = SAMPLE_RATE*SUSTAIN_TIME_MS/1000;
short wave[WAVE_SIZE];

size_t playing_pos;
size_t elapse_frame;

int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE, LPTSTR, int nCmdShow)
{
	HRESULT hr;
	int ret = 0;

	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(hr))
	{
		ShowError(hr);
		goto Exit;
	}

	IMMDeviceEnumerator *deviceEnumerator = NULL;
	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator));
	if (FAILED(hr))
	{
		ShowError(hr);
		goto Exit;
	}

	IMMDeviceCollection *deviceCollection = NULL;
	hr = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
	if (FAILED(hr))
	{
		ShowError(hr);
		goto Exit;
	}

	IMMDevice *device = NULL;
	hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
	if (FAILED(hr))
	{
		ShowError(hr);
		goto Exit;
	}

	ShutdownEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
	if (ShutdownEvent == NULL)
	{
		ShowError(hr);
		goto Exit;
	}

	AudioSamplesReadyEvent = CreateEventEx(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
	if (AudioSamplesReadyEvent == NULL)
	{
		ShowError(hr);
		return false;
	}

	hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, NULL, reinterpret_cast<void **>(&AudioClient));
	if (FAILED(hr))
	{
		ShowError(hr);
		return false;
	}

	REFERENCE_TIME DefaultDevicePeriod;
	REFERENCE_TIME MinimumDevicePeriod;
	hr = AudioClient->GetDevicePeriod(&DefaultDevicePeriod, &MinimumDevicePeriod);
	if (FAILED(hr))
	{
		ShowError(hr);
		goto Exit;
	}

	WAVEFORMATEXTENSIBLE WaveFormat;
	WaveFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	WaveFormat.Format.nChannels = 2;
	WaveFormat.Format.nSamplesPerSec = SAMPLE_RATE;
	WaveFormat.Format.wBitsPerSample = 16;
	WaveFormat.Format.nBlockAlign = WaveFormat.Format.wBitsPerSample / 8 * WaveFormat.Format.nChannels;
	WaveFormat.Format.nAvgBytesPerSec = WaveFormat.Format.nSamplesPerSec * WaveFormat.Format.nBlockAlign;
	WaveFormat.Format.cbSize = 22;
	WaveFormat.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	WaveFormat.Samples.wValidBitsPerSample = WaveFormat.Format.wBitsPerSample;
	WaveFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

	hr = AudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
		DefaultDevicePeriod,
		0,
		(WAVEFORMATEX*)&WaveFormat,
		NULL);
	if (FAILED(hr))
	{
		ShowError(hr);
		goto Exit;
	}

	hr = AudioClient->GetBufferSize(&BufferSize);
	if(FAILED(hr))
	{
		ShowError(hr);
		goto Exit;
	}

	hr = AudioClient->SetEventHandle(AudioSamplesReadyEvent);
	if (FAILED(hr))
	{
		ShowError(hr);
		goto Exit;
	}

	hr = AudioClient->GetService(IID_PPV_ARGS(&RenderClient));
	if (FAILED(hr))
	{
		ShowError(hr);
		goto Exit;
	}

	// ”gŒ`€”õ
	PrepareWave();

	HWND hDlg = CreateDialog(hInstance, MAKEINTRESOURCE(IDD_DIALOG1), NULL, DlgProc);
	if (!hDlg)
	{
		ShowError(GetLastError());
		goto Exit;
	}

	UpdateWindow(hDlg);

	MSG msg;
	while(GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	AudioClient->Stop();

	SetEvent(ShutdownEvent);
	WaitForSingleObject(RenderThread, INFINITE);
	CloseHandle(RenderThread);
	CloseHandle(ShutdownEvent);
	CloseHandle(AudioSamplesReadyEvent);

	ret = msg.wParam;

Exit:
	SAFE_RELEASE(RenderClient);
	SAFE_RELEASE(AudioClient);
	SAFE_RELEASE(device);
	SAFE_RELEASE(deviceCollection);
	SAFE_RELEASE(deviceEnumerator);

	return ret;
}

void ShowError_Imp(HRESULT hr, wchar_t *file, int line)
{
	wchar_t fname[_MAX_PATH];
	wchar_t ext[_MAX_PATH];
	wchar_t buf[_MAX_PATH];
	wchar_t code[64];

	code[0] = NULL;

	switch(hr)
	{
	case AUDCLNT_E_BUFFER_ERROR:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_BUFFER_ERROR ");break;
	case AUDCLNT_E_BUFFER_TOO_LARGE:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_BUFFER_TOO_LARGE ");break;
	case AUDCLNT_E_BUFFER_SIZE_ERROR:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_BUFFER_SIZE_ERROR ");break;
	case AUDCLNT_E_OUT_OF_ORDER:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_OUT_OF_ORDER ");break;
	case AUDCLNT_E_DEVICE_INVALIDATED:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_DEVICE_INVALIDATED ");break;
	case AUDCLNT_E_BUFFER_OPERATION_PENDING:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_BUFFER_OPERATION_PENDING ");break;
	case AUDCLNT_E_SERVICE_NOT_RUNNING:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_SERVICE_NOT_RUNNING ");break;
	case AUDCLNT_E_ALREADY_INITIALIZED:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_ALREADY_INITIALIZED ");break;
	case AUDCLNT_E_WRONG_ENDPOINT_TYPE:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_WRONG_ENDPOINT_TYPE ");break;
	case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED ");break;
	case AUDCLNT_E_CPUUSAGE_EXCEEDED:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_CPUUSAGE_EXCEEDED ");break;
	case AUDCLNT_E_DEVICE_IN_USE:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_DEVICE_IN_USE ");break;
	case AUDCLNT_E_ENDPOINT_CREATE_FAILED:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_ENDPOINT_CREATE_FAILED ");break;
	case AUDCLNT_E_INVALID_DEVICE_PERIOD:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_INVALID_DEVICE_PERIOD ");break;
	case AUDCLNT_E_UNSUPPORTED_FORMAT:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_UNSUPPORTED_FORMAT ");break;
	case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED ");break;
	case AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL:StringCbCopy(code, sizeof(code), L"AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL ");break;
	case E_POINTER:StringCbCopy(code, sizeof(code), L"E_POINTER ");break;
	case E_INVALIDARG:StringCbCopy(code, sizeof(code), L"E_INVALIDARG ");break;
	case E_OUTOFMEMORY:StringCbCopy(code, sizeof(code), L"E_OUTOFMEMORY ");break;
	}

	_wsplitpath_s(file, NULL, 0, NULL, 0, fname, _MAX_PATH, ext, _MAX_PATH);
	StringCbPrintf(buf, _MAX_PATH, L"code : %s%x (%s%s:%d)", code, hr, fname, ext, line);
	MessageBox(NULL, buf, L"error", MB_OK);
}

INT CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_INITDIALOG:
		{
			wchar_t str[16];
			StringCbPrintf(str, sizeof(str), L"%d", tempo);
			SetWindowText(GetDlgItem(hDlg, IDC_ED_TEMPO), str);

			SendDlgItemMessage(hDlg, IDC_CHECKBOX1, BM_SETCHECK, BST_CHECKED, 0);

			StringCbPrintf(str, sizeof(str), L"%d", frequency);
			SetWindowText(GetDlgItem(hDlg, IDC_ED_FREQ), str);

			SendDlgItemMessage(hDlg, IDC_SLIDER_VOL, TBM_SETRANGEMIN, FALSE, 0);
			SendDlgItemMessage(hDlg, IDC_SLIDER_VOL, TBM_SETRANGEMAX, FALSE, 32767);
			SendDlgItemMessage(hDlg, IDC_SLIDER_VOL, TBM_SETPOS, TRUE, vol);

			return TRUE;
		}
	case WM_COMMAND:
		switch (HIWORD(wParam))
		{
		case BN_CLICKED:
			switch (LOWORD(wParam))
			{
			case IDC_BTN_STARTSTOP:
				if (bPlaying)
				{
					bPlaying = false;
					SetWindowText(GetDlgItem(hDlg, IDC_BTN_STARTSTOP), L"Start");
					Stop();
				}
				else
				{
					bPlaying = true;
					SetWindowText(GetDlgItem(hDlg, IDC_BTN_STARTSTOP), L"Stop");
					Start();
				}
				break;
			case IDC_CHECKBOX1:
			case IDC_CHECKBOX2:
			case IDC_CHECKBOX3:
				bSaw = (SendDlgItemMessage(hDlg, IDC_CHECKBOX1, BM_GETCHECK, 0, 0) == BST_CHECKED);
				bSquare = (SendDlgItemMessage(hDlg, IDC_CHECKBOX2, BM_GETCHECK, 0, 0) == BST_CHECKED);
				bSin = (SendDlgItemMessage(hDlg, IDC_CHECKBOX3, BM_GETCHECK, 0, 0) == BST_CHECKED);
				PrepareWave();
				break;
			}
			return TRUE;
		case EN_CHANGE:
			{
				wchar_t str[8];

				switch (LOWORD(wParam))
				{
				case IDC_ED_TEMPO:
					{
						GetWindowText(GetDlgItem(hDlg, IDC_ED_TEMPO), str, sizeof(str) / sizeof(wchar_t));
						int tempo_tmp = _wtoi(str);
						if (tempo_tmp <= 0)
						{
							MessageBox(hDlg, L"TEMPO > 0", L"Caution", MB_OK);
						}
						else
						{
							tempo = tempo_tmp;
						}
					}
					break;
				case IDC_ED_FREQ:
					{
						GetWindowText(GetDlgItem(hDlg, IDC_ED_FREQ), str, sizeof(str) / sizeof(wchar_t));
						int frequency_tmp = _wtoi(str);
						if (frequency_tmp <= 0)
						{
							MessageBox(hDlg, L"frequency > 0", L"Caution", MB_OK);
						}
						else
						{
							frequency = frequency_tmp;
							PrepareWave();
						}
					}
					break;
				}
				return TRUE;
			}
		}
		break;
	case WM_HSCROLL:
		if ((HWND)lParam == GetDlgItem(hDlg, IDC_SLIDER_VOL))
		{
			vol = (short)SendDlgItemMessage(hDlg, IDC_SLIDER_VOL, TBM_GETPOS, 0, 0);
			PrepareWave();
			return TRUE;
		}
		break;
	case WM_CLOSE:
		DestroyWindow(hDlg);
		PostQuitMessage(0);
		return TRUE;
	}
	return FALSE;
}

void Start()
{
	playing_pos = 0;
	elapse_frame = 0;

	RenderThread = CreateThread(NULL, 0, WASAPIRenderThread, NULL, 0, NULL);
	if (RenderThread == NULL)
	{
		ShowError(GetLastError());
		return;
	}

	Play();

	HRESULT hr = AudioClient->Start();
	if (FAILED(hr))
	{
		ShowError(hr);
		return;
	}
}

void Stop()
{
	HRESULT hr = AudioClient->Stop();
	if (FAILED(hr))
	{
		ShowError(hr);
	}

	SetEvent(ShutdownEvent);
	WaitForSingleObject(RenderThread, INFINITE);
	CloseHandle(RenderThread);
	RenderThread = NULL;
}

void PrepareWave()
{
	for (size_t i = 0; i < WAVE_SIZE; i++)
	{
		double t = double(i) / SAMPLE_RATE;
		double a = vol;

		int v1 = 0;
		int v2 = 0;
		int v3 = 0;
		int num = 0;

		// ƒmƒRƒMƒŠ”h
		if (bSaw)
		{
			v1 = short(fmod(a * frequency * t, a * 2) - a);
			num++;
		}

		// ‹éŒ`”h
		if (bSquare)
		{
			if (fmod(t, 1.0/frequency) < 0.5/frequency)
			{
				v2 = vol;
			}
			else
			{
				v2 = -vol;
			}
			num++;
		}

		// ³Œ·”h
		if (bSin)
		{
			v3 = short(sin(2.0 * M_PI * frequency * t) * a);
			num++;
		}

		if (num > 0)
		{
			wave[i] = (v1 + v2 + v3) / num;
		}
	}
}

DWORD WINAPI WASAPIRenderThread(LPVOID Context)
{
	bool stillPlaying = true;
	HANDLE waitArray[2] = {ShutdownEvent, AudioSamplesReadyEvent};
	HANDLE mmcssHandle = NULL;
	DWORD mmcssTaskIndex = 0;

	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(hr))
	{
		ShowError(hr);
		return hr;
	}

	mmcssHandle = AvSetMmThreadCharacteristics(L"Audio", &mmcssTaskIndex);
	if (mmcssHandle == NULL)
	{
		ShowError(hr);
	}

	while (stillPlaying)
	{
		DWORD waitResult = WaitForMultipleObjects(2, waitArray, FALSE, INFINITE);
		switch (waitResult)
		{
		case WAIT_OBJECT_0 + 0:     // ShutdownEvent
			stillPlaying = false;
			break;
		case WAIT_OBJECT_0 + 1:     // AudioSamplesReadyEvent
			bool ret = Play();
			if (!ret)
			{
				stillPlaying = false;
			}
			break;
		}
	}

	AvRevertMmThreadCharacteristics(mmcssHandle);
	CoUninitialize();
	return 0;
}

bool Play()
{
	HRESULT hr;
	UINT32 padding;
	short *pData;

	hr = AudioClient->GetCurrentPadding(&padding);
	if (FAILED(hr))
	{
		ShowError(hr);
		return true;
	}

	UINT32 framesAvailable = BufferSize - padding;

	UINT32 period = SAMPLE_RATE * 60 / tempo;
	UINT32 start_pos = 0;

	if (elapse_frame % period < framesAvailable)
	{
		// –Â‚ç‚·ˆÊ’u
		start_pos = elapse_frame % period;
		playing_pos = 0;
	}

	hr = RenderClient->GetBuffer(framesAvailable, (BYTE**)&pData);
	if (FAILED(hr))
	{
		ShowError(hr);
		return false;
	}

	if (playing_pos < WAVE_SIZE)
	{
		for (size_t i = 0; i < start_pos; i++)
		{
			pData[i*2] = 0;
			pData[i*2+1] = 0;
		}

		for (size_t i = start_pos; i < framesAvailable; i++)
		{
			if (playing_pos < WAVE_SIZE)
			{
				pData[i*2] = wave[playing_pos];
				pData[i*2+1] = wave[playing_pos];;
				playing_pos++;
			}
			else
			{
				pData[i*2] = 0;
				pData[i*2+1] = 0;
			}
		}
		hr = RenderClient->ReleaseBuffer(framesAvailable, 0);
		if (FAILED(hr))
		{
			ShowError(hr);
			return false;
		}
	}
	else
	{
		hr = RenderClient->ReleaseBuffer(framesAvailable, AUDCLNT_BUFFERFLAGS_SILENT);
		if (FAILED(hr))
		{
			ShowError(hr);
			return false;
		}
	}

	elapse_frame += framesAvailable;
	return true;
}