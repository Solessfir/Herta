"""Synthesizes the Sandbox's sounds into Content/Audio as 16-bit WAV files.

Every sound is generated from noise, sines, and filters with fixed seeds, so the files are original, public domain (CC0), and the same on
every run. Run it with any Python 3 from this folder: python GenerateSounds.py
"""

import math
import random
import struct
import wave
from pathlib import Path

RATE = 48000
OUTPUT = Path(__file__).resolve().parent.parent / "Content" / "Audio"


def write(name, channels, frames):
    """Writes mono samples, or stereo pairs, after normalizing the peak to -1 dBFS."""
    flat = [value for frame in frames for value in frame] if channels == 2 else frames
    peak = max(1e-9, max(abs(value) for value in flat))
    scale = 0.891 / peak * 32767
    data = struct.pack("<%dh" % len(flat), *(max(-32768, min(32767, int(round(value * scale)))) for value in flat))
    with wave.open(str(OUTPUT / name), "wb") as file:
        file.setnchannels(channels)
        file.setsampwidth(2)
        file.setframerate(RATE)
        file.writeframes(data)


class StateVariable:
    """A topology-preserving state-variable filter; returns low, band, and high outputs."""

    def __init__(self, resonance=0.7):
        self.k = 1.0 / resonance
        self.ic1 = 0.0
        self.ic2 = 0.0

    def run(self, value, cutoff):
        g = math.tan(math.pi * min(cutoff, RATE * 0.45) / RATE)
        a1 = 1.0 / (1.0 + g * (g + self.k))
        a2 = g * a1
        a3 = g * a2
        v3 = value - self.ic2
        v1 = a1 * self.ic1 + a2 * v3
        v2 = self.ic2 + a2 * self.ic1 + a3 * v3
        self.ic1 = 2.0 * v1 - self.ic1
        self.ic2 = 2.0 * v2 - self.ic2
        return v2, v1, value - self.k * v1 - v2


def seconds(duration):
    return int(duration * RATE)


def footstep(seed):
    rng = random.Random(seed)
    count = seconds(0.16)
    body = StateVariable(0.9)
    grit = StateVariable(0.6)
    pitch = rng.uniform(70.0, 95.0)
    out = []
    for index in range(count):
        t = index / RATE
        envelope = math.exp(-t * 38.0) * min(1.0, t * 900.0)
        noise = rng.uniform(-1.0, 1.0)
        low, _, _ = body.run(noise, 420.0)
        _, band, _ = grit.run(noise, rng.uniform(2200.0, 3400.0))
        thump = math.sin(2.0 * math.pi * pitch * t * (1.0 - t * 1.5)) * math.exp(-t * 55.0)
        out.append((low * 1.8 + band * 0.35 + thump * 0.9) * envelope)
    return out


def land():
    rng = random.Random(11)
    count = seconds(0.45)
    body = StateVariable(0.8)
    out = []
    for index in range(count):
        t = index / RATE
        envelope = math.exp(-t * 14.0) * min(1.0, t * 1200.0)
        low, _, _ = body.run(rng.uniform(-1.0, 1.0), 300.0 + 900.0 * math.exp(-t * 20.0))
        thump = math.sin(2.0 * math.pi * 58.0 * t * (1.0 - t * 0.8)) * math.exp(-t * 18.0)
        out.append((low * 1.6 + thump * 1.2) * envelope)
    return out


def whoosh(seed, duration, low_cutoff, high_cutoff, resonance):
    rng = random.Random(seed)
    count = seconds(duration)
    sweep = StateVariable(resonance)
    out = []
    for index in range(count):
        phase = index / count
        envelope = math.sin(math.pi * phase) ** 2
        cutoff = low_cutoff + (high_cutoff - low_cutoff) * math.sin(math.pi * phase)
        _, band, _ = sweep.run(rng.uniform(-1.0, 1.0), cutoff)
        out.append(band * envelope)
    return out


