#!/usr/bin/env python3
"""Simulateur moteur pour l'adaptateur Jhoinrch RH-02 Plus.

Le RH-02 Plus (firmware SLCAN d'usine, compatible CANable) reçoit ces
commandes sur son port USB et les pose sur le bus CAN. L'afficheur décode
les trames classiques 11 bits 0x3E8, 0x3E9 et 0x3EA à 1 Mbit/s, DLC 8,
entiers 16 bits en big-endian, comme dans main/can.c.

Lancer l'interface web locale :
  python3 Simulateur/simulateur.py --ui
Puis ouvrir http://127.0.0.1:8765/
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import urlparse

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial manquant : python3 -m pip install -r Simulateur/requirements.txt")

CAN_ID_ENGINE = 0x3E8
CAN_ID_STATUS = 0x3E9
CAN_ID_LAMBDA = 0x3EA

# Index LAWICEL / firmware SLCAN CANable. L'afficheur est fixé à 1 Mbit/s.
BITRATE_INDEX = {
    10_000: "0",
    20_000: "1",
    50_000: "2",
    100_000: "3",
    125_000: "4",
    250_000: "5",
    500_000: "6",
    800_000: "7",
    1_000_000: "8",
}

GEAR_RPM_PER_KMH = (95.0, 58.0, 40.0, 30.0, 24.0)
RPM_MAX = 7500.0
RPM_SHIFT = 7300.0
ECT_MANUAL_MAX = 130.0
LAMBDA_MANUAL_MIN = 0.70
LAMBDA_MANUAL_MAX = 1.30
IAT_MANUAL_MAX = 80.0
TPS_MANUAL_MAX = 100.0
UI_HOST = "127.0.0.1"
UI_PORT = 8765


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def u8(value: float) -> int:
    return max(0, min(255, int(round(value))))


def be16(value: float) -> bytes:
    raw = max(0, min(65535, int(round(value))))
    return bytes((raw >> 8, raw & 0xFF))


class Engine:
    """Cycle contact, démarrage, chauffe, accélérations et frein moteur."""

    def __init__(self) -> None:
        self.t = 0.0
        self.rpm = 0.0
        self.speed = 0.0
        self.gear = 1
        self.ect = 22.0
        self.oil_temp = 20.0
        self.lambda1 = 1.0
        self.iat = 24.0
        self.tps = 0.0
        self.running = False
        self.rpm_mode = "auto"
        self.ect_mode = "auto"
        self.lambda_mode = "auto"
        self.iat_mode = "auto"
        self.tps_mode = "auto"
        self.manual_rpm = 0.0
        self.manual_ect = 22.0
        self.manual_lambda = 1.0
        self.manual_iat = 24.0
        self.manual_tps = 0.0
        self._lock = threading.Lock()

    def set_rpm_mode(self, mode: str, value: float | None = None) -> None:
        with self._lock:
            if mode not in ("auto", "manual", "accel"):
                raise ValueError("mode régime invalide")
            if mode == "manual":
                if value is None:
                    value = self.rpm
                self.manual_rpm = clamp(float(value), 0.0, RPM_MAX)
                self.rpm = self.manual_rpm
            self.rpm_mode = mode

    def set_ect_mode(self, mode: str, value: float | None = None) -> None:
        with self._lock:
            if mode not in ("auto", "manual"):
                raise ValueError("mode eau invalide")
            if mode == "manual":
                if value is None:
                    value = self.ect
                self.manual_ect = clamp(float(value), 0.0, ECT_MANUAL_MAX)
                self.ect = self.manual_ect
            self.ect_mode = mode

    def set_lambda_mode(self, mode: str, value: float | None = None) -> None:
        with self._lock:
            if mode not in ("auto", "manual"):
                raise ValueError("mode lambda invalide")
            if mode == "manual":
                if value is None:
                    value = self.lambda1
                self.manual_lambda = clamp(float(value), LAMBDA_MANUAL_MIN, LAMBDA_MANUAL_MAX)
                self.lambda1 = self.manual_lambda
            self.lambda_mode = mode

    def set_iat_mode(self, mode: str, value: float | None = None) -> None:
        with self._lock:
            if mode not in ("auto", "manual"):
                raise ValueError("mode air moteur invalide")
            if mode == "manual":
                if value is None:
                    value = self.iat
                self.manual_iat = clamp(float(value), 0.0, IAT_MANUAL_MAX)
                self.iat = self.manual_iat
            self.iat_mode = mode

    def set_tps_mode(self, mode: str, value: float | None = None) -> None:
        with self._lock:
            if mode not in ("auto", "manual"):
                raise ValueError("mode papillon invalide")
            if mode == "manual":
                if value is None:
                    value = self.tps
                self.manual_tps = clamp(float(value), 0.0, TPS_MANUAL_MAX)
                self.tps = self.manual_tps
            self.tps_mode = mode

    def set_manual_rpm(self, value: float) -> None:
        with self._lock:
            self.manual_rpm = clamp(float(value), 0.0, RPM_MAX)
            if self.rpm_mode == "manual":
                self.rpm = self.manual_rpm

    def set_manual_ect(self, value: float) -> None:
        with self._lock:
            self.manual_ect = clamp(float(value), 0.0, ECT_MANUAL_MAX)
            if self.ect_mode == "manual":
                self.ect = self.manual_ect

    def set_manual_lambda(self, value: float) -> None:
        with self._lock:
            self.manual_lambda = clamp(float(value), LAMBDA_MANUAL_MIN, LAMBDA_MANUAL_MAX)
            if self.lambda_mode == "manual":
                self.lambda1 = self.manual_lambda

    def set_manual_iat(self, value: float) -> None:
        with self._lock:
            self.manual_iat = clamp(float(value), 0.0, IAT_MANUAL_MAX)
            if self.iat_mode == "manual":
                self.iat = self.manual_iat

    def set_manual_tps(self, value: float) -> None:
        with self._lock:
            self.manual_tps = clamp(float(value), 0.0, TPS_MANUAL_MAX)
            if self.tps_mode == "manual":
                self.tps = self.manual_tps

    def snapshot_controls(self) -> dict[str, Any]:
        with self._lock:
            return {
                "rpm_mode": self.rpm_mode,
                "ect_mode": self.ect_mode,
                "lambda_mode": self.lambda_mode,
                "iat_mode": self.iat_mode,
                "tps_mode": self.tps_mode,
                "manual_rpm": round(self.manual_rpm),
                "manual_ect": round(self.manual_ect, 1),
                "manual_lambda": round(self.manual_lambda, 2),
                "manual_iat": round(self.manual_iat, 1),
                "manual_tps": round(self.manual_tps, 1),
                "rpm": round(self.rpm),
                "ect_c": round(self.ect, 1),
                "lambda1": round(self.lambda1, 2),
                "iat_c": round(self.iat, 1),
                "tps_pct": round(self.tps, 1),
            }

    def step(self, dt: float) -> dict[str, float]:
        with self._lock:
            self.t += dt
            if self.rpm_mode == "accel":
                pedal, running, cranking = self._pedal_accel()
            else:
                pedal, running, cranking = self._pedal(self.t % 40.0)
            self.running = running
            rpm_manual = self.rpm_mode == "manual"
            ect_manual = self.ect_mode == "manual"
            lambda_manual = self.lambda_mode == "manual"
            iat_manual = self.iat_mode == "manual"
            tps_manual = self.tps_mode == "manual"

            if rpm_manual:
                self.rpm = self.manual_rpm
                if self.rpm > 400.0:
                    self.running = True
            else:
                target_rpm = 0.0
                if cranking:
                    target_rpm = 260.0
                elif running:
                    target_rpm = 820.0 + pedal * (RPM_MAX - 820.0)
                # Accélérations : montée plus vive pour atteindre RPM_MAX pendant le palier.
                if self.rpm_mode == "accel":
                    tau = 0.22 if target_rpm >= self.rpm else 0.55
                else:
                    tau = 0.45 if target_rpm >= self.rpm else 0.8
                self.rpm += (target_rpm - self.rpm) * min(1.0, dt / tau)
                if self.rpm_mode == "accel" and pedal >= 0.99 and abs(target_rpm - self.rpm) < 40.0:
                    self.rpm = target_rpm
                if running and pedal < 0.04:
                    self.rpm += 12.0 * math.sin(self.t * 18.0)
                self.rpm = clamp(self.rpm, 0.0, RPM_MAX)
                # Pas de passage de rapport en Accélérations : sinon coupe à RPM_SHIFT (~7300).
                if self.rpm_mode != "accel":
                    self._shift(pedal)

            self._speed(dt, pedal)
            if ect_manual:
                self.ect = self.manual_ect
                oil_target = self.ect - 3.0
                self.oil_temp += (oil_target - self.oil_temp) * min(1.0, dt / 70.0)
            else:
                self._warm(dt, pedal)

            if tps_manual:
                self.tps = self.manual_tps
            else:
                self.tps = pedal * 100.0

            volts = 12.5
            if cranking and not rpm_manual:
                volts = 10.6
            elif self.running or self.rpm > 400.0:
                volts = 14.2 - pedal * 0.3

            manifold = 101.3
            if self.rpm > 400.0:
                manifold = 28.0 + pedal * 73.0 + max(0.0, self.rpm - 800.0) * 0.0015

            ignition = 0.0
            if self.rpm > 400.0:
                ignition = 8.0 + (self.rpm / RPM_MAX) * 28.0 - pedal * 14.0

            if lambda_manual:
                self.lambda1 = self.manual_lambda
            elif self.rpm < 400.0:
                self.lambda1 = 1.0
            elif pedal > 0.65:
                self.lambda1 = 0.86
            elif pedal < 0.05 and self.rpm > 1800.0:
                self.lambda1 = 1.06
            else:
                self.lambda1 = 0.995

            if iat_manual:
                self.iat = self.manual_iat
            else:
                self.iat = 24.0 + pedal * 16.0 + max(0.0, self.ect - 70.0) * 0.2

            oil_bar_10 = 0.0
            fuel_bar_10 = 0.0
            if self.rpm > 400.0:
                oil_bar_10 = 10.0 + self.rpm / 160.0
                fuel_bar_10 = 32.0 + pedal * 6.0

            return {
                "rpm": self.rpm,
                "manifold_kpa": clamp(manifold, 20.0, 115.0),
                "ect_c": self.ect,
                "iat_c": self.iat,
                "ecu_volts": volts,
                "oil_temp_c": self.oil_temp,
                "tps_pct": self.tps,
                "ignition_deg": clamp(ignition, -10.0, 45.0),
                "speed": clamp(self.speed, 0.0, 240.0),
                "oil_pressure": clamp(oil_bar_10, 0.0, 255.0),
                "fuel_pressure": clamp(fuel_bar_10, 0.0, 255.0),
                "ecu_temp_c": 30.0 + max(0.0, self.ect - 22.0) * 0.4,
                "lambda1": self.lambda1,
                "lambda2": self.lambda1 + 0.004,
                "steering_deg": 25.0 * math.sin(self.t * 0.35),
                "atmosphere_kpa": 101.3,
            }

    def _pedal(self, cycle: float) -> tuple[float, bool, bool]:
        if cycle < 2.0:
            return 0.0, False, False
        if cycle < 3.4:
            return 0.18, False, True
        points = (
            (3.4, 0.0),
            (8.0, 0.0),
            (11.0, 1.0),
            (24.0, 1.0),
            (28.0, 0.15),
            (36.0, 0.0),
            (40.0, 0.0),
        )
        pedal = points[-1][1]
        for (t0, p0), (t1, p1) in zip(points, points[1:]):
            if t0 <= cycle <= t1:
                span = t1 - t0
                mix = 0.0 if span == 0.0 else (cycle - t0) / span
                pedal = p0 + (p1 - p0) * mix
                break
        return clamp(pedal, 0.0, 1.0), True, False

    def _pedal_accel(self) -> tuple[float, bool, bool]:
        """Série d'accélérations 0→100 %→palier→0, en boucle (moteur déjà lancé).

        Le palier à fond doit être assez long pour que le régime atteigne RPM_MAX
        et que l'afficheur (lissage ~40 ms) montre le rideau à fond.
        """
        press = 0.9
        hold = 2.2
        release = 1.0
        rest = 0.6
        period = press + hold + release + rest
        phase = self.t % period
        if phase < press:
            pedal = phase / press
        elif phase < press + hold:
            pedal = 1.0
        elif phase < press + hold + release:
            pedal = 1.0 - (phase - press - hold) / release
        else:
            pedal = 0.0
        return clamp(pedal, 0.0, 1.0), True, False

    def _shift(self, pedal: float) -> None:
        if not self.running:
            self.gear = 1
            return
        if self.rpm >= RPM_SHIFT and pedal > 0.4 and self.gear < len(GEAR_RPM_PER_KMH):
            self.gear += 1
            self.rpm *= 0.66
        elif self.rpm < 1400.0 and pedal < 0.15 and self.gear > 1:
            self.gear -= 1
            self.rpm = min(RPM_MAX, self.rpm * 1.4)
        self.rpm = clamp(self.rpm, 0.0, RPM_MAX)

    def _speed(self, dt: float, pedal: float) -> None:
        if self.rpm < 500.0:
            self.speed += (0.0 - self.speed) * min(1.0, dt / 1.2)
            return
        target = self.rpm / GEAR_RPM_PER_KMH[self.gear - 1]
        if pedal < 0.08:
            target *= 0.92
        self.speed += (target - self.speed) * min(1.0, dt / 0.5)

    def _warm(self, dt: float, pedal: float) -> None:
        if not self.running:
            ect_target = 22.0
            ect_tau = 60.0
        elif self.rpm >= 6000.0 and pedal > 0.8:
            ect_target = 118.0
            ect_tau = 6.0
        else:
            ect_target = 93.0
            ect_tau = 25.0
        self.ect += (ect_target - self.ect) * min(1.0, dt / ect_tau)
        self.ect += pedal * dt * 0.15
        self.ect = clamp(self.ect, 18.0, 120.0)
        oil_target = self.ect - 3.0
        self.oil_temp += (oil_target - self.oil_temp) * min(1.0, dt / 70.0)


def encode(sample: dict[str, float]) -> dict[int, bytes]:
    engine = (
        be16(sample["rpm"])
        + be16(sample["manifold_kpa"] + 100.0)
        + bytes((
            u8(sample["ect_c"] + 50.0),
            u8(sample["iat_c"] + 50.0),
            u8(sample["ecu_volts"] * 10.0),
            u8(sample["oil_temp_c"] + 50.0),
        ))
    )
    status = (
        be16(sample["tps_pct"] * 10.0)
        + be16((sample["ignition_deg"] + 100.0) * 10.0)
        + bytes((
            u8(sample["speed"]),
            u8(sample["oil_pressure"]),
            u8(sample["fuel_pressure"]),
            u8(sample["ecu_temp_c"] + 50.0),
        ))
    )
    lambda_frame = (
        be16(sample["lambda1"] * 1000.0)
        + be16(sample["lambda2"] * 1000.0)
        + be16(sample["steering_deg"] * 10.0 + 30000.0)
        + be16(sample["atmosphere_kpa"] * 10.0)
    )
    return {
        CAN_ID_ENGINE: engine,
        CAN_ID_STATUS: status,
        CAN_ID_LAMBDA: lambda_frame,
    }


class Rh02:
    def __init__(self, port: str, bitrate: int) -> None:
        if bitrate not in BITRATE_INDEX:
            choices = ", ".join(str(item) for item in BITRATE_INDEX)
            raise SystemExit(f"Débit non supporté par le SLCAN du RH-02 : {bitrate}. Choix : {choices}")
        self.bitrate = bitrate
        self.ser = serial.Serial(port, baudrate=115200, timeout=0.05, write_timeout=1)
        self.port = port

    def open_bus(self) -> str:
        self._command("C", check=False)
        version = self._command("V", check=False).strip()
        self._command("S" + BITRATE_INDEX[self.bitrate], check=True)
        self._command("M0", check=False)
        self._command("A1", check=False)
        self._command("O", check=True)
        return version

    def send(self, can_id: int, data: bytes) -> None:
        if len(data) != 8:
            raise ValueError("l'afficheur n'accepte que des trames de 8 octets")
        line = f"t{can_id:03X}8{data.hex().upper()}\r".encode("ascii")
        self.ser.write(line)
        self._raise_on_bell(self._drain())

    def close(self) -> None:
        try:
            self.ser.write(b"C\r")
            self.ser.flush()
        finally:
            self.ser.close()

    def _command(self, cmd: str, check: bool) -> str:
        self.ser.reset_input_buffer()
        self.ser.write(cmd.encode("ascii") + b"\r")
        self.ser.flush()
        deadline = time.monotonic() + 0.25
        chunks = bytearray()
        while time.monotonic() < deadline:
            waiting = self.ser.in_waiting
            if waiting:
                chunks += self.ser.read(waiting)
                if b"\r" in chunks or b"\x07" in chunks:
                    break
            else:
                time.sleep(0.01)
        text = bytes(chunks)
        if check:
            self._raise_on_bell(text)
        return text.decode("ascii", errors="replace")

    def _drain(self) -> bytes:
        waiting = self.ser.in_waiting
        if waiting <= 0:
            return b""
        return self.ser.read(waiting)

    @staticmethod
    def _raise_on_bell(payload: bytes) -> None:
        if b"\x07" in payload:
            raise RuntimeError("le RH-02 a refusé la commande SLCAN")


def candidate_ports() -> list[str]:
    """Ports SLCAN du RH-02. Le port WCH 1A86 est la console de l'afficheur."""
    found: list[str] = []
    for port in list_ports.comports():
        if port.vid == 0x1A86:
            continue
        text = f"{port.device} {port.description or ''} {port.manufacturer or ''}".upper()
        canable = (port.vid, port.pid) in {(0x16D0, 0x117E), (0x0483, 0x5740)}
        named = any(token in text for token in ("CANABLE", "SLCAN", "STM32", "RH-02", "RH02"))
        if canable or named:
            found.append(port.device)
    return found


