#include <windows.h>
#include <strsafe.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include "resource.h"

#ifdef _DEBUG
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

struct Rect : RECT
{
	LONG width() { return right - left; }
	LONG height() { return bottom - top; }
};

extern "C" void rdft(int n, int isgn, double *a, int *ip, double *w);
INT CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
void GetDlgItemRect(HWND hDlg, int nIDDlgItem, RECT& rc);
HDC CreateBackBuffer(HWND hWnd, const int width, const int height);

inline double power(const double re, const double im)
{
	return sqrt(re*re + im*im);
}

inline double han_window(const int i, const int size)
{
	return 0.5 - 0.5 * cos(2.0 * M_PI * i / size);
}

inline int find_max(const double *a, const int size)
{
	int max_i = 0;
	double max_value = a[0];
	for (int i = 1; i < size; ++i)
	{
		if (a[i] > max_value)
		{
			max_i = i;
			max_value = a[i];
		}
	}
	return max_i;
}

const DWORD fftsize = 2048; // フーリエ変換の次数, 周波数ポイントの数
// FFT用ワークテーブル
int ip[46+2];
double w[fftsize*5/4]; // cos/sin work table

// バックバッファー
Rect rcPane1, rcPane2, rcPane3;
HDC hdcPane1, hdcPane2, hdcPane3;


