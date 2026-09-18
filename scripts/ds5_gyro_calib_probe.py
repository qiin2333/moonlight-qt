#!/usr/bin/env python3
"""Read a DualSense's factory calibration (HID feature report 0x05) and replay
SDL's accept/reject decision on it.

Motivation: SDL is the only layer in our client's motion path that subtracts the
gyro zero-rate bias. It rejects the whole device's hardware calibration when any
axis fails a sanity check, and the uncalibrated fallback (value * 64.f) removes
no bias at all -- a large yaw bias then reaches the game and integrates into
constant camera rotation. This script reports the raw calibration values, the
verdict SDL would reach, and the resting gyro output under both paths.

Usage:  python scripts/ds5_gyro_calib_probe.py
Requires: pip install hidapi
"""

import sys
import time

try:
    import hid
except ImportError:
    sys.exit("Missing dependency. Install with: pip install hidapi")

SONY_VID = 0x054C
DS5_PIDS = {
    0x0CE6: "DualSense",
    0x0DF2: "DualSense Edge",
}

FEATURE_REPORT_CALIBRATION = 0x05

# From SDL_hidapi_ps5.c
GYRO_RES_PER_DEGREE = 1024.0
SDL_BIAS_LIMIT = 1024
SDL_GYRO_DIVISOR = 64.0
SDL_SENSITIVITY_TOLERANCE = 0.5

AXES = ("pitch", "yaw", "roll")


def s16(lo, hi):
    v = lo | (hi << 8)
    return v - 0x10000 if v & 0x8000 else v


def find_dualsense():
    for info in hid.enumerate(SONY_VID, 0):
        if info["product_id"] in DS5_PIDS:
            return info
    return None


def read_calibration(dev):
    # hidapi prepends the report ID; SDL requires >= 35 bytes of payload.
    # hidapi 在缓冲区头部保留报告 ID;SDL 的字段偏移是含 ID 的,不要剥掉
    data = dev.get_feature_report(FEATURE_REPORT_CALIBRATION, 64)
    if not data:
        return None, "feature report 0x05 returned no data"
    if data[0] != FEATURE_REPORT_CALIBRATION:
        return None, f"unexpected report ID 0x{data[0]:02x} (want 0x05)"
    if len(data) < 35:
        return None, f"only {len(data)} bytes (SDL requires >= 35, would reject)"
    return data, None


def parse(data):
    """Mirror HIDAPI_DriverPS5_LoadCalibrationData for the gyro axes."""
    bias = [s16(data[1], data[2]), s16(data[3], data[4]), s16(data[5], data[6])]
    plus = [s16(data[7], data[8]), s16(data[11], data[12]), s16(data[15], data[16])]
    minus = [s16(data[9], data[10]), s16(data[13], data[14]), s16(data[17], data[18])]
    speed_plus = s16(data[19], data[20])
    speed_minus = s16(data[21], data[22])

    numerator = (speed_plus + speed_minus) * GYRO_RES_PER_DEGREE
    sensitivity = []
    for i in range(3):
        span = plus[i] - minus[i]
        sensitivity.append(numerator / span if span else float("inf"))
    return bias, sensitivity, (speed_plus, speed_minus)


def sdl_verdict(bias, sensitivity):
    """SDL rejects the ENTIRE device if any single axis fails."""
    failures = []
    for i in range(3):
        if abs(bias[i]) > SDL_BIAS_LIMIT:
            failures.append(
                f"{AXES[i]}: |bias| {abs(bias[i])} > {SDL_BIAS_LIMIT}")
        dev = abs(1.0 - sensitivity[i] / SDL_GYRO_DIVISOR)
        if dev > SDL_SENSITIVITY_TOLERANCE:
            failures.append(
                f"{AXES[i]}: sensitivity {sensitivity[i]:.2f} deviates "
                f"{dev * 100:.0f}% from nominal {SDL_GYRO_DIVISOR:.0f}")
    return failures