def resolve_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    found = candidate_ports()
    if len(found) == 1:
        return found[0]
    if not found:
        raise SystemExit(
            "Aucun RH-02 Plus trouvé. Branche-le en USB. "
            "Le firmware d'usine doit être le SLCAN CANable (port série), pas candleLight."
        )
    joined = "\n".join(f"  {item}" for item in found)
    raise SystemExit(f"Plusieurs ports possibles. Relance avec --port :\n{joined}")


def format_status(sample: dict[str, float]) -> str:
    return (
        f"rpm {sample['rpm']:4.0f}  "
        f"collecteur {sample['manifold_kpa']:5.1f} kPa  "
        f"eau {sample['ect_c']:4.1f} °C  "
        f"air {sample['iat_c']:4.1f} °C  "
        f"papillon {sample['tps_pct']:5.1f} %  "
        f"avance {sample['ignition_deg']:5.1f} °  "
        f"vitesse {sample['speed']:5.1f}  "
        f"lambda {sample['lambda1']:.3f}  "
        f"{sample['ecu_volts']:.1f} V"
    )


UI_HTML = """<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Simulateur Dash — RH-02</title>
<style>
  :root {
    --bg: #0f1419;
    --panel: #1a2332;
    --text: #e8eef6;
    --muted: #8b9bb0;
    --accent: #3d9cf0;
    --ok: #3ecf8e;
    --warn: #f0a03d;
    --bad: #e85d5d;
    --track: #2a3648;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    font-family: "SF Pro Text", "Segoe UI", system-ui, sans-serif;
    background: radial-gradient(1200px 600px at 10% -10%, #1e3a5f 0%, var(--bg) 55%);
    color: var(--text);
    min-height: 100vh;
  }
  main {
    max-width: 720px;
    margin: 0 auto;
    padding: 2rem 1.25rem 3rem;
  }
  h1 {
    font-size: 1.45rem;
    font-weight: 650;
    margin: 0 0 0.35rem;
  }
  .sub { color: var(--muted); margin-bottom: 1.5rem; font-size: 0.95rem; }
  .conn {
    display: flex;
    align-items: center;
    gap: 0.65rem;
    padding: 0.85rem 1rem;
    border-radius: 10px;
    background: var(--panel);
    margin-bottom: 1.25rem;
    border: 1px solid #2c3a4f;
  }
  .dot {
    width: 10px; height: 10px; border-radius: 50%;
    background: var(--muted);
    flex-shrink: 0;
  }
  .dot.ok { background: var(--ok); box-shadow: 0 0 8px #3ecf8e88; }
  .dot.bad { background: var(--bad); box-shadow: 0 0 8px #e85d5d88; }
  .dot.warn { background: var(--warn); }
  .card {
    background: var(--panel);
    border: 1px solid #2c3a4f;
    border-radius: 12px;
    padding: 1.15rem 1.2rem 1.35rem;
    margin-bottom: 1rem;
  }
  .card h2 {
    margin: 0 0 0.85rem;
    font-size: 1.05rem;
    font-weight: 600;
  }
  .modes {
    display: flex;
    gap: 0.5rem;
    margin-bottom: 1rem;
  }
  .modes button {
    flex: 1;
    border: 1px solid #33465f;
    background: #121a26;
    color: var(--muted);
    padding: 0.55rem 0.75rem;
    border-radius: 8px;
    cursor: pointer;
    font-size: 0.92rem;
  }
  .modes button.active {
    background: #1e3a5f;
    border-color: var(--accent);
    color: var(--text);
  }
  .modes button:disabled { opacity: 0.45; cursor: default; }
  label.slider-label {
    display: flex;
    justify-content: space-between;
    color: var(--muted);
    font-size: 0.88rem;
    margin-bottom: 0.4rem;
  }
  input[type=range] {
    width: 100%;
    accent-color: var(--accent);
    margin: 0.2rem 0 0.35rem;
  }
  .sent {
    font-variant-numeric: tabular-nums;
    font-size: 1.55rem;
    font-weight: 650;
    letter-spacing: 0.02em;
  }
  .sent span { color: var(--muted); font-size: 0.85rem; font-weight: 500; }
  .hint { color: var(--muted); font-size: 0.82rem; margin-top: 0.55rem; }
  .live {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
    gap: 0.75rem;
    margin-top: 0.25rem;
  }
  .live .card { margin: 0; text-align: center; }
  .live .sent { font-size: 1.25rem; }
  .err {
    color: #ffc9c9;
    background: #3a1c1c;
    border: 1px solid #6a3030;
    border-radius: 8px;
    padding: 0.7rem 0.9rem;
    margin-bottom: 1rem;
    font-size: 0.9rem;
    display: none;
  }
  .err.show { display: block; }
</style>
</head>
<body>
<main>
  <h1>Simulateur moteur</h1>
  <p class="sub">Régime, eau, lambda, air moteur et papillon — modes auto / accélérations / manuel indépendants</p>
  <div id="err" class="err"></div>
  <div class="conn">
    <div id="dot" class="dot"></div>
    <div>
      <div id="conn-text">Connexion RH-02…</div>
      <div id="conn-detail" class="hint" style="margin:0.2rem 0 0"></div>
    </div>
  </div>
  <div class="live">
    <div class="card">
      <div class="hint" style="margin:0">Envoyé</div>
      <div class="sent" id="live-rpm">— <span>tr/min</span></div>
    </div>
    <div class="card">
      <div class="hint" style="margin:0">Envoyé</div>
      <div class="sent" id="live-ect">— <span>°C</span></div>
    </div>
    <div class="card">
      <div class="hint" style="margin:0">Envoyé</div>
      <div class="sent" id="live-lambda">— <span>λ</span></div>
    </div>
    <div class="card">
      <div class="hint" style="margin:0">Envoyé</div>
      <div class="sent" id="live-iat">— <span>°C</span></div>
    </div>
    <div class="card">
      <div class="hint" style="margin:0">Envoyé</div>
      <div class="sent" id="live-tps">— <span>%</span></div>
    </div>
  </div>
  <div class="card" style="margin-top:1rem">
    <h2>Régime moteur</h2>
    <div class="modes">
      <button type="button" id="rpm-auto" class="active">Automatique</button>
      <button type="button" id="rpm-accel">Accélérations</button>
      <button type="button" id="rpm-manual">Manuel</button>
    </div>
    <label class="slider-label"><span>Curseur manuel</span><span id="rpm-label">0 tr/min</span></label>
    <input type="range" id="rpm-slider" min="0" max="7500" step="50" value="0" disabled>
    <p class="hint">Auto : scénario 40 s. Accélérations : pédale 0→100 %→0 en boucle. Manuel : 0–7500 tr/min fixe, sans rapports.</p>
  </div>
  <div class="card">
    <h2>Water</h2>
    <div class="modes">
      <button type="button" id="ect-auto" class="active">Automatique</button>
      <button type="button" id="ect-manual">Manuel</button>
    </div>
    <label class="slider-label"><span>Curseur manuel</span><span id="ect-label">22.0 °C</span></label>
    <input type="range" id="ect-slider" min="0" max="130" step="0.5" value="22" disabled>
    <p class="hint">En manuel : 0–130 °C (au-delà de 110 °C → alerte STOP ENGINE).</p>
  </div>
  <div class="card">
    <h2>Lambda</h2>
    <div class="modes">
      <button type="button" id="lambda-auto" class="active">Automatique</button>
      <button type="button" id="lambda-manual">Manuel</button>
    </div>
    <label class="slider-label"><span>Curseur manuel</span><span id="lambda-label">1.00</span></label>
    <input type="range" id="lambda-slider" min="0.70" max="1.30" step="0.01" value="1.00" disabled>
    <p class="hint">En manuel : 0,70–1,30 (afficheur : &lt; 0,95 rouge, &gt; 1,2 bleu).</p>
  </div>
  <div class="card">
    <h2>Air Temp</h2>
    <div class="modes">
      <button type="button" id="iat-auto" class="active">Automatique</button>
      <button type="button" id="iat-manual">Manuel</button>
    </div>
    <label class="slider-label"><span>Curseur manuel</span><span id="iat-label">24.0 °C</span></label>
    <input type="range" id="iat-slider" min="0" max="80" step="0.5" value="24" disabled>
    <p class="hint">En manuel : température d’admission fixe 0–80 °C.</p>
  </div>
  <div class="card">
    <h2>Papillon (TPS)</h2>
    <div class="modes">
      <button type="button" id="tps-auto" class="active">Automatique</button>
      <button type="button" id="tps-manual">Manuel</button>
    </div>
    <label class="slider-label"><span>Curseur manuel</span><span id="tps-label">0.0 %</span></label>
    <input type="range" id="tps-slider" min="0" max="100" step="0.5" value="0" disabled>
    <p class="hint">En manuel : 0–100 % (indépendant de la pédale du cycle auto).</p>
  </div>
</main>
<script>
const rpmAuto = document.getElementById("rpm-auto");
const rpmAccel = document.getElementById("rpm-accel");
const rpmManual = document.getElementById("rpm-manual");
const rpmSlider = document.getElementById("rpm-slider");
const rpmLabel = document.getElementById("rpm-label");
const ectAuto = document.getElementById("ect-auto");
const ectManual = document.getElementById("ect-manual");
const ectSlider = document.getElementById("ect-slider");
const ectLabel = document.getElementById("ect-label");
const lambdaAuto = document.getElementById("lambda-auto");
const lambdaManual = document.getElementById("lambda-manual");
const lambdaSlider = document.getElementById("lambda-slider");
const lambdaLabel = document.getElementById("lambda-label");
const iatAuto = document.getElementById("iat-auto");
const iatManual = document.getElementById("iat-manual");
const iatSlider = document.getElementById("iat-slider");
const iatLabel = document.getElementById("iat-label");
const tpsAuto = document.getElementById("tps-auto");
const tpsManual = document.getElementById("tps-manual");
const tpsSlider = document.getElementById("tps-slider");
const tpsLabel = document.getElementById("tps-label");
const liveRpm = document.getElementById("live-rpm");
const liveEct = document.getElementById("live-ect");
const liveLambda = document.getElementById("live-lambda");
const liveIat = document.getElementById("live-iat");
const liveTps = document.getElementById("live-tps");
const connText = document.getElementById("conn-text");
const connDetail = document.getElementById("conn-detail");
const dot = document.getElementById("dot");
const errBox = document.getElementById("err");

let controlChain = Promise.resolve();
let editingRpm = false;
let editingEct = false;
let editingLambda = false;
let editingIat = false;
let editingTps = false;

function showErr(msg) {
  if (!msg) { errBox.classList.remove("show"); errBox.textContent = ""; return; }
  errBox.textContent = msg;
  errBox.classList.add("show");
}

function syncUi(s) {
  const rpmMan = s.rpm_mode === "manual";
  const rpmAcc = s.rpm_mode === "accel";
  const ectMan = s.ect_mode === "manual";
  const lambdaMan = s.lambda_mode === "manual";
  const iatMan = s.iat_mode === "manual";
  const tpsMan = s.tps_mode === "manual";
  rpmAuto.classList.toggle("active", s.rpm_mode === "auto");
  rpmAccel.classList.toggle("active", rpmAcc);
  rpmManual.classList.toggle("active", rpmMan);
  ectAuto.classList.toggle("active", !ectMan);
  ectManual.classList.toggle("active", ectMan);
  lambdaAuto.classList.toggle("active", !lambdaMan);
  lambdaManual.classList.toggle("active", lambdaMan);
  iatAuto.classList.toggle("active", !iatMan);
  iatManual.classList.toggle("active", iatMan);
  tpsAuto.classList.toggle("active", !tpsMan);
  tpsManual.classList.toggle("active", tpsMan);
  rpmSlider.disabled = !rpmMan;
  ectSlider.disabled = !ectMan;
  lambdaSlider.disabled = !lambdaMan;
  iatSlider.disabled = !iatMan;
  tpsSlider.disabled = !tpsMan;
  if (rpmMan && !editingRpm) {
    rpmSlider.value = String(s.manual_rpm);
    rpmLabel.textContent = Math.round(s.manual_rpm) + " tr/min";
  }
  if (ectMan && !editingEct) {
    ectSlider.value = String(s.manual_ect);
    ectLabel.textContent = Number(s.manual_ect).toFixed(1) + " °C";
  }
  if (lambdaMan && !editingLambda) {
    lambdaSlider.value = String(s.manual_lambda);
    lambdaLabel.textContent = Number(s.manual_lambda).toFixed(2);
  }
  if (iatMan && !editingIat) {
    iatSlider.value = String(s.manual_iat);
    iatLabel.textContent = Number(s.manual_iat).toFixed(1) + " °C";
  }
  if (tpsMan && !editingTps) {
    tpsSlider.value = String(s.manual_tps);
    tpsLabel.textContent = Number(s.manual_tps).toFixed(1) + " %";
  }
  liveRpm.innerHTML = Math.round(s.rpm) + ' <span>tr/min</span>';
  liveEct.innerHTML = Number(s.ect_c).toFixed(1) + ' <span>°C</span>';
  liveLambda.innerHTML = Number(s.lambda1).toFixed(2) + ' <span>λ</span>';
  liveIat.innerHTML = Number(s.iat_c).toFixed(1) + ' <span>°C</span>';
  liveTps.innerHTML = Number(s.tps_pct).toFixed(1) + ' <span>%</span>';

  if (s.connected) {
    dot.className = "dot ok";
    connText.textContent = "RH-02 connecté — bus CAN ouvert";
    connDetail.textContent = (s.port || "") + (s.firmware ? " · " + s.firmware : "");
    showErr("");
  } else {
    dot.className = "dot bad";
    connText.textContent = "RH-02 non connecté";
    connDetail.textContent = s.connection_error || "Branchez l’adaptateur USB et relancez, ou attendez une reconnexion.";
    showErr(s.connection_error || "Aucun RH-02 détecté. L’interface tourne, mais aucune trame CAN n’est envoyée.");
  }
}

function postControl(body) {
  controlChain = controlChain.then(async () => {
    try {
      const r = await fetch("/api/control", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const s = await r.json();
      syncUi(s);
    } catch (e) {
      showErr("Impossible de joindre le simulateur : " + e);
    }
  });
  return controlChain;
}

async function poll() {
  try {
    const r = await fetch("/api/status");
    const s = await r.json();
    syncUi(s);
  } catch (e) {
    dot.className = "dot warn";
    connText.textContent = "Serveur local injoignable";
    showErr(String(e));
  }
}

rpmAuto.onclick = () => postControl({ rpm_mode: "auto" });
rpmAccel.onclick = () => postControl({ rpm_mode: "accel" });
rpmManual.onclick = () => postControl({ rpm_mode: "manual" });
ectAuto.onclick = () => postControl({ ect_mode: "auto" });
ectManual.onclick = () => postControl({ ect_mode: "manual" });
lambdaAuto.onclick = () => postControl({ lambda_mode: "auto" });
lambdaManual.onclick = () => postControl({ lambda_mode: "manual" });
iatAuto.onclick = () => postControl({ iat_mode: "auto" });
iatManual.onclick = () => postControl({ iat_mode: "manual" });
tpsAuto.onclick = () => postControl({ tps_mode: "auto" });
tpsManual.onclick = () => postControl({ tps_mode: "manual" });

rpmSlider.onpointerdown = () => { editingRpm = true; };
rpmSlider.onpointerup = () => { editingRpm = false; };
rpmSlider.oninput = () => {
  editingRpm = true;
  rpmLabel.textContent = rpmSlider.value + " tr/min";
};
rpmSlider.onchange = () => {
  editingRpm = false;
  postControl({ manual_rpm: Number(rpmSlider.value) });
};
ectSlider.onpointerdown = () => { editingEct = true; };
ectSlider.onpointerup = () => { editingEct = false; };
ectSlider.oninput = () => {
  editingEct = true;
  ectLabel.textContent = Number(ectSlider.value).toFixed(1) + " °C";
};
ectSlider.onchange = () => {
  editingEct = false;
  postControl({ manual_ect: Number(ectSlider.value) });
};
lambdaSlider.onpointerdown = () => { editingLambda = true; };
lambdaSlider.onpointerup = () => { editingLambda = false; };
lambdaSlider.oninput = () => {
  editingLambda = true;
  lambdaLabel.textContent = Number(lambdaSlider.value).toFixed(2);
};
lambdaSlider.onchange = () => {
  editingLambda = false;
  postControl({ manual_lambda: Number(lambdaSlider.value) });
};
iatSlider.onpointerdown = () => { editingIat = true; };
iatSlider.onpointerup = () => { editingIat = false; };
iatSlider.oninput = () => {
  editingIat = true;
  iatLabel.textContent = Number(iatSlider.value).toFixed(1) + " °C";
};
iatSlider.onchange = () => {
  editingIat = false;
  postControl({ manual_iat: Number(iatSlider.value) });
};
tpsSlider.onpointerdown = () => { editingTps = true; };
tpsSlider.onpointerup = () => { editingTps = false; };
tpsSlider.oninput = () => {
  editingTps = true;
  tpsLabel.textContent = Number(tpsSlider.value).toFixed(1) + " %";
};
tpsSlider.onchange = () => {
  editingTps = false;
  postControl({ manual_tps: Number(tpsSlider.value) });
};

poll();
setInterval(poll, 400);
</script>
</body>
</html>
"""


