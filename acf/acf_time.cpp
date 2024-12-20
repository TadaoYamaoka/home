#include <windows.h>
#include <strsafe.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <float.h>

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

inline double power(const double re, const double im)
{
    return re*re + im*im;
}

inline double han_window(const int i, size_t size)
{
    return 0.5 - 0.5 * cos(2.0 * M_PI * i / size);
}

inline int find_max(const double *a, size_t size)
{
    int max_i = 0;
    double max_value = a[0];
    for (size_t i = 1; i < size; ++i)
    {
        if (a[i] > max_value)
        {
            max_i = i;
            max_value = a[i];
        }
    }
    return max_i;
}

int find_peak(const double* a, size_t size)
{
    double max_value = DBL_MIN;
    int max_idx = 0;
    double dy = 0;
    for (size_t i = 1; i < size; ++i)
    {
        double dy_pre = dy;
        dy = a[i] - a[i-1];
        if (dy_pre > 0 && dy <= 0)
        {
            if (a[i] > max_value)
            {
                max_value = a[i];
                max_idx = i;
            }
        }
    }
    return max_idx;
}

const DWORD fftsize = 2048; // フーリエ変換の次数, 周波数ポイントの数
// FFT用ワークテーブル
int ip[46+2];
double w[fftsize*5/4]; // cos/sin work table

int wmain()
{
    HANDLE hFile = CreateFile(L"a.wav", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    RiffHeader header;
    DWORD readsize;
    ReadFile(hFile, &header, sizeof(header), &readsize, NULL);

    FormatChunk fmt;
    ReadFile(hFile, &fmt, sizeof(fmt), &readsize, NULL);

    int Fs = fmt.dwSamplesPerSec;

    DataChunk data;
    ReadFile(hFile, &data, sizeof(data), &readsize, NULL);

    DWORD wav_length = data.chunkSize/sizeof(float);
    float *wav = new float[wav_length];
    ReadFile(hFile, wav, data.chunkSize, &readsize, NULL);

    int center = wav_length/2; // 中心のサンプル番号

    double cuttime = 0.046440; // 切り出す長さ[s]
    int wavdata_length = int(cuttime/2*Fs)*2;
    double *wavdata = new double[fftsize];
    double *wavdata1 = new double[fftsize];
    double *wavdata2 = new double[fftsize];
    float *p_wav = wav + center - wavdata_length/2 - 1;
    for (int i = 0; i < fftsize; ++i, ++p_wav)
    {
        if (i < wavdata_length)
        {
            // ハニング窓
            wavdata[i] = han_window(i, wavdata_length) * double(*p_wav);
            wavdata2[i] = wavdata[i]; // コピーを保持
        } else {
            wavdata[i] = 0;
        }
    }

    double *acf = new double [fftsize];
    double *acf2 = new double[fftsize];

    DWORD time_fft = 0;
    DWORD time_nofft = 0;

    for (int c = 0; c < 1000; ++c)
    {
        CopyMemory(wavdata1, wavdata, sizeof(double)*fftsize);

        DWORD time0 = GetTickCount();
        rdft(fftsize, 1, wavdata1, ip, w); // 離散フーリエ変換

        // パワースペクトルから自己相関関数に変換
        for (int i = 0; i < fftsize/2; ++i)
        {
            acf[i*2] = power(wavdata1[i*2], wavdata1[i*2+1]);
            acf[i*2+1] = 0.0;
        }
        acf[1] = power(wavdata1[1], 0);
        rdft(fftsize, -1, acf, ip, w); // 逆フーリエ変換

        DWORD time1 = GetTickCount();

        // 自己相関関数を計算
        ZeroMemory(acf2, sizeof(double)*fftsize);
        for (int m = 0; m < fftsize; ++m)
        {
            for (int n = 0; n < fftsize - m; ++n)
            {
                acf2[m] += wavdata2[n] * wavdata2[n+m];
            }
        }
        DWORD time2 = GetTickCount();

        time_fft   += time1 - time0;
        time_nofft += time2 - time1;
    }
    // 基本周波数に変換
    int max_n = find_peak(acf, fftsize/2);
    double peakQuefrency = 1.0 / Fs * max_n;
    double f0 = 1.0 / peakQuefrency;
    wprintf(L"%f Hz\n", f0);

    // 基本周波数に変換
    max_n = find_peak(acf2, fftsize/2);
    peakQuefrency = 1.0 / Fs * max_n;
    f0 = 1.0 / peakQuefrency;
    wprintf(L"%f Hz\n", f0);

    // 測定結果
    wprintf(L"fft:   %5d\n", time_fft);
    wprintf(L"nofft: %5d\n", time_nofft);

    return 0;
}
