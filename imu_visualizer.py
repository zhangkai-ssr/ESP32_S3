#!/usr/bin/env python3
"""
IMU 三维姿态可视化工具

功能：
  - 连接 ESP32 LSM9DS1TR TCP 数据流（端口 3334）
  - 陀螺仪零偏自动校准（连接后静止 2 秒完成）
  - Mahony AHRS 滤波器（加速度计修正 Roll/Pitch，磁力计锁定 Yaw）
  - 实时三维姿态渲染 + 传感器数据面板
  - CSV 数据录制（按 [L] 键或界面 REC 按钮）

用法：
    python imu_visualizer.py [ESP32_IP]

依赖：
    pip install matplotlib numpy

Yaw 漂移根因说明：
  - 若陀螺仪三轴存在零偏（静止时输出非零角速度），积分后导致姿态旋转
  - 启动后前 2 s 自动采集零偏并从后续数据中扣除
  - 磁力计数据通过 Mahony 算法提供绝对航向参考，消除 Yaw 长期漂移
"""

import csv
import os
import socket
import struct
import sys
import time
import threading
import numpy as np
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from matplotlib.widgets import Button
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
from datetime import datetime

# ── 通信协议常量 ──────────────────────────────────────────────────────────────
IMU_TCP_PORT    = 3334          # ESP32 IMU TCP 端口
PACKET_HEADER   = 0xBB          # 包头
PACKET_FOOTER   = 0x55          # 包尾
PACKET_VERSION  = 0x01          # 协议版本
PACKET_TYPE_IMU = 0x20          # 数据包类型：IMU
BATCH_SAMPLES   = 10            # 每包样本数
SAMPLE_BYTES    = 20            # 每个样本字节数：ax ay az gx gy gz mx my mz rsvd × int16 LE
PACKET_OVERHEAD = 11            # 包头开销：hdr+ver+type+seq(2)+ts(4)+n+ftr
PACKET_SIZE     = PACKET_OVERHEAD + BATCH_SAMPLES * SAMPLE_BYTES  # 总包大小 211 字节

# ── 传感器量程换算系数（LSM9DS1TR，来源：lsm9ds1.h）─────────────────────────
ACCEL_SENS    = 0.122e-3 * 9.80665          # 加速度计：±4g 量程 → m/s² / LSB
GYRO_SENS_RAD = 17.5e-3 * (3.14159265 / 180.0)  # 陀螺仪：±500 dps → rad/s / LSB
GYRO_SENS_DEG = 17.5e-3                     # 陀螺仪：±500 dps → deg/s / LSB
MAG_SENS      = 0.29e-3                     # 磁力计：±8 gauss → gauss / LSB

SAMPLE_DT   = 1.0 / 100.0  # 采样周期（100 Hz）
CAL_SAMPLES = 200           # 陀螺零偏校准窗口：200 个样本 = 2 秒（100 Hz）


