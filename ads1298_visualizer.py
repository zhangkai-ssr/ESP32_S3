import socket
import threading
import time
import tkinter as tk
from tkinter import ttk
import numpy as np
import collections
import csv

import matplotlib
matplotlib.use('TkAgg')
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg

# ---------- Packet constants ----------
HEADER = 0xAA
VERSION = 0x02
FOOTER = 0x55
DEVICE_COUNT = 2
PACKET_SIZE = 64
DEFAULT_PORT = 3333
SAMPLE_RATE = 2000      # Hz
VREF = 4.5              # V
PGA_GAIN = 12.0
ADC_MAX = 1 << 23


# ---------- Packet parsing ----------
def u16_le(data, off):
    return data[off] | (data[off + 1] << 8)


def s24_be(data, off):
    v = (data[off] << 16) | (data[off + 1] << 8) | data[off + 2]
    return v - 0x1000000 if v & 0x800000 else v


def adc_to_uv(raw):
    return raw * VREF / (PGA_GAIN * ADC_MAX) * 1e6


def looks_like_packet(buf):
    return (len(buf) == PACKET_SIZE and buf[0] == HEADER and
            buf[1] == VERSION and buf[8] == DEVICE_COUNT and buf[-1] == FOOTER)


def extract_packets(buffer):
    packets = []
    skipped = 0
    while len(buffer) >= PACKET_SIZE:
        if looks_like_packet(buffer[:PACKET_SIZE]):
            packets.append(bytes(buffer[:PACKET_SIZE]))
            del buffer[:PACKET_SIZE]
            continue
        pos = buffer.find(bytes([HEADER]), 1)
        if pos == -1:
            skipped += len(buffer) - (PACKET_SIZE - 1)
            del buffer[:len(buffer) - (PACKET_SIZE - 1)]
            break
        skipped += pos
        del buffer[:pos]
    return packets, skipped


def parse_channels(pkt):
    """Return 16 channel values in µV."""
    vals = []
    payload = pkt[9:63]             # 54 bytes: [U17 27B][U3 27B]
    for dev in range(2):
        frame = payload[dev * 27:(dev + 1) * 27]
        for ch in range(8):         # frame[0:3]=status, frame[3:27]=data
            vals.append(adc_to_uv(s24_be(frame, 3 + ch * 3)))
    return vals


# ---------- Main GUI ----------
BG_DARK  = '#0d1117'
BG_BAR   = '#161b22'
BG_FIELD = '#21262d'
FG_MAIN  = '#e6edf3'
FG_DIM   = '#8b949e'
COLOR_RAW = '#3fb950'
COLOR_RMS = '#f78166'
COLOR_FFT = '#58a6ff'