def sample_rest(dev, seconds=3.0):
    """Average the raw gyro fields while the controller sits still.

    USB input report 0x01: gyro int16 LE at bytes 15/17/19 (payload, after the
    report ID byte). Bluetooth enhanced report 0x31: gyro lands at buffer
    indices 17/19/21 (payload starts after ID + tag + sequence).
    """
    dev.set_nonblocking(True)
    totals = [0, 0, 0]
    count = 0
    deadline = time.time() + seconds
    while time.time() < deadline:
        buf = dev.read(64)
        if not buf or len(buf) < 22:
            time.sleep(0.001)
            continue
        # 缓冲区含报告 ID:USB 0x01 的陀螺在 buf[16/18/20],
        # 蓝牙增强报文 0x31 载荷从 buf[2] 起,陀螺在 buf[17/19/21]
        if buf[0] == 0x01:
            base = 16
        elif buf[0] == 0x31:
            base = 17
        else:
            continue
        for i in range(3):
            totals[i] += s16(buf[base + i * 2], buf[base + 1 + i * 2])
        count += 1
    if not count:
        return None, 0
    return [t / count for t in totals], count


def main():
    info = find_dualsense()
    if not info:
        sys.exit("No DualSense found. Connect it over USB and retry.\n"
                 "(Bluetooth works too, but USB gives the cleanest report layout.)")

    name = DS5_PIDS.get(info["product_id"], "unknown")
    print(f"Found {name} (VID {info['vendor_id']:04X} PID {info['product_id']:04X})")
    print(f"  path: {info['path'].decode(errors='replace')}\n")

    dev = hid.Device(path=info["path"])
    try:
        data, err = read_calibration(dev)
        if err:
            print(f"Calibration read FAILED: {err}")
            print("=> SDL would use the uncalibrated fallback (value * 64.f), "
                  "which subtracts NO bias.")
            return

        bias, sensitivity, speeds = parse(data)
        print("Factory calibration (feature report 0x05):")
        print(f"  gyro speed plus/minus: {speeds[0]} / {speeds[1]}")
        for i in range(3):
            dps = bias[i] * sensitivity[i] / GYRO_RES_PER_DEGREE
            print(f"  {AXES[i]:>5}: bias {bias[i]:>7}  ({dps:>8.3f} deg/s)   "
                  f"sensitivity {sensitivity[i]:.3f}")

        failures = sdl_verdict(bias, sensitivity)
        print()
        print("NOTE: gyro-only precheck -- SDL's accept/reject also covers the")
        print("      accelerometer calibration pairs, which are NOT evaluated here.")
        if failures:
            print("SDL VERDICT (gyro axes): REJECTED -- hardware_calibration = FALSE")
            print("             whole device (the check has no break statement).")
            for f in failures:
                print(f"  - {f}")
            print("\n=> Gyro takes the fallback path: result = value * 64.f")
            print("   No bias is subtracted. Any resting offset reaches the game.")
        else:
            print("SDL VERDICT (gyro axes): ACCEPTED -- bias is subtracted as")
            print("             result = (value - bias) * sensitivity")
            print("\n=> If the camera still spins, this gyro rejection path is ruled out (accel checks not evaluated here).")

        print("\nHold the controller still on a flat surface...")
        time.sleep(0.7)
        rest, count = sample_rest(dev)
        if not rest:
            print("  No input reports captured (is another process grabbing the "
                  "device exclusively?). Skipping the rest measurement.")
            return

        print(f"  Averaged {count} samples at rest:")
        for i in range(3):
            raw = rest[i]
            uncal_dps = raw * SDL_GYRO_DIVISOR / GYRO_RES_PER_DEGREE
            cal_dps = (raw - bias[i]) * sensitivity[i] / GYRO_RES_PER_DEGREE
            print(f"  {AXES[i]:>5}: raw {raw:>9.1f}   "
                  f"uncalibrated {uncal_dps:>8.2f} deg/s   "
                  f"calibrated {cal_dps:>8.2f} deg/s")

        print("\nInterpretation: the 'uncalibrated' column is what a game receives")
        print("when SDL rejects the calibration. A steady non-zero value there is")
        print("integrated into continuous camera rotation on that axis.")
    finally:
        dev.close()


if __name__ == "__main__":
    main()