class SimulatorState:
    """État partagé entre la boucle CAN et le serveur HTTP."""

    def __init__(self, engine: Engine, period: float) -> None:
        self.engine = engine
        self.period = period
        self.lock = threading.Lock()
        self.sample: dict[str, float] = {
            "rpm": 0.0,
            "ect_c": 22.0,
            "lambda1": 1.0,
            "iat_c": 24.0,
            "tps_pct": 0.0,
        }
        self.connected = False
        self.port: str | None = None
        self.firmware = ""
        self.connection_error = "Connexion en cours…"
        self.stop = threading.Event()
        self.bus: Rh02 | None = None

    def status_dict(self) -> dict[str, Any]:
        controls = self.engine.snapshot_controls()
        with self.lock:
            sample = dict(self.sample)
            connected = self.connected
            port = self.port
            firmware = self.firmware
            connection_error = self.connection_error
        return {
            **controls,
            "rpm": round(sample.get("rpm", controls["rpm"])),
            "ect_c": round(float(sample.get("ect_c", controls["ect_c"])), 1),
            "lambda1": round(float(sample.get("lambda1", controls["lambda1"])), 2),
            "iat_c": round(float(sample.get("iat_c", controls["iat_c"])), 1),
            "tps_pct": round(float(sample.get("tps_pct", controls["tps_pct"])), 1),
            "connected": connected,
            "port": port,
            "firmware": firmware,
            "connection_error": connection_error,
        }

    def apply_control(self, payload: dict[str, Any]) -> dict[str, Any]:
        if "rpm_mode" in payload:
            mode = str(payload["rpm_mode"])
            value = payload.get("manual_rpm")
            self.engine.set_rpm_mode(mode, None if value is None else float(value))
        if "ect_mode" in payload:
            mode = str(payload["ect_mode"])
            value = payload.get("manual_ect")
            self.engine.set_ect_mode(mode, None if value is None else float(value))
        if "lambda_mode" in payload:
            mode = str(payload["lambda_mode"])
            value = payload.get("manual_lambda")
            self.engine.set_lambda_mode(mode, None if value is None else float(value))
        if "iat_mode" in payload:
            mode = str(payload["iat_mode"])
            value = payload.get("manual_iat")
            self.engine.set_iat_mode(mode, None if value is None else float(value))
        if "tps_mode" in payload:
            mode = str(payload["tps_mode"])
            value = payload.get("manual_tps")
            self.engine.set_tps_mode(mode, None if value is None else float(value))
        if "manual_rpm" in payload and "rpm_mode" not in payload:
            self.engine.set_manual_rpm(float(payload["manual_rpm"]))
        if "manual_ect" in payload and "ect_mode" not in payload:
            self.engine.set_manual_ect(float(payload["manual_ect"]))
        if "manual_lambda" in payload and "lambda_mode" not in payload:
            self.engine.set_manual_lambda(float(payload["manual_lambda"]))
        if "manual_iat" in payload and "iat_mode" not in payload:
            self.engine.set_manual_iat(float(payload["manual_iat"]))
        if "manual_tps" in payload and "tps_mode" not in payload:
            self.engine.set_manual_tps(float(payload["manual_tps"]))
        return self.status_dict()