int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPTSTR, int nCmdShow)
{
	HANDLE hFile = CreateFile(L"a.wav", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

	RiffHeader header;
	DWORD readsize;
	ReadFile(hFile, &header, sizeof(header), &readsize, NULL);
	TRACE(L"%d\n", header.size);

	FormatChunk fmt;
	ReadFile(hFile, &fmt, sizeof(fmt), &readsize, NULL);
	TRACE(L"%c%c%c\n", fmt.chunkID[0], fmt.chunkID[1], fmt.chunkID[2]);
	TRACE(L"%d channel\n", fmt.wChannels);
	TRACE(L"%d blocksize\n", fmt.wBlockAlign);
	TRACE(L"%d bit per channel\n", fmt.wBitsPerSample);

	int Fs = fmt.dwSamplesPerSec; 
	TRACE(L"%d sample rate\n", Fs);

	DataChunk data;
	ReadFile(hFile, &data, sizeof(data), &readsize, NULL);
	TRACE(L"%c%c%c%c\n", data.chunkID[0], data.chunkID[1], data.chunkID[2], data.chunkID[3]);
	TRACE(L"%d size\n", data.chunkSize);

	DWORD wav_length = data.chunkSize/sizeof(float);
	float *wav = new float[wav_length];
	ReadFile(hFile, wav, data.chunkSize, &readsize, NULL);

	int center = wav_length/2; // 中心のサンプル番号
	TRACE(L"%d\n", center);

	double cuttime = 0.046440; // 切り出す長さ[s]
	int wavdata_length = int(cuttime/2*Fs)*2;
	TRACE(L"%d wavdata length\n", wavdata_length);
	double *wavdata = new double[fftsize];
	float *p_wav = wav + center - wavdata_length/2 - 1;
	for (int i = 0; i < fftsize; ++i, ++p_wav)
	{
		if (i < wavdata_length)
		{
			// ハニング窓
			wavdata[i] = han_window(i, wavdata_length) * double(*p_wav);
			//wavdata[i] = double(*p_wav);
			//TRACE(L"%e\t%f\n", *p_wav, wavdata[i]);
		} else {
			wavdata[i] = 0;
		}
	}

	rdft(fftsize, 1, wavdata, ip, w); // 離散フーリエ変換

	double Adft_log[fftsize/2+1]; // 対数振幅スペクトル
	Adft_log[0] = log10(wavdata[0]) / fftsize;
	Adft_log[fftsize/2] = log10(wavdata[1]) / fftsize;
	TRACE(L"%f\t%f\t%e\n", wavdata[0], 0.0, Adft_log[0]);
	for (int i = 1; i < fftsize/2; ++i)
	{
		Adft_log[i] = log10(power(wavdata[i*2], wavdata[i*2+1])) / fftsize;
		//TRACE(L"%f\t%f\t%e\n", wavdata[i*2], wavdata[i*2+1], Adft_log[i]);
	}

	//ダイアログの作成
	HWND hDlg = CreateDialogParam(hInstance, MAKEINTRESOURCE(IDD_DIALOG1), NULL, DlgProc, 0);

	// 対数振幅スペクトル表示
	GetDlgItemRect(hDlg, IDC_PANE1, (RECT&)rcPane1);
	hdcPane1 = CreateBackBuffer(hDlg, rcPane1.width(), rcPane1.height());
	PatBlt(hdcPane1, 0, 0, rcPane1.width(), rcPane1.height(), WHITENESS);
	HPEN hpenBlue = CreatePen(PS_SOLID, 1, RGB(0, 0, 255));
	SelectObject(hdcPane1, hpenBlue);
	HFONT hFont = (HFONT)GetStockObject(SYSTEM_FONT);
	SelectObject(hdcPane1, hFont);
	TextOut(hdcPane1, int(rcPane1.width()/2), 0, L"対数振幅スペクトル", 9);
	double x_zoom = 1.5;
	double y_zoom = 50000;
	int y0 = -100 + rcPane1.height();
	MoveToEx(hdcPane1, 0, int(y0 - Adft_log[0]*y_zoom), NULL);
	for (int x = 1; x <= fftsize/2; ++x)
	{
		LineTo(hdcPane1, int(x/x_zoom), int(y0 - Adft_log[x]*y_zoom));
	}

	// 逆フーリエ変換
	double *cps = new double [fftsize];
	for (int i = 0; i < fftsize/2; ++i)
	{
		cps[i*2] = Adft_log[i];
		cps[i*2+1] = 0.0;
	}
	cps[1] = Adft_log[fftsize/2];
	rdft(fftsize, -1, cps, ip, w); // 離散フーリエ変換

	// ケプストラム表示
	GetDlgItemRect(hDlg, IDC_PANE2, (RECT&)rcPane2);
	hdcPane2 = CreateBackBuffer(hDlg, rcPane2.width(), rcPane2.height());
	PatBlt(hdcPane2, 0, 0, rcPane2.width(), rcPane2.height(), WHITENESS);
	SelectObject(hdcPane2, hpenBlue);
	SelectObject(hdcPane2, hFont);
	TextOut(hdcPane2, int(rcPane2.width()/2), 0, L"ケプストラム", 6);
	x_zoom = 1.5;
	y_zoom = 500;
	y0 = -30 + rcPane2.height();
	MoveToEx(hdcPane2, 0, int(y0 - cps[0]*y_zoom), NULL);
	for (int x = 0; x < fftsize; ++x)
	{
		LineTo(hdcPane2, int(x/x_zoom), int(y0 - cps[x]*y_zoom));
		//TRACE(L"%e\n", cps[x]);
	}

	// 基本周波数ピーク抽出
	GetDlgItemRect(hDlg, IDC_PANE3, (RECT&)rcPane3);
	hdcPane3 = CreateBackBuffer(hDlg, rcPane3.width(), rcPane3.height());
	PatBlt(hdcPane3, 0, 0, rcPane3.width(), rcPane3.height(), WHITENESS);
	SelectObject(hdcPane3, hpenBlue);
	SelectObject(hdcPane3, hFont);
	SelectObject(hdcPane3, hFont);
	TextOut(hdcPane3, int(rcPane3.width()/2), 0, L"基本周波数ピーク", 8);
	x_zoom = 1.5;
	y_zoom = 3000;
	y0 = -30 +  + rcPane2.height();
	int low_cut_num = fftsize/2/20;
	MoveToEx(hdcPane3, 0, y0, NULL);
	LineTo(hdcPane3, int(low_cut_num/x_zoom), y0);
	for (int x = low_cut_num; x < fftsize/2; ++x)
	{
		LineTo(hdcPane3, int(x/x_zoom), int(y0 - cps[x]*y_zoom));
	}

	// 基本周波数に変換
	int max_n = find_max(cps + low_cut_num, fftsize/2 - low_cut_num) + low_cut_num;
	TRACE(L"%d max_n\n", max_n);
	double peakQuefrency = 1.0 / Fs * max_n;
	double f0 = 1.0 / peakQuefrency;
	TRACE(L"%f Hz\n", f0);
	wchar_t str[32];
	int len = swprintf_s(str, sizeof(str)/sizeof(wchar_t), L"%f Hz", f0);
	TextOut(hdcPane3, int(max_n/x_zoom), int(y0 - cps[max_n]*y_zoom), str, len);

	InvalidateRect(hDlg, NULL, FALSE);
	ShowWindow(hDlg, SW_SHOW);
	UpdateWindow(hDlg);

	//メッセージループ
	MSG msg;
	while(GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	CloseHandle(hFile);

	return msg.wParam;
}

//ダイアログプロシージャ
INT CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hDlg, &ps);
			BitBlt(hdc, rcPane1.left, rcPane1.top, rcPane1.width(), rcPane1.height(), hdcPane1, 0, 0, SRCCOPY);
			BitBlt(hdc, rcPane2.left, rcPane2.top, rcPane2.width(), rcPane2.height(), hdcPane2, 0, 0, SRCCOPY);
			BitBlt(hdc, rcPane3.left, rcPane3.top, rcPane3.width(), rcPane3.height(), hdcPane3, 0, 0, SRCCOPY);
			EndPaint(hDlg, &ps);
			return TRUE;
		}
	case WM_CLOSE:
		DestroyWindow(hDlg);
		PostQuitMessage(0);
		return TRUE;
	}
	return FALSE;
}

void GetDlgItemRect(HWND hDlg, int nIDDlgItem, RECT& rc)
{
	HWND hItem = GetDlgItem(hDlg, nIDDlgItem);
	GetWindowRect(hItem, &rc);
	POINT pt;
	pt.y = rc.top;
	pt.x = rc.left;
	ScreenToClient(hDlg, &pt);
	rc.bottom = pt.y + rc.bottom - rc.top;
	rc.top = pt.y;
	rc.right = pt.x + rc.right - rc.left;
	rc.left = pt.x;
}

HDC CreateBackBuffer(HWND hWnd, const int width, const int height)
{
	HDC hDC = GetDC(hWnd);
	HBITMAP hBmp = CreateCompatibleBitmap(hDC, width, height);
	HDC hdcBack = CreateCompatibleDC(hDC);
	SelectObject(hdcBack, hBmp);
	return hdcBack;
}