# ── Mahony AHRS 姿态融合滤波器 ────────────────────────────────────────────────
# 原理：
#   - 加速度计提供重力方向参考，修正 Roll / Pitch
#   - 磁力计提供水平磁北参考，修正 Yaw（消除陀螺积分漂移）
#   - 陀螺仪提供高频角速度，积分得到平滑姿态
#   - Kp（比例增益）控制传感器修正力度；Ki（积分增益）消除稳态误差
class MahonyFilter:
    def __init__(self, Kp: float = 2.0, Ki: float = 0.005):
        self.Kp = Kp           # 比例增益（越大修正越快，过大会引起震荡）
        self.Ki = Ki           # 积分增益（消除陀螺稳态偏差）
        self.q  = np.array([1.0, 0.0, 0.0, 0.0], dtype=np.float64)  # 姿态四元数 [w,x,y,z]
        self._integral = np.zeros(3, dtype=np.float64)               # 积分误差累积项

    def reset(self):
        """重置四元数为单位姿态，清空积分项。"""
        self.q[:] = [1.0, 0.0, 0.0, 0.0]
        self._integral[:] = 0.0

    def update(self, ax: float, ay: float, az: float,
               gx: float, gy: float, gz: float,
               mx: float, my: float, mz: float, dt: float) -> None:
        """
        更新姿态四元数（已扣除陀螺零偏后的值）。
          ax/ay/az : 加速度计，单位 m/s²
          gx/gy/gz : 陀螺仪，单位 rad/s（零偏已扣除）
          mx/my/mz : 磁力计，单位 gauss
          dt       : 时间步长，单位 s
        """
        if dt <= 0:
            return
        # 归一化加速度计，得到重力方向单位向量
        norm = (ax*ax + ay*ay + az*az) ** 0.5
        if norm < 1e-6:  # 加速度接近零（自由落体），跳过本次修正
            return
        ax /= norm; ay /= norm; az /= norm

        qw, qx, qy, qz = self.q

        # 由当前四元数估算传感器坐标系下的重力方向（旋转矩阵第三列）
        vx = 2.0*(qx*qz - qw*qy)
        vy = 2.0*(qw*qx + qy*qz)
        vz = qw*qw - qx*qx - qy*qy + qz*qz

        # 加速度计误差 = 估算重力 × 实测重力（叉乘），修正 Roll / Pitch
        ex = ay*vz - az*vy
        ey = az*vx - ax*vz
        ez = ax*vy - ay*vx

        # ── 磁力计修正 Yaw ─────────────────────────────────────────────────────
        norm_m = (mx*mx + my*my + mz*mz) ** 0.5
        if norm_m > 1e-6:  # 磁力计有效才做修正
            mx /= norm_m; my /= norm_m; mz /= norm_m

            # 将磁力计测量旋转到世界坐标系：h = q ⊗ m ⊗ q*
            hx = 2.0*(mx*(0.5 - qy*qy - qz*qz) + my*(qx*qy - qw*qz) + mz*(qx*qz + qw*qy))
            hy = 2.0*(mx*(qx*qy + qw*qz) + my*(0.5 - qx*qx - qz*qz) + mz*(qy*qz - qw*qx))
            hz = 2.0*(mx*(qx*qz - qw*qy) + my*(qy*qz + qw*qx) + mz*(0.5 - qx*qx - qy*qy))

            # 投影到水平面，去除磁倾角影响：参考磁场 b = [bx, 0, bz]
            bx = (hx*hx + hy*hy) ** 0.5
            bz = hz

            # 由参考磁场 b 和当前四元数，估算传感器系下的磁场方向 w
            wx = 2.0*(bx*(0.5 - qy*qy - qz*qz) + bz*(qx*qz - qw*qy))
            wy = 2.0*(bx*(qx*qy - qw*qz)        + bz*(qw*qx + qy*qz))
            wz = 2.0*(bx*(qw*qy + qx*qz)        + bz*(0.5 - qx*qx - qy*qy))

            # 磁力计误差 = 估算方向 × 实测方向（叉乘），主要修正 Yaw
            ex += my*wz - mz*wy
            ey += mz*wx - mx*wz
            ez += mx*wy - my*wx

        # PI 反馈：将误差叠加到角速度上（比例项 + 积分项）
        self._integral += np.array([ex, ey, ez]) * (self.Ki * dt)
        gx += self.Kp*ex + self._integral[0]
        gy += self.Kp*ey + self._integral[1]
        gz += self.Kp*ez + self._integral[2]

        # 四元数微分方程积分：q += 0.5 * dt * Ω(ω) ⊗ q
        half_dt = 0.5 * dt
        dq = np.array([
            -qx*gx - qy*gy - qz*gz,
             qw*gx + qy*gz - qz*gy,
             qw*gy - qx*gz + qz*gx,
             qw*gz + qx*gy - qy*gx,
        ]) * half_dt
        self.q += dq
        self.q /= np.linalg.norm(self.q)  # 归一化保证单位四元数

    def rotation_matrix(self) -> np.ndarray:
        """由四元数计算 3×3 旋转矩阵（机体系 → 世界系）。"""
        qw, qx, qy, qz = self.q
        return np.array([
            [1-2*(qy*qy+qz*qz),   2*(qx*qy-qw*qz),   2*(qx*qz+qw*qy)],
            [  2*(qx*qy+qw*qz), 1-2*(qx*qx+qz*qz),   2*(qy*qz-qw*qx)],
            [  2*(qx*qz-qw*qy),   2*(qy*qz+qw*qx), 1-2*(qx*qx+qy*qy)],
        ], dtype=np.float64)

    def euler_deg(self):
        """从旋转矩阵计算欧拉角（单位：度）：Roll / Pitch / Yaw。"""
        R = self.rotation_matrix()
        roll  = np.degrees(np.arctan2(R[2, 1], R[2, 2]))           # 横滚角
        pitch = np.degrees(np.arcsin(np.clip(-R[2, 0], -1.0, 1.0)))  # 俯仰角
        yaw   = np.degrees(np.arctan2(R[1, 0], R[0, 0]))           # 偏航角
        return roll, pitch, yaw


