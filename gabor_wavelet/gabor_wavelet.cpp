#include <windows.h>
#define _USE_MATH_DEFINES
#include <math.h>

#ifdef _DEBUG
#include <strsafe.h>
#define TRACE(X, ...) DEBUG_PRINTF(X, __VA_ARGS__)
void DEBUG_PRINTF(const wchar_t *format, ...)
{
	wchar_t buf[1024];
	va_list arg_list;
	va_start(arg_list, format);
	StringCbVPrintf(buf, sizeof(buf), format, arg_list);
	va_end(arg_list);
	OutputDebugString(buf);
}
#else
#define TRACE(X, ...)
#endif // _DEBUG

#pragma pack(push, 1)
typedef struct {
	char          id[4];
	unsigned long size;
	char          fmt[4];
} RiffHeader;

typedef struct {
	char           chunkID[4];
	long           chunkSize; 
	short          wFormatTag;
	unsigned short wChannels;
	unsigned long  dwSamplesPerSec;
	unsigned long  dwAvgBytesPerSec;
	unsigned short wBlockAlign;
	unsigned short wBitsPerSample;
	/* Note: there may be additional fields here, depending upon wFormatTag. */ 
} FormatChunk;

typedef struct {
	char           chunkID[4];
	long           chunkSize; 
} DataChunk;
#pragma pack(pop)

LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

const double sigma = 1.0;
const int octave_div = 12; // 音程単位
const int cent_div = 100; // セント単位

const double freq0 = 110; // 下限周波数
const int octave = 5; // 解析範囲のオクターブ
const int cent_interval = 25; // 解析周波数間隔(セント単位)

const int cent_max = cent_div*octave_div*octave; // 上限(セント単位)

RECT rcWnd;
HDC hdcBack; // 描画バックバッファ

COLORREF hsv2rgb(double h, double s, double v)
{
	if (h < 0)
	{
		h += 360;
	}
	else if (h >= 360)
	{
		h -= 360;
	}

	int H_i = (int)floor(h / 60);
	double fl = (h / 60) - H_i;
	if( !(H_i & 1)) fl = 1 - fl; // if i is even
	double m = v * ( 1 - s );
	double n = v * ( 1 - s * fl );

	double r, g, b;
	switch (H_i)
	{
	   case 0: r = v; g = n; b = m; break;
	   case 1: r = n; g = v; b = m; break;
	   case 2: r = m; g = v; b = n; break;
	   case 3: r = m; g = n; b = v; break;
	   case 4: r = n; g = m; b = v; break;
	   case 5: r = v; g = m; b = n; break;
	}

	return RGB(int(r*256), int(g*256), int(b*256));
}

