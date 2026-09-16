#!/usr/bin/env python3

import glob
import json
import os
import time

CONFIG_PATH = os.environ.get("CPU_CLOCK_SWITCH_CONFIG", "/etc/cpu-clock-switch.json")
DEFAULT_CONFIG = {
    "low_threshold_pct": 6.0,
    "high_threshold_pct": 10.0,
    "low_frequency_khz": 1_200_000,
    "high_frequency_khz": 3_500_000,
    "interval_ms": 500,
}


def load_config():
    config = DEFAULT_CONFIG.copy()
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as config_file:
            loaded = json.load(config_file)
        if isinstance(loaded, dict):
            config.update(loaded)
    except FileNotFoundError:
        pass
    except (OSError, ValueError) as error:
        print(f"cpu-clock-switch: configuração inválida ({error}); usando padrões", flush=True)

    try:
        config["low_threshold_pct"] = min(99.0, max(0.0, float(config["low_threshold_pct"])) )
        config["high_threshold_pct"] = min(100.0, max(1.0, float(config["high_threshold_pct"])) )
        config["low_frequency_khz"] = max(0, int(config["low_frequency_khz"]))
        config["high_frequency_khz"] = max(0, int(config["high_frequency_khz"]))
        config["interval_ms"] = min(5_000, max(100, int(config["interval_ms"])) )
    except (KeyError, TypeError, ValueError):
        print("cpu-clock-switch: campos inválidos; usando padrões", flush=True)
        config = DEFAULT_CONFIG.copy()

    if config["low_threshold_pct"] >= config["high_threshold_pct"]:
        config["low_threshold_pct"] = DEFAULT_CONFIG["low_threshold_pct"]
        config["high_threshold_pct"] = DEFAULT_CONFIG["high_threshold_pct"]
    if config["low_frequency_khz"] > config["high_frequency_khz"]:
        config["low_frequency_khz"], config["high_frequency_khz"] = (
            config["high_frequency_khz"],
            config["low_frequency_khz"],
        )
    return config


def cpu_times():
    with open("/proc/stat", encoding="utf-8") as stat_file:
        values = list(map(int, stat_file.readline().split()[1:9]))
    idle = values[3] + values[4]
    total = sum(values)
    return idle, total


def write(path, value):
    try:
        with open(path, "w", encoding="ascii") as target:
            target.write(str(value))
    except (OSError, PermissionError):
        pass


def set_frequency(target, low_frequency):
    for policy in policies:
        try:
            with open(f"{policy}/cpuinfo_min_freq", encoding="ascii") as minimum_file:
                hardware_min = int(minimum_file.read())
            with open(f"{policy}/cpuinfo_max_freq", encoding="ascii") as maximum_file:
                hardware_max = int(maximum_file.read())

            frequency = max(hardware_min, min(target, hardware_max))
            # A ordem evita EINVAL quando min/max estão sendo igualados.
            if target <= low_frequency:
                write(f"{policy}/scaling_min_freq", frequency)
                write(f"{policy}/scaling_max_freq", frequency)
            else:
                write(f"{policy}/scaling_max_freq", frequency)
                write(f"{policy}/scaling_min_freq", frequency)
        except OSError:
            pass


config = load_config()
LOW_THRESHOLD = config["low_threshold_pct"]
HIGH_THRESHOLD = config["high_threshold_pct"]
LOW_FREQ = config["low_frequency_khz"]
HIGH_FREQ = config["high_frequency_khz"]
INTERVAL = config["interval_ms"] / 1000.0
policies = glob.glob("/sys/devices/system/cpu/cpufreq/policy*")

idle_old, total_old = cpu_times()
state = None

while True:
    time.sleep(INTERVAL)
    idle_new, total_new = cpu_times()
    idle_delta = idle_new - idle_old
    total_delta = total_new - total_old
    idle_old, total_old = idle_new, total_new

    if total_delta <= 0:
        continue

    usage = 100.0 * (1.0 - idle_delta / total_delta)
    if usage < LOW_THRESHOLD and state != "low":
        set_frequency(LOW_FREQ, LOW_FREQ)
        state = "low"
    elif usage > HIGH_THRESHOLD and state != "high":
        set_frequency(HIGH_FREQ, LOW_FREQ)
        state = "high"