# ── CSV 数据录制器 ────────────────────────────────────────────────────────────
# 文件保存路径：脚本目录下 imuLOG/imu_log_YYYYMMDD_HHMMSS.csv
# 列：时间戳 | 加速度(m/s²) | 陀螺(dps，已扣零偏) | 磁场(gauss) | 欧拉角(°)
class DataLogger:
    _HEADER = [
        'monotonic_s',
        'ax_ms2', 'ay_ms2', 'az_ms2',
        'gx_dps', 'gy_dps', 'gz_dps',
        'mx_gauss', 'my_gauss', 'mz_gauss',
        'roll_deg', 'pitch_deg', 'yaw_deg',
    ]

    def __init__(self):
        self._lock    = threading.Lock()  # 线程锁（接收线程写，UI 线程读）
        self._file    = None
        self._writer  = None
        self._path    = ''
        self._count   = 0
        self.enabled  = False

    def start(self):
        """开始录制，创建带时间戳的 CSV 文件。"""
        with self._lock:
            if self._file is not None:
                return
            ts      = datetime.now().strftime('%Y%m%d_%H%M%S')
            log_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'imuLOG')
            os.makedirs(log_dir, exist_ok=True)  # 目录不存在时自动创建
            self._path   = os.path.join(log_dir, f'imu_log_{ts}.csv')
            self._file   = open(self._path, 'w', newline='', encoding='utf-8')
            self._writer = csv.writer(self._file)
            self._writer.writerow(self._HEADER)
            self._count  = 0
            self.enabled = True
        print(f'[log] 录制开始 → {self._path}')

    def stop(self):
        """停止录制，关闭文件。"""
        with self._lock:
            if self._file is None:
                return
            self._file.close()
            self._file   = None
            self._writer = None
            self.enabled = False
        print(f'[log] 录制结束  ({self._count} 行) → {self._path}')

    def toggle(self):
        """切换录制状态（开 ↔ 关）。"""
        if self.enabled:
            self.stop()
        else:
            self.start()

    def write(self, mono_s,
              ax, ay, az,
              gx, gy, gz,
              mx, my, mz,
              roll, pitch, yaw):
        with self._lock:
            if not self.enabled or self._writer is None:
                return
            self._writer.writerow([
                f'{mono_s:.6f}',
                f'{ax:.5f}', f'{ay:.5f}', f'{az:.5f}',
                f'{gx:.4f}', f'{gy:.4f}', f'{gz:.4f}',
                f'{mx:.5f}', f'{my:.5f}', f'{mz:.5f}',
                f'{roll:.3f}', f'{pitch:.3f}', f'{yaw:.3f}',
            ])
            self._count += 1

    @property
    def row_count(self):
        return self._count

    @property
    def path(self):
        return self._path


