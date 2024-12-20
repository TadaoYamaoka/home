global Fs;
Fs=8000;
N=2048;
freq = [55, 58, 440, 466, 1760, 1865];
t = 0:1/Fs:(N-1)/Fs;
global f;
f = zeros(1,N);
for i=1:length(freq)
    global f;
    f += sin(2*pi*freq(i).*t);
endfor

global sigma omega;
omega = 2*pi;

function r = gabor(t)
    global sigma omega;
    r = 1/sqrt(2*pi*sigma^2) .* exp(-t.^2./(2*sigma^2)) .* exp(-i * omega .* t);
endfunction

function [WT,frequency] = gwt(wavdata)
    global Fs sigma omega;
    size = 2048;
    frequency = [];
    for y=1:12*2*6
        freq = 55*2^((y-1)/(12*2));
        frequency(y) = freq;
        a = 1/freq;
        dt = a*sigma*sqrt(-2*log(0.01));
        dx = fix(dt * Fs);
    
        x=fix(size/2);
        b=x;
        v = 0;
        for m=-dx:dx
            n = b + m;
            if n > 0 && n <= size
                v += wavdata(n) * gabor(m/Fs/a);
            endif
        endfor
        v /= sqrt(a);
        WT(y) = abs(v);
    endfor
endfunction

F = fft(f);
subplot(5,1,1);semilogx([5:N/2].*Fs./N, abs(F(5:N/2)));

sigma = 2;
[WT,frequency] = gwt(f);
subplot(5,1,2);semilogx(frequency,WT);

sigma = 4;
[WT,frequency] = gwt(f);
subplot(5,1,3);semilogx(frequency,WT);

sigma = 6;
[WT,frequency] = gwt(f);
subplot(5,1,4);semilogx(frequency,WT);

sigma = 8;
[WT,frequency] = gwt(f);
subplot(5,1,5);semilogx(frequency,WT);