def make_handler(state: SimulatorState) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format: str, *args: Any) -> None:
            return

        def _json(self, code: int, payload: dict[str, Any]) -> None:
            body = json.dumps(payload).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _html(self, code: int, text: str) -> None:
            body = text.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:
            path = urlparse(self.path).path
            if path in ("/", "/index.html"):
                self._html(200, UI_HTML)
                return
            if path == "/api/status":
                self._json(200, state.status_dict())
                return
            self._json(404, {"error": "introuvable"})

        def do_POST(self) -> None:
            path = urlparse(self.path).path
            if path != "/api/control":
                self._json(404, {"error": "introuvable"})
                return
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length) if length else b"{}"
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                if not isinstance(payload, dict):
                    raise ValueError("JSON objet attendu")
                self._json(200, state.apply_control(payload))
            except Exception as exc:
                self._json(400, {"error": str(exc)})

    return Handler


def try_open_bus(state: SimulatorState, port: str | None, bitrate: int) -> None:
    try:
        resolved = resolve_port(port)
    except SystemExit as exc:
        with state.lock:
            state.connected = False
            state.bus = None
            state.port = None
            state.firmware = ""
            state.connection_error = str(exc)
        return
    try:
        bus = Rh02(resolved, bitrate)
        version = bus.open_bus()
    except Exception as exc:
        with state.lock:
            state.connected = False
            state.bus = None
            state.port = resolved
            state.firmware = ""
            state.connection_error = f"Impossible d’ouvrir le RH-02 ({resolved}) : {exc}"
        return
    with state.lock:
        state.bus = bus
        state.connected = True
        state.port = resolved
        state.firmware = version.strip() if version else ""
        state.connection_error = ""