class EMGVisualizer:
    def __init__(self, root):
        self.root = root
        self.root.title('ADS1298 EMG Visualizer  (2000 SPS)')
        self.root.configure(bg=BG_DARK)
        self.root.minsize(1000, 640)

        # Network state
        self.sock = None
        self.recv_thread = None
        self.running = False
        self.connected = False

        # Stats
        self.total_pkts = 0
        self.lost_pkts = 0
        self.last_seq = None
        self.pkt_rate = 0.0
        self._rate_cnt = 0
        self._rate_t = time.time()

        # Data lock + ring buffers (10 s max)
        self.lock = threading.Lock()
        maxbuf = SAMPLE_RATE * 10
        self.bufs = [collections.deque(maxlen=maxbuf) for _ in range(16)]

        # Recording
        self.recording = False
        self.rec_rows = []

        # Tk vars
        self.v_win   = tk.DoubleVar(value=3.0)
        self.v_ch    = tk.IntVar(value=1)
        self.v_yr    = tk.StringVar(value='auto')
        self.v_fftw  = tk.DoubleVar(value=4.0)
        self.v_status = tk.StringVar(value='Connected: False    pkt/s: --    lost: 0')
        self.v_peak  = tk.StringVar(value='Peak: -- Hz')
        self.v_snr   = tk.StringVar(value='SNR: -- dB')
        self.v_recst = tk.StringVar(value='未录制')

        self._build_toolbar()
        self._build_toolbar2()
        self._build_plot()
        self._animate()

    # ---- UI build ----
    def _build_toolbar(self):
        bar = tk.Frame(self.root, bg=BG_BAR, pady=5)
        bar.pack(fill='x')

        def lbl(text, **kw):
            kw.setdefault('fg', FG_MAIN)
            return tk.Label(bar, text=text, bg=BG_BAR, **kw)

        lbl('ESP32 IP:').pack(side='left', padx=(8, 2))
        self.e_ip = tk.Entry(bar, width=15, bg=BG_FIELD, fg=FG_MAIN,
                             insertbackground=FG_MAIN, relief='flat')
        self.e_ip.insert(0, '192.168.1.100')
        self.e_ip.pack(side='left', padx=2)

        lbl('Port:').pack(side='left', padx=(6, 2))
        self.e_port = tk.Entry(bar, width=6, bg=BG_FIELD, fg=FG_MAIN,
                               insertbackground=FG_MAIN, relief='flat')
        self.e_port.insert(0, str(DEFAULT_PORT))
        self.e_port.pack(side='left', padx=2)

        lbl('  时间窗(s):').pack(side='left', padx=(8, 2))
        tk.Spinbox(bar, from_=0.5, to=10.0, increment=0.5,
                   textvariable=self.v_win, width=5,
                   bg=BG_FIELD, fg=FG_MAIN, buttonbackground=BG_BAR,
                   relief='flat').pack(side='left', padx=2)

        lbl('  采样率:').pack(side='left', padx=(8, 2))
        lbl('2000 Hz', fg=COLOR_RAW,
            font=('Consolas', 9, 'bold')).pack(side='left')

        self.btn_conn = tk.Button(bar, text='Connect', width=10,
                                  bg='#238636', fg='white', relief='flat',
                                  activebackground='#2ea043',
                                  command=self._toggle_connect)
        self.btn_conn.pack(side='left', padx=12)

        tk.Label(bar, textvariable=self.v_status, bg=BG_BAR,
                 fg=COLOR_RMS, font=('Consolas', 9)).pack(side='right', padx=10)

    def _build_toolbar2(self):
        bar2 = tk.Frame(self.root, bg=BG_DARK, pady=3)
        bar2.pack(fill='x')

        def lbl(text):
            return tk.Label(bar2, text=text, bg=BG_DARK, fg=FG_MAIN)

        lbl('画图通道:').pack(side='left', padx=(8, 2))
        ttk.Combobox(bar2, textvariable=self.v_ch, width=4,
                     values=list(range(1, 17)),
                     state='readonly').pack(side='left', padx=2)

        lbl('  Y轴范围:').pack(side='left', padx=(8, 2))
        ttk.Combobox(bar2, textvariable=self.v_yr, width=7,
                     values=['auto', '±100', '±500', '±1000', '±5000'],
                     state='readonly').pack(side='left', padx=2)

        lbl('  FFT窗口(s):').pack(side='left', padx=(8, 2))
        ttk.Combobox(bar2, textvariable=self.v_fftw, width=4,
                     values=[1, 2, 4, 8],
                     state='readonly').pack(side='left', padx=2)

        tk.Label(bar2, textvariable=self.v_peak,
                 bg=BG_DARK, fg=COLOR_FFT).pack(side='left', padx=12)
        tk.Label(bar2, textvariable=self.v_snr,
                 bg=BG_DARK, fg=COLOR_FFT).pack(side='left', padx=4)

        tk.Label(bar2, textvariable=self.v_recst,
                 bg=BG_DARK, fg=FG_DIM).pack(side='right', padx=6)
        self.btn_rec = tk.Button(bar2, text='开始录制', width=8,
                                 bg=BG_FIELD, fg=FG_MAIN, relief='flat',
                                 command=self._toggle_record)
        self.btn_rec.pack(side='right', padx=6)

    def _build_plot(self):
        self.fig = Figure(facecolor=BG_DARK)
        gs = self.fig.add_gridspec(3, 1, hspace=0.45)
        self.ax1 = self.fig.add_subplot(gs[0])
        self.ax2 = self.fig.add_subplot(gs[1])
        self.ax3 = self.fig.add_subplot(gs[2])

        titles = ['EMG Signal (raw)', 'EMG Envelope (50ms RMS)', 'EMG Spectrum']
        ylabels = ['µV', 'RMS µV', 'Amplitude']
        for ax, t, yl in zip([self.ax1, self.ax2, self.ax3], titles, ylabels):
            ax.set_facecolor(BG_DARK)
            ax.set_title(t, color=FG_MAIN, fontsize=9, pad=3)
            ax.set_ylabel(yl, color=FG_DIM, fontsize=8)
            ax.tick_params(colors=FG_DIM, labelsize=7)
            for sp in ax.spines.values():
                sp.set_color('#30363d')
        self.ax3.set_xlabel('Frequency (Hz)', color=FG_DIM, fontsize=8)

        self.ln1, = self.ax1.plot([], [], color=COLOR_RAW, lw=0.8)
        self.ln2, = self.ax2.plot([], [], color=COLOR_RMS, lw=1.0)
        self.ln3, = self.ax3.plot([], [], color=COLOR_FFT, lw=0.9)
        self.ax3.set_xlim(0, 500)

        self.canvas = FigureCanvasTkAgg(self.fig, master=self.root)
        self.canvas.get_tk_widget().pack(fill='both', expand=True)

    # ---- Connection ----
    def _toggle_connect(self):
        if self.connected:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        host = self.e_ip.get().strip()
        try:
            port = int(self.e_port.get().strip())
        except ValueError:
            port = DEFAULT_PORT
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(4.0)
            s.connect((host, port))
            s.settimeout(2.0)
            self.sock = s
            self.connected = True
            self.running = True
            self.btn_conn.config(text='Disconnect', bg='#da3633')
            self.recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
            self.recv_thread.start()
        except Exception as e:
            self.v_status.set(f'Connect failed: {e}')

    def _disconnect(self):
        self.running = False
        self.connected = False
        if self.sock:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None
        self.btn_conn.config(text='Connect', bg='#238636')
        self.v_status.set(f'Connected: False    pkt/s: --    lost: {self.lost_pkts}')

    # ---- Recv thread ----
    def _recv_loop(self):
        buf = bytearray()
        while self.running:
            try:
                chunk = self.sock.recv(8192)
                if not chunk:
                    break
                buf.extend(chunk)
                pkts, _ = extract_packets(buf)
                for p in pkts:
                    self._process_packet(p)
                self._rate_cnt += len(pkts)
                now = time.time()
                dt = now - self._rate_t
                if dt >= 1.0:
                    self.pkt_rate = self._rate_cnt / dt
                    self._rate_cnt = 0
                    self._rate_t = now
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    self.root.after(0, lambda err=e: self.v_status.set(f'Error: {err}'))
                break
        self.root.after(0, self._disconnect)

    def _process_packet(self, pkt):
        seq = u16_le(pkt, 2)
        if self.last_seq is not None:
            exp = (self.last_seq + 1) & 0xFFFF
            if seq != exp:
                self.lost_pkts += (seq - exp) & 0xFFFF
        self.last_seq = seq
        self.total_pkts += 1
        vals = parse_channels(pkt)
        with self.lock:
            for i, v in enumerate(vals):
                self.bufs[i].append(v)
            if self.recording:
                self.rec_rows.append(vals)

    # ---- Animation (~20 FPS) ----
    def _animate(self):
        try:
            ch = self.v_ch.get() - 1
            win = int(self.v_win.get() * SAMPLE_RATE)
            fftw = int(self.v_fftw.get() * SAMPLE_RATE)
            with self.lock:
                raw = np.array(list(self.bufs[ch]))

            if len(raw) >= 4:
                # --- Raw signal ---
                sig = raw[-win:] if len(raw) > win else raw
                t = np.arange(len(sig)) / SAMPLE_RATE
                self.ln1.set_data(t, sig)
                self.ax1.set_xlim(0, max(t[-1], 0.01))
                yr = self.v_yr.get()
                if yr == 'auto':
                    rng = np.ptp(sig)
                    pad = max(rng * 0.1, 1.0)
                    self.ax1.set_ylim(sig.min() - pad, sig.max() + pad)
                else:
                    lim = float(yr.replace('\xb1', ''))
                    self.ax1.set_ylim(-lim, lim)

                # --- RMS envelope (50 ms) ---
                rms_w = max(1, int(0.05 * SAMPLE_RATE))
                n_seg = len(sig) // rms_w
                if n_seg >= 1:
                    segs = sig[:n_seg * rms_w].reshape(n_seg, rms_w)
                    rms = np.sqrt(np.mean(segs ** 2, axis=1))
                    t_r = np.arange(n_seg) * rms_w / SAMPLE_RATE
                    self.ln2.set_data(t_r, rms)
                    self.ax2.set_xlim(0, max(t_r[-1], 0.01))
                    top = max(rms.max() * 1.15, 1.0)
                    self.ax2.set_ylim(0, top)

                # --- FFT ---
                fft_src = raw[-fftw:] if len(raw) > fftw else raw
                if len(fft_src) >= 64:
                    x = (fft_src - np.mean(fft_src)) * np.hanning(len(fft_src))
                    N = len(x)
                    freqs = np.fft.rfftfreq(N, 1.0 / SAMPLE_RATE)
                    amp = np.abs(np.fft.rfft(x)) * 2 / N
                    mask = freqs <= 500
                    f, a = freqs[mask], amp[mask]
                    self.ln3.set_data(f, a)
                    self.ax3.set_xlim(0, 500)
                    self.ax3.set_ylim(0, max(a.max() * 1.15, 1.0))
                    if len(a) > 1:
                        pi = np.argmax(a[1:]) + 1
                        pf = f[pi]
                        self.v_peak.set(f'Peak: {pf:.1f} Hz')
                        sig_m = np.abs(f - pf) <= 5
                        noi_m = ~sig_m & (f > 0)
                        if np.any(sig_m) and np.any(noi_m):
                            sp = np.mean(a[sig_m] ** 2)
                            np_ = np.mean(a[noi_m] ** 2)
                            if np_ > 0:
                                self.v_snr.set(f'SNR: {10*np.log10(sp/np_):.1f} dB')

            if self.connected:
                self.v_status.set(
                    f'Connected: True    pkt/s: {self.pkt_rate:.0f}'
                    f'    total: {self.total_pkts}    lost: {self.lost_pkts}')

            self.canvas.draw_idle()
        except Exception:
            pass
        self.root.after(50, self._animate)

    # ---- Recording ----
    def _toggle_record(self):
        if not self.recording:
            with self.lock:
                self.rec_rows = []
            self.recording = True
            self.btn_rec.config(text='停止录制', bg='#b91c1c')
            self.v_recst.set('录制中...')
        else:
            self.recording = False
            self.btn_rec.config(text='开始录制', bg=BG_FIELD)
            self._save_csv()

    def _save_csv(self):
        with self.lock:
            rows = list(self.rec_rows)
        if not rows:
            self.v_recst.set('无数据')
            return
        fname = f'emg_{time.strftime("%Y%m%d_%H%M%S")}.csv'
        try:
            with open(fname, 'w', newline='') as f:
                w = csv.writer(f)
                w.writerow([f'CH{i+1}_uV' for i in range(16)])
                w.writerows(rows)
            self.v_recst.set(f'已保存: {fname}')
        except Exception as e:
            self.v_recst.set(f'保存失败: {e}')


def main():
    root = tk.Tk()
    root.geometry('1280x760')
    app = EMGVisualizer(root)
    root.protocol('WM_DELETE_WINDOW', lambda: (app._disconnect(), root.destroy()))
    root.mainloop()


if __name__ == '__main__':
    main()