def impact(seed):
    rng = random.Random(seed)
    count = seconds(0.6)
    body = StateVariable(0.7)
    modes = [(rng.uniform(110.0, 170.0), 1.0, 16.0), (rng.uniform(230.0, 320.0), 0.55, 24.0), (rng.uniform(480.0, 700.0), 0.3, 40.0)]
    out = []
    for index in range(count):
        t = index / RATE
        noise = rng.uniform(-1.0, 1.0)
        low, _, _ = body.run(noise, 1800.0 * math.exp(-t * 30.0) + 250.0)
        click = noise * math.exp(-t * 400.0) * 0.6
        knock = sum(amplitude * math.sin(2.0 * math.pi * frequency * t) * math.exp(-t * decay) for frequency, amplitude, decay in modes)
        out.append((low * math.exp(-t * 22.0) * 1.4 + knock * 0.8 + click) * min(1.0, t * 3000.0))
    return out


def slice_sound():
    rng = random.Random(21)
    count = seconds(0.5)
    air = StateVariable(1.2)
    partials = [(2350.0, 1.0, 9.0), (3610.0, 0.6, 12.0), (5020.0, 0.4, 16.0), (6840.0, 0.25, 22.0)]
    out = []
    for index in range(count):
        t = index / RATE
        _, band, _ = air.run(rng.uniform(-1.0, 1.0), 1500.0 + 5000.0 * math.exp(-t * 9.0))
        ring = sum(amplitude * math.sin(2.0 * math.pi * frequency * (1.0 + 0.002 * math.sin(30.0 * t)) * t) * math.exp(-t * decay) for frequency, amplitude, decay in partials)
        out.append((band * math.exp(-t * 14.0) * 0.8 + ring * 0.45) * min(1.0, t * 2000.0))
    return out


def sever():
    rng = random.Random(31)
    count = seconds(0.55)
    wet = StateVariable(2.5)
    body = StateVariable(0.8)
    out = []
    for index in range(count):
        t = index / RATE
        noise = rng.uniform(-1.0, 1.0)
        _, squelch, _ = wet.run(noise, 700.0 + 500.0 * math.sin(2.0 * math.pi * 9.0 * t))
        low, _, _ = body.run(noise, 260.0)
        thump = math.sin(2.0 * math.pi * 72.0 * t) * math.exp(-t * 20.0)
        out.append((squelch * math.exp(-t * 7.0) * 0.6 + low * math.exp(-t * 16.0) * 1.4 + thump) * min(1.0, t * 2500.0))
    return out


def clash(seed, duration, partials, brightness, crack):
    """Steel on steel: inharmonic partials ringing out of a sharp crack, as two blades meeting."""
    rng = random.Random(seed)
    count = seconds(duration)
    air = StateVariable(1.4)
    out = []
    for index in range(count):
        t = index / RATE
        noise = rng.uniform(-1.0, 1.0)
        _, _, high = air.run(noise, brightness)
        snap = high * math.exp(-t * crack)
        ring = sum(amplitude * math.sin(2.0 * math.pi * frequency * t + phase) * math.exp(-t * decay) for frequency, amplitude, decay, phase in partials)
        out.append((snap * 0.9 + ring * 0.6) * min(1.0, t * 6000.0))
    return out


def chime(notes, spacing, duration):
    count = seconds(duration)
    out = [0.0] * count
    for number, frequency in enumerate(notes):
        start = seconds(spacing * number)
        for index in range(start, count):
            t = (index - start) / RATE
            envelope = math.exp(-t * 3.2) * min(1.0, t * 400.0)
            out[index] += envelope * (math.sin(2.0 * math.pi * frequency * t) + 0.35 * math.sin(4.0 * math.pi * frequency * t) + 0.12 * math.sin(6.0 * math.pi * frequency * t))
    return out


def loop_crossfade(samples, fade):
    """Makes a seamless loop by fading the extra tail into the head."""
    body = samples[:-fade]
    for index in range(fade):
        mix = index / fade
        tail = samples[len(samples) - fade + index]
        body[index] = body[index] * mix + tail * (1.0 - mix) if not isinstance(tail, tuple) else tuple(head * mix + end * (1.0 - mix) for head, end in zip(body[index], tail))
    return body