_logger = DataLogger()


# ── 线程共享状态（接收线程写 / UI 线程读，均需持锁） ──────────────────────────
class _State:
    def __init__(self):
        self.lock       = threading.Lock()
        self.filter     = MahonyFilter()    # 姿态融合滤波器
        self.accel      = np.zeros(3)       # 最新加速度，单位 m/s²
        self.gyro_dps   = np.zeros(3)       # 最新角速度（原始），单位 deg/s
        self.mag        = np.zeros(3)       # 最新磁场，单位 gauss
        self.gyro_bias  = np.zeros(3)       # 陀螺零偏（校准值），单位 rad/s
        self.cal_done   = False             # 零偏校准是否完成
        self.cal_count  = 0                 # 已采集的校准样本数
        self._cal_sum   = np.zeros(3)       # 校准期间角速度累加（rad/s）
        self.pkt_count  = 0                 # 已接收包数
        self.drop_count = 0                 # 丢弃字节/包数
        self.connected  = False             # TCP 连接状态
        self.status     = "等待连接..."

_state = _State()


# ── 数据包解析 ────────────────────────────────────────────────────────────────
def _parse_samples(data: bytes):
    """
    解析一个完整数据包，返回 (ax,ay,az,gx,gy,gz,mx,my,mz) 原始 int16 元组列表。
    包格式校验失败时返回 None。
    """
    if (len(data) != PACKET_SIZE
            or data[0] != PACKET_HEADER
            or data[1] != PACKET_VERSION
            or data[2] != PACKET_TYPE_IMU
            or data[-1] != PACKET_FOOTER):
        return None
    n = data[9]   # 实际样本数（通常为 BATCH_SAMPLES=10）
    out = []
    off = 10
    for _ in range(n):
        vals = struct.unpack_from('<9h', data, off)   # 解包 9 个 int16 小端
        out.append(vals)
        off += SAMPLE_BYTES
    return out