def close_bus(state: SimulatorState) -> None:
    with state.lock:
        bus = state.bus
        state.bus = None
        state.connected = False
    if bus is not None:
        try:
            bus.close()
        except Exception:
            pass


def simulation_loop(state: SimulatorState, port: str | None, bitrate: int) -> None:
    previous = time.monotonic()
    next_tick = previous
    last_log = 0.0
    last_retry = 0.0
    try_open_bus(state, port, bitrate)

    while not state.stop.is_set():
        now = time.monotonic()
        if now < next_tick:
            state.stop.wait(next_tick - now)
            now = time.monotonic()
            if state.stop.is_set():
                break

        if not state.connected and now - last_retry >= 2.0:
            try_open_bus(state, port, bitrate)
            last_retry = now

        dt = min(0.2, now - previous)
        previous = now
        sample = state.engine.step(dt)
        with state.lock:
            state.sample = sample
            bus = state.bus
            connected = state.connected

        if connected and bus is not None:
            try:
                for can_id, data in encode(sample).items():
                    bus.send(can_id, data)
            except Exception as exc:
                with state.lock:
                    state.connected = False
                    state.connection_error = f"Perte de liaison RH-02 : {exc}"
                    state.bus = None
                try:
                    bus.close()
                except Exception:
                    pass

        if now - last_log >= 1.0:
            controls = state.engine.snapshot_controls()
            link = "CAN OK" if state.connected else "CAN OFF"
            print(
                f"[{link}] {format_status(sample)}  "
                f"régime={controls['rpm_mode']} eau={controls['ect_mode']} "
                f"λ={controls['lambda_mode']} air={controls['iat_mode']} "
                f"tps={controls['tps_mode']}",
                flush=True,
            )
            last_log = now

        next_tick += state.period
        if next_tick < now:
            next_tick = now

    close_bus(state)