float* read_wav(const wchar_t* file_path, int* fs, int* len)
{
	HANDLE hFile = CreateFile(file_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

	RiffHeader header;
	DWORD readsize;
	ReadFile(hFile, &header, sizeof(header), &readsize, NULL);

	FormatChunk fmt;
	ReadFile(hFile, &fmt, sizeof(fmt), &readsize, NULL);
	TRACE(L"%d channel\n", fmt.wChannels);
	TRACE(L"%d blocksize\n", fmt.wBlockAlign);
	TRACE(L"%d bit per channel\n", fmt.wBitsPerSample);

	*fs = fmt.dwSamplesPerSec; 

	DataChunk data;
	ReadFile(hFile, &data, sizeof(data), &readsize, NULL);

	*len = data.chunkSize/sizeof(float);
	float *wav = new float[*len];
	ReadFile(hFile, wav, data.chunkSize, &readsize, NULL);

	return wav;
}

void gabor_transform(float* wav, int b, int size, int Fs, double* wt)
{

	for (int y = 0; y < cent_max/cent_interval; y++)
	{
		double freq = freq0 * pow(2.0, double(y)*cent_interval/(cent_div*octave_div)); // 周波数
		double a = 1.0/freq;
		double dt = a*sigma*sqrt(-2.0*log(0.01)); // 窓幅(秒)
		int    dx = int(dt * Fs);                 // 窓幅(サンプル数)

		// 窓幅の範囲を積分
		double real_wt = 0;
		double imag_wt = 0;
		for (int m = -dx; m <= dx; m++)
		{
			int n = b + m; // 信号のサンプル位置
			if (n >= 0 && n < size)
			{
				double t = double(m)/Fs/a;
				double gauss = 1.0/sqrt(2.0*M_PI*sigma*sigma) * exp(-t*t/(2.0*sigma*sigma)); // ガウス関数
				double omega_t = 2.0*M_PI*t; // ωt
				real_wt += double(wav[n]) * gauss * cos(omega_t);
				imag_wt += double(wav[n]) * gauss * sin(omega_t);
			}
		}
		wt[y] = 1.0/sqrt(a) * sqrt(real_wt*real_wt + imag_wt*imag_wt);
	}
}

void analyze()
{
	// waveファイル読み込み
	int Fs; // サンプリング周波数
	int wav_length;
	float *wav = read_wav(L"a.wav", &Fs, &wav_length);
	TRACE(L"%d sample rate\n", Fs);

	int center = wav_length/2; // 中心のサンプル番号

	double cuttime = 0.046440; // 切り出す長さ[s](2048サンプル)
	int wavdata_length = int(cuttime/2*Fs)*2;
	float *p_wav = wav + center - wavdata_length/2 - 1;

	TRACE(L"analyze freq %f - %f\n", freq0, freq0*pow(2.0, octave));

	// ガボールウェーブレット変換
	double wt[cent_max/cent_interval];
	for (int x = 0; x < wavdata_length; x+=64)
	{
		gabor_transform(p_wav, x, wavdata_length, Fs, wt);

		// スペクトログラム描画
		double max_y_v = 0;
		int max_y = 0;
		for (int y = 0; y < cent_max/cent_interval; y++)
		{
			SetDCPenColor(hdcBack, hsv2rgb(270 - wt[y]*3.5, 0.99, 0.99));

			int y0 = cent_max/cent_interval;
			MoveToEx(hdcBack, x/8, y0 - y, NULL);
			LineTo(hdcBack, (x+64)/8, y0 - y);

			if (wt[y] > max_y_v)
			{
				max_y_v = wt[y];
				max_y = y;
			}
		}
		double freq = freq0 * pow(2.0, double(max_y)*cent_interval/(cent_div*octave_div)); // 周波数
		TRACE(L"%f\n", freq);
	}
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
	WNDCLASS wc;
	wc.style         = 0;
	wc.lpfnWndProc   = WindowProc;
	wc.cbClsExtra    = 0;
	wc.cbWndExtra    = 0;
	wc.hInstance     = hInstance;
	wc.hIcon         = NULL;
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = NULL;
	wc.lpszMenuName  = NULL;
	wc.lpszClassName = L"gabor_wavelet";

	RegisterClass(&wc);

	HWND hWnd = CreateWindow(wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 265, 270, NULL, NULL, hInstance, NULL);

	GetClientRect(hWnd, &rcWnd);

	// 描画バックバッファ作成
	HDC hDC = GetDC(hWnd);
	HBITMAP hBmp = CreateCompatibleBitmap(hDC, rcWnd.right, rcWnd.bottom);
	hdcBack = CreateCompatibleDC(hDC);
	SelectObject(hdcBack, hBmp);
	ReleaseDC(hWnd, hDC);
	SelectObject(hdcBack, GetStockObject(DC_PEN));
	PatBlt(hdcBack, 0, 0, rcWnd.right, rcWnd.bottom, WHITENESS);

	// ウェーブレット解析
	analyze();

	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return msg.wParam;
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hWnd, &ps);
			BitBlt(hdc, 0, 0, rcWnd.right, rcWnd.bottom, hdcBack, 0, 0, SRCCOPY);
			EndPaint(hWnd, &ps);
			return 0;
		}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}