# ── TCP 数据接收线程 ──────────────────────────────────────────────────────────
def _recv_once(host: str) -> None:
    """单次连接并持续接收，连接失败时抛出异常由外层 _recv_loop 捕获重连。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    sock.connect((host, IMU_TCP_PORT))          # raises on failure

    with _state.lock:
        _state.connected = True
        _state.status    = f"Connected  {host}:{IMU_TCP_PORT}"
    print(f"[imu] {_state.status}  (PACKET_SIZE={PACKET_SIZE})")

    buf = bytearray()   # 接收缓冲区（流式拼包）
    consecutive_to = 0  # 连续超时计数

    try:
        while True:
            try:
                chunk = sock.recv(4096)
            except TimeoutError:
                consecutive_to += 1
                with _state.lock:
                    _state.status = (f"已连接  {host}:{IMU_TCP_PORT}  "
                                     f"(无数据 ×{consecutive_to})")
                if consecutive_to >= 5:  # 连续 5 次超时则断开重连
                    print("[imu] 连续 5 次超时，断开重连")
                    break
                continue
            if not chunk:  # 服务端关闭连接
                break
            consecutive_to = 0
            buf.extend(chunk)

            # 逐包从缓冲区中提取完整数据包
            while len(buf) >= PACKET_SIZE:
                if buf[0] != PACKET_HEADER:  # 包头不对，向前搜索有效包头
                    idx = buf.find(bytes([PACKET_HEADER]), 1)
                    if idx == -1:  # 缓冲区中无有效包头，全部丢弃
                        buf.clear()
                        break
                    with _state.lock:
                        _state.drop_count += idx
                    del buf[:idx]  # 跳过垃圾字节
                    continue

                samples = _parse_samples(bytes(buf[:PACKET_SIZE]))
                del buf[:PACKET_SIZE]  # 无论解析是否成功，都消耗这段缓冲
                if samples is None:    # 包头对但内容校验失败
                    with _state.lock:
                        _state.drop_count += 1
                    continue

                mono_now = time.monotonic()
                with _state.lock:
                    for ax_r, ay_r, az_r, gx_r, gy_r, gz_r, mx_r, my_r, mz_r in samples:
                        # 将原始 int16 换算为物理量（rad/s）
                        gx_rad = gx_r * GYRO_SENS_RAD
                        gy_rad = gy_r * GYRO_SENS_RAD
                        gz_rad = gz_r * GYRO_SENS_RAD

                        # ── 陀螺零偏校准（前 CAL_SAMPLES 个样本，设备必须静止）──
                        if not _state.cal_done:
                            _state._cal_sum += np.array([gx_rad, gy_rad, gz_rad])
                            _state.cal_count += 1
                            if _state.cal_count >= CAL_SAMPLES:
                                _state.gyro_bias = _state._cal_sum / CAL_SAMPLES
                                _state.cal_done  = True
                                bd = _state.gyro_bias * (180.0 / np.pi)
                                print(f'[校准完成] 陀螺零偏: '
                                      f'gx={bd[0]:+.3f} gy={bd[1]:+.3f} gz={bd[2]:+.3f} dps')

                        # 扣除陀螺零偏，得到真实角速度（rad/s）
                        gx_cal = gx_rad - _state.gyro_bias[0]
                        gy_cal = gy_rad - _state.gyro_bias[1]
                        gz_cal = gz_rad - _state.gyro_bias[2]

                        _state.filter.update(
                            ax_r * ACCEL_SENS,
                            ay_r * ACCEL_SENS,
                            az_r * ACCEL_SENS,
                            gx_cal, gy_cal, gz_cal,
                            mx_r * MAG_SENS,
                            my_r * MAG_SENS,
                            mz_r * MAG_SENS,
                            SAMPLE_DT,
                        )
                        roll_s, pitch_s, yaw_s = _state.filter.euler_deg()
                        _logger.write(
                            mono_now,
                            ax_r * ACCEL_SENS, ay_r * ACCEL_SENS, az_r * ACCEL_SENS,
                            gx_cal * (180.0 / np.pi),
                            gy_cal * (180.0 / np.pi),
                            gz_cal * (180.0 / np.pi),
                            mx_r * MAG_SENS, my_r * MAG_SENS, mz_r * MAG_SENS,
                            roll_s, pitch_s, yaw_s,
                        )
                    last = samples[-1]
                    _state.accel    = np.array(last[0:3], dtype=float) * ACCEL_SENS
                    _state.gyro_dps = np.array(last[3:6], dtype=float) * GYRO_SENS_DEG
                    _state.mag      = np.array(last[6:9], dtype=float) * MAG_SENS
                    _state.pkt_count += 1
                    _state.status = f"Connected  {host}:{IMU_TCP_PORT}"

    except Exception as exc:
        print(f"[imu] 接收异常: {exc}")
    finally:
        sock.close()
        with _state.lock:
            _state.connected = False
        print("[imu] 已断开连接")


def _recv_loop(host: str) -> None:
    """自动重连循环：_recv_once 断开或失败后按指数退避重试。"""
    retry = 0
    while True:
        try:
            _recv_once(host)
        except Exception as exc:
            print(f"[imu] 连接失败: {exc}")
        retry += 1
        wait = min(2 ** retry, 16)  # 退避等待：2→4→8→16 秒上限
        with _state.lock:
            _state.connected = False
            _state.status    = f"重连中，{wait}s 后重试（第 {retry} 次）…"
        print(f"[imu] {_state.status}")
        time.sleep(wait)


# ── 三维 PCB 盒体几何（宽/薄/高，模拟手环形态）──────────────────────────────
def _make_box_faces(lx=1.8, ly=0.18, lz=1.2):
    hx, hy, hz = lx/2, ly/2, lz/2
    v = np.array([  # 8 个顶点
        [-hx, -hy, -hz], [ hx, -hy, -hz], [ hx,  hy, -hz], [-hx,  hy, -hz],
        [-hx, -hy,  hz], [ hx, -hy,  hz], [ hx,  hy,  hz], [-hx,  hy,  hz],
    ])
    faces = [
        [v[0], v[1], v[2], v[3]],  # 底面  (-Z)
        [v[4], v[5], v[6], v[7]],  # 顶面  (+Z)
        [v[0], v[1], v[5], v[4]],  # 正面  (-Y)
        [v[2], v[3], v[7], v[6]],  # 背面  (+Y)
        [v[0], v[3], v[7], v[4]],  # 左面  (-X)
        [v[1], v[2], v[6], v[5]],  # 右面  (+X)
    ]
    colors = ['#1565C0', '#1976D2', '#0288D1', '#0097A7', '#00695C', '#2E7D32']
    return faces, colors

_BOX_FACES, _BOX_COLORS = _make_box_faces()


# ── Matplotlib 界面初始化 ─────────────────────────────────────────────────────
BG_DARK  = '#0d1117'
BG_PANEL = '#161b22'
CLR_X    = '#ff6b6b'
CLR_Y    = '#51cf66'
CLR_Z    = '#339af0'
CLR_TXT  = '#c9d1d9'
CLR_DIM  = '#6e7681'

matplotlib.rcParams['text.color']   = CLR_TXT
matplotlib.rcParams['axes.labelcolor'] = CLR_TXT
matplotlib.rcParams['xtick.color']  = CLR_DIM
matplotlib.rcParams['ytick.color']  = CLR_DIM

fig = plt.figure(figsize=(13, 7), facecolor=BG_DARK)
fig.canvas.manager.set_window_title('IMU 3D Orientation  —  LSM9DS1TR @ 100 Hz')

ax3d = fig.add_axes([0.0, 0.0, 0.6, 1.0], projection='3d')
ax3d.set_facecolor(BG_PANEL)
for pane in (ax3d.xaxis.pane, ax3d.yaxis.pane, ax3d.zaxis.pane):
    pane.fill = False
    pane.set_edgecolor('#30363d')

ax_txt = fig.add_axes([0.60, 0.0, 0.40, 1.0])
ax_txt.set_facecolor(BG_PANEL)
ax_txt.axis('off')

ax_btn = fig.add_axes([0.845, 0.944, 0.135, 0.050])
_btn_rec = Button(ax_btn, '●  REC', color='#21262d', hovercolor='#2d333b')
_btn_rec.label.set_color('#51cf66')
_btn_rec.label.set_fontsize(9.5)
_btn_rec.label.set_fontweight('bold')
for spine in ax_btn.spines.values():
    spine.set_edgecolor('#444d56')


def _on_btn_rec(_event):
    """REC 按钮点击回调：切换录制状态。"""
    _logger.toggle()


def _update(frame):
    # ── 加锁快照共享状态（避免与接收线程竞争）────────────────────────────────
    with _state.lock:
        R           = _state.filter.rotation_matrix()
        roll, pitch, yaw = _state.filter.euler_deg()
        accel       = _state.accel.copy()
        gyro_dps    = _state.gyro_dps.copy()
        mag         = _state.mag.copy()
        pkt_count   = _state.pkt_count
        drop_count  = _state.drop_count
        connected   = _state.connected
        status      = _state.status
        cal_done    = _state.cal_done
        cal_count   = _state.cal_count

    # ── 三维坐标轴绘制 ─────────────────────────────────────────────────────────
    ax3d.cla()
    ax3d.set_facecolor(BG_PANEL)
    LIM = 2.0
    ax3d.set_xlim(-LIM, LIM); ax3d.set_ylim(-LIM, LIM); ax3d.set_zlim(-LIM, LIM)
    ax3d.set_xlabel('X', color=CLR_X, labelpad=2)
    ax3d.set_ylabel('Y', color=CLR_Y, labelpad=2)
    ax3d.set_zlabel('Z', color=CLR_Z, labelpad=2)
    ax3d.tick_params(labelsize=7, colors=CLR_DIM)
    ax3d.set_title('Orientation  (Mahony + Mag)', color=CLR_TXT, pad=8, fontsize=11)

    # 世界坐标系网格线（淡色参考网格）
    for val in (-1, 0, 1):
        ax3d.plot([-LIM, LIM], [val, val], [0, 0], color='#21262d', lw=0.4)
        ax3d.plot([val, val], [-LIM, LIM], [0, 0], color='#21262d', lw=0.4)

    # 重力参考箭头（世界系 -Z 方向，黄色）
    ax3d.quiver(0, 0, 1.6, 0, 0, -0.9,
                color='#ffd43b', linewidth=1.2, arrow_length_ratio=0.25, alpha=0.7)
    ax3d.text(0.08, 0.08, 0.55, 'g', color='#ffd43b', fontsize=9)

    # PCB 盒体：将各面顶点通过旋转矩阵 R 变换到世界系后渲染
    rot_faces = [[R @ v for v in face] for face in _BOX_FACES]
    poly = Poly3DCollection(rot_faces, alpha=0.82, linewidths=0.6,
                            edgecolor='#8b949e')
    poly.set_facecolor(_BOX_COLORS)
    ax3d.add_collection3d(poly)

    # 机体坐标轴（X/Y/Z 箭头，随旋转矩阵实时更新）
    AXIS_L = 1.9
    for i, (col, lbl) in enumerate(zip([CLR_X, CLR_Y, CLR_Z], ['X', 'Y', 'Z'])):
        e = R[:, i] * AXIS_L
        ax3d.quiver(0, 0, 0, e[0], e[1], e[2],
                    color=col, linewidth=2.2, arrow_length_ratio=0.18)
        ax3d.text(e[0]*1.1, e[1]*1.1, e[2]*1.1, lbl,
                  color=col, fontsize=11, fontweight='bold')

    # ── 右侧文字信息面板 ───────────────────────────────────────────────────────
    ax_txt.cla()
    ax_txt.set_facecolor(BG_PANEL)
    ax_txt.axis('off')

    def _row(y, label, value, vc=CLR_TXT, lc=CLR_DIM):
        ax_txt.text(0.05, y, label, transform=ax_txt.transAxes,
                    color=lc, fontsize=9.5, va='top')
        ax_txt.text(0.52, y, value, transform=ax_txt.transAxes,
                    color=vc, fontsize=9.5, va='top',
                    fontfamily='monospace', ha='left')

    def _section(y, title):
        ax_txt.text(0.05, y, title, transform=ax_txt.transAxes,
                    color='#8b949e', fontsize=8.5, va='top', style='italic')

    sc = '#4caf50' if connected else '#f44336'
    ax_txt.text(0.05, 0.97, status, transform=ax_txt.transAxes,
                color=sc, fontsize=9, va='top', fontweight='bold')
    ax_txt.text(0.05, 0.93, f'pkts {pkt_count:6d}   drops {drop_count}',
                transform=ax_txt.transAxes, color=CLR_DIM, fontsize=8.5, va='top')
    if not cal_done:
        ax_txt.text(0.05, 0.895,
                    f'Calibrating gyro...  {cal_count} / {CAL_SAMPLES}',
                    transform=ax_txt.transAxes,
                    color='#ffd43b', fontsize=8.5, va='top')
    elif _logger.enabled:
        log_txt = f'[REC]  {_logger.row_count} rows  →  {os.path.basename(_logger.path)}'
        ax_txt.text(0.05, 0.895, log_txt, transform=ax_txt.transAxes,
                    color='#ff6b6b', fontsize=8.5, va='top', fontweight='bold')
    if _logger.enabled:
        _btn_rec.ax.set_facecolor('#4a1010')
        _btn_rec.label.set_text('■  STOP')
        _btn_rec.label.set_color('#ff6b6b')
    else:
        _btn_rec.ax.set_facecolor('#21262d')
        _btn_rec.label.set_text('●  REC')
        _btn_rec.label.set_color('#51cf66')

    y = 0.87
    _section(y, '─── 姿态角 (Mahony+Mag) ───'); y -= 0.06
    _row(y, 'Roll',   f'{roll:+8.2f} °', CLR_X);  y -= 0.055
    _row(y, 'Pitch',  f'{pitch:+8.2f} °', CLR_Y); y -= 0.055
    _row(y, 'Yaw',    f'{yaw:+8.2f} °', CLR_Z);  y -= 0.065

    _section(y, '─── 加速度计 (m/s²) ───'); y -= 0.06
    _row(y, 'Ax', f'{accel[0]:+8.3f}', CLR_X); y -= 0.055
    _row(y, 'Ay', f'{accel[1]:+8.3f}', CLR_Y); y -= 0.055
    _row(y, 'Az', f'{accel[2]:+8.3f}', CLR_Z); y -= 0.055
    g_total = np.linalg.norm(accel)
    _row(y, '|a|', f'{g_total:8.3f}', '#e6ac00'); y -= 0.065

    _section(y, '─── 陀螺仪 (°/s，已扣零偏) ───'); y -= 0.06
    _row(y, 'Gx', f'{gyro_dps[0]:+8.2f}', CLR_X); y -= 0.055
    _row(y, 'Gy', f'{gyro_dps[1]:+8.2f}', CLR_Y); y -= 0.055
    _row(y, 'Gz', f'{gyro_dps[2]:+8.2f}', CLR_Z); y -= 0.065

    _section(y, '─── 磁力计 (gauss) ───'); y -= 0.06
    _row(y, 'Mx', f'{mag[0]:+8.4f}', CLR_X); y -= 0.055
    _row(y, 'My', f'{mag[1]:+8.4f}', CLR_Y); y -= 0.055
    _row(y, 'Mz', f'{mag[2]:+8.4f}', CLR_Z); y -= 0.055
    m_total = np.linalg.norm(mag)
    _row(y, '|m|', f'{m_total:8.4f}', '#e6ac00'); y -= 0.065

    _section(y, '─── 按键 ───'); y -= 0.06
    ax_txt.text(0.05, y, '[R] 重置滤波器   [L] / 按钮 录制   [Q] 退出',
                transform=ax_txt.transAxes, color=CLR_DIM,
                fontsize=8.5, va='top')


def _on_key(event):
    """键盘事件：[R] 重置滤波器  [L] 切换录制  [Q] 退出。"""
    if event.key in ('r', 'R'):
        with _state.lock:
            _state.filter.reset()  # 重置四元数为单位姿态，重新收敛
        print("[imu] 滤波器已重置")
    elif event.key in ('l', 'L'):
        _logger.toggle()
    elif event.key in ('q', 'Q'):
        _logger.stop()
        plt.close('all')


# ── 程序入口 ──────────────────────────────────────────────────────────────────
if __name__ == '__main__':
    host = sys.argv[1] if len(sys.argv) > 1 else input('请输入 ESP32 IP 地址: ').strip()

    # 启动后台接收线程（daemon=True 保证主线程退出时自动结束）
    t = threading.Thread(target=_recv_loop, args=(host,), daemon=True)
    t.start()

    fig.canvas.mpl_connect('key_press_event', _on_key)  # 绑定键盘事件
    _btn_rec.on_clicked(_on_btn_rec)                     # 绑定 REC 按钮
    # 每 50 ms 刷新一次画面（约 20 FPS）
    ani = animation.FuncAnimation(fig, _update, interval=50, cache_frame_data=False)

    try:
        plt.show()
    except KeyboardInterrupt:
        pass
    finally:
        _logger.stop()  # 退出时确保 CSV 文件正常关闭