def run_ui(port: str | None, bitrate: int, period: float, bind_host: str, bind_port: int) -> None:
    engine = Engine()
    state = SimulatorState(engine, period)
    worker = threading.Thread(
        target=simulation_loop,
        args=(state, port, bitrate),
        name="can-sim",
        daemon=True,
    )
    worker.start()
    handler = make_handler(state)
    server = ThreadingHTTPServer((bind_host, bind_port), handler)
    print(f"Interface : http://{bind_host}:{bind_port}/")
    print("Ctrl+C pour arrêter (ferme le bus CAN avec la commande SLCAN C).")
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        print("\nArrêt de l’interface.")
    finally:
        state.stop.set()
        server.shutdown()
        worker.join(timeout=2.0)
        close_bus(state)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Envoie un moteur simulé du RH-02 Plus vers l'afficheur, sur le CAN 1 Mbit/s."
    )
    parser.add_argument("--port", help="Port série du RH-02, par exemple /dev/cu.usbmodem1101")
    parser.add_argument("--list", action="store_true", help="Affiche les ports détectés et quitte")
    parser.add_argument("--bitrate", type=int, default=1_000_000, help="Débit CAN, 1000000 pour l'afficheur")
    parser.add_argument("--period", type=float, default=0.02, help="Période d'envoi des 3 trames, en secondes")
    parser.add_argument(
        "--ui",
        action="store_true",
        help="Ouvre l'interface web locale (régime / eau / lambda / air / papillon auto-manuel)",
    )
    parser.add_argument("--ui-host", default=UI_HOST, help=f"Adresse HTTP (défaut {UI_HOST})")
    parser.add_argument("--ui-port", type=int, default=UI_PORT, help=f"Port HTTP (défaut {UI_PORT})")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.list:
        found = candidate_ports()
        if not found:
            print("Aucun port RH-02 détecté.")
        else:
            print("\n".join(found))
        return
    if args.period <= 0:
        raise SystemExit("--period doit être positif")

    if args.ui:
        run_ui(args.port, args.bitrate, args.period, args.ui_host, args.ui_port)
        return

    port = resolve_port(args.port)
    bus = Rh02(port, args.bitrate)
    engine = Engine()
    version = bus.open_bus()
    print(f"RH-02 Plus sur {port}")
    if version:
        print(f"Firmware : {version}")
    print("CAN classique 1 Mbit/s, identifiants 0x3E8, 0x3E9, 0x3EA.")
    print("CANH et CANL vers l'afficheur, terminaison 120 Ω à chaque extrémité du bus.")
    print("Ctrl+C pour arrêter.")

    next_tick = time.monotonic()
    last_log = 0.0
    previous = time.monotonic()
    try:
        while True:
            now = time.monotonic()
            if now < next_tick:
                time.sleep(next_tick - now)
                now = time.monotonic()
            dt = min(0.2, now - previous)
            previous = now
            sample = engine.step(dt)
            for can_id, data in encode(sample).items():
                bus.send(can_id, data)
            if now - last_log >= 1.0:
                print(format_status(sample), flush=True)
                last_log = now
            next_tick += args.period
            if next_tick < now:
                next_tick = now
    except KeyboardInterrupt:
        print("\nArrêt du simulateur.")
    finally:
        bus.close()


if __name__ == "__main__":
    main()
