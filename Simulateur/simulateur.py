#!/usr/bin/env python3
"""Simulateur moteur pour l'adaptateur Jhoinrch RH-02 Plus.

Le RH-02 Plus (firmware SLCAN d'usine, compatible CANable) reçoit ces
commandes sur son port USB et les pose sur le bus CAN. L'afficheur décode
les trames classiques 11 bits 0x3E8, 0x3E9 et 0x3EA à 1 Mbit/s, DLC 8,
entiers 16 bits en big-endian, comme dans main/can.c.
"""

from __future__ import annotations

import argparse
import math
import sys
import time

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
        self.running = False

    def step(self, dt: float) -> dict[str, float]:
        self.t += dt
        cycle = self.t % 40.0
        pedal, running, cranking = self._pedal(cycle)
        self.running = running

        target_rpm = 0.0
        if cranking:
            target_rpm = 260.0
        elif running:
            target_rpm = 820.0 + pedal * 5600.0
        tau = 0.45 if target_rpm >= self.rpm else 0.8
        self.rpm += (target_rpm - self.rpm) * min(1.0, dt / tau)
        if running and pedal < 0.04:
            self.rpm += 12.0 * math.sin(self.t * 18.0)

        self._shift(pedal)
        self._speed(dt, pedal)
        self._warm(dt, pedal)

        volts = 12.5
        if cranking:
            volts = 10.6
        elif running:
            volts = 14.2 - pedal * 0.3

        manifold = 101.3
        if self.rpm > 400.0:
            manifold = 28.0 + pedal * 73.0 + max(0.0, self.rpm - 800.0) * 0.0015

        ignition = 0.0
        if self.rpm > 400.0:
            ignition = 8.0 + (self.rpm / 6500.0) * 28.0 - pedal * 14.0

        if self.rpm < 400.0:
            lambda1 = 1.0
        elif pedal > 0.65:
            lambda1 = 0.86
        elif pedal < 0.05 and self.rpm > 1800.0:
            lambda1 = 1.06
        else:
            lambda1 = 0.995

        oil_bar_10 = 0.0
        fuel_bar_10 = 0.0
        if self.rpm > 400.0:
            oil_bar_10 = 10.0 + self.rpm / 160.0
            fuel_bar_10 = 32.0 + pedal * 6.0

        return {
            "rpm": clamp(self.rpm, 0.0, 9000.0),
            "manifold_kpa": clamp(manifold, 20.0, 115.0),
            "ect_c": self.ect,
            "iat_c": 24.0 + pedal * 16.0 + max(0.0, self.ect - 70.0) * 0.2,
            "ecu_volts": volts,
            "oil_temp_c": self.oil_temp,
            "tps_pct": pedal * 100.0,
            "ignition_deg": clamp(ignition, -10.0, 45.0),
            "speed": clamp(self.speed, 0.0, 240.0),
            "oil_pressure": clamp(oil_bar_10, 0.0, 255.0),
            "fuel_pressure": clamp(fuel_bar_10, 0.0, 255.0),
            "ecu_temp_c": 30.0 + max(0.0, self.ect - 22.0) * 0.4,
            "lambda1": lambda1,
            "lambda2": lambda1 + 0.004,
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
            (13.0, 0.9),
            (17.0, 0.72),
            (19.0, 0.12),
            (24.0, 0.62),
            (30.0, 0.28),
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

    def _shift(self, pedal: float) -> None:
        if not self.running:
            self.gear = 1
            return
        if self.rpm > 5400.0 and pedal > 0.4 and self.gear < len(GEAR_RPM_PER_KMH):
            self.gear += 1
            self.rpm *= 0.66
        elif self.rpm < 1400.0 and pedal < 0.15 and self.gear > 1:
            self.gear -= 1
            self.rpm = min(6500.0, self.rpm * 1.4)

    def _speed(self, dt: float, pedal: float) -> None:
        if self.rpm < 500.0:
            self.speed += (0.0 - self.speed) * min(1.0, dt / 1.2)
            return
        target = self.rpm / GEAR_RPM_PER_KMH[self.gear - 1]
        if pedal < 0.08:
            target *= 0.92
        self.speed += (target - self.speed) * min(1.0, dt / 0.5)

    def _warm(self, dt: float, pedal: float) -> None:
        ect_target = 93.0 if self.running else 22.0
        ect_tau = 25.0 if self.running else 60.0
        self.ect += (ect_target - self.ect) * min(1.0, dt / ect_tau)
        self.ect += pedal * dt * 0.15
        self.ect = clamp(self.ect, 18.0, 110.0)
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Envoie un moteur simulé du RH-02 Plus vers l'afficheur, sur le CAN 1 Mbit/s."
    )
    parser.add_argument("--port", help="Port série du RH-02, par exemple /dev/cu.usbmodem1101")
    parser.add_argument("--list", action="store_true", help="Affiche les ports détectés et quitte")
    parser.add_argument("--bitrate", type=int, default=1_000_000, help="Débit CAN, 1000000 pour l'afficheur")
    parser.add_argument("--period", type=float, default=0.02, help="Période d'envoi des 3 trames, en secondes")
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