def slide_loop():
    rng = random.Random(41)
    fade = seconds(0.25)
    count = seconds(2.0) + fade
    scrape = StateVariable(0.8)
    rumble = StateVariable(0.7)
    out = []
    for index in range(count):
        t = index / RATE
        noise = rng.uniform(-1.0, 1.0)
        _, band, _ = scrape.run(noise, 1700.0 + 500.0 * math.sin(2.0 * math.pi * 3.0 * t))
        low, _, _ = rumble.run(noise, 180.0)
        out.append(band * 0.5 + low * 1.5)
    return loop_crossfade(out, fade)


def hum_loop():
    rng = random.Random(51)
    fade = seconds(0.2)
    count = seconds(2.0) + fade
    hiss = StateVariable(0.7)
    out = []
    for index in range(count):
        t = index / RATE
        tone = sum(amplitude * math.sin(2.0 * math.pi * 60.0 * harmonic * t) for harmonic, amplitude in ((1, 1.0), (2, 0.5), (3, 0.3), (5, 0.12)))
        whine = 0.08 * math.sin(2.0 * math.pi * 1240.0 * t) * (0.6 + 0.4 * math.sin(2.0 * math.pi * 0.5 * t))
        low, _, _ = hiss.run(rng.uniform(-1.0, 1.0), 900.0)
        out.append(tone * (0.85 + 0.15 * math.sin(2.0 * math.pi * 4.0 * t)) + whine + low * 0.25)
    return loop_crossfade(out, fade)


def wind_loop():
    """Thirty seconds of stereo wind: two decorrelated noise beds whose gusts drift slowly, long enough to stream."""
    rng = random.Random(61)
    fade = seconds(2.0)
    count = seconds(30.0) + fade
    filters = [(StateVariable(1.4), StateVariable(0.6)), (StateVariable(1.4), StateVariable(0.6))]
    out = []
    for index in range(count):
        t = index / RATE
        frame = []
        for channel, (whistle, bed) in enumerate(filters):
            gust = 0.55 + 0.25 * math.sin(2.0 * math.pi * (0.071 + channel * 0.013) * t) + 0.2 * math.sin(2.0 * math.pi * 0.023 * t + channel)
            noise = rng.uniform(-1.0, 1.0)
            _, band, _ = whistle.run(noise, 380.0 + 420.0 * gust)
            low, _, _ = bed.run(noise, 140.0 + 160.0 * gust)
            frame.append((band * 0.5 + low * 1.3) * gust)
        out.append(tuple(frame))
    return loop_crossfade(out, fade)


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for number in range(4):
        write("Footstep_%d.wav" % (number + 1), 1, footstep(100 + number))
        write("Impact_%d.wav" % (number + 1), 1, impact(200 + number))
    write("Land.wav", 1, land())
    write("Jump.wav", 1, whoosh(301, 0.22, 600.0, 2400.0, 0.9))
    write("Swing_1.wav", 1, whoosh(401, 0.32, 500.0, 3800.0, 2.2))
    write("Swing_2.wav", 1, whoosh(402, 0.28, 650.0, 4400.0, 2.6))
    write("Slice.wav", 1, slice_sound())
    write("Sever.wav", 1, sever())
    # A deflect rings bright and long; a block is a shorter, duller clank.
    write("Deflect.wav", 1, clash(501, 1.1, [(1873.0, 0.7, 5.5, 0.0), (2951.0, 1.0, 4.0, 1.1), (4387.0, 0.8, 6.0, 2.3), (6122.0, 0.5, 8.0, 0.4), (8513.0, 0.3, 11.0, 1.7)], 5200.0, 70.0))
    write("Block.wav", 1, clash(502, 0.45, [(612.0, 0.8, 18.0, 0.0), (1387.0, 1.0, 14.0, 0.9), (2244.0, 0.6, 20.0, 1.9), (3371.0, 0.3, 28.0, 0.5)], 2600.0, 110.0))
    write("Run_Start.wav", 1, chime([880.0, 1318.5], 0.09, 0.7))
    write("Run_Finish.wav", 1, chime([659.25, 830.61, 987.77, 1318.5], 0.11, 1.6))
    write("Slide_Loop.wav", 1, slide_loop())
    write("Machine_Hum_Loop.wav", 1, hum_loop())
    write("Wind_Loop.wav", 2, wind_loop())


if __name__ == "__main__":
    main()
