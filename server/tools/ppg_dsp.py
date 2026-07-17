import math

class BandPass:
    def __init__(self, hpR=0.90, lpBeta=0.55, jumThresh=8000.0):
        self._hpR = hpR
        self._lpBeta = lpBeta
        self._jump = jumThresh
        self._x1a = 0.0
        self._y1a = 0.0
        self._x1b = 0.0
        self._y1b = 0.0
        self._lp = 0.0
        self._out = 0.0
        self._baseline = 0.0
        self._seeded = False

    def seed(self, x0: float):
        self._x1a = x0
        self._y1a = 0.0
        self._x1b = 0.0
        self._y1b = 0.0
        self._lp = 0.0
        self._baseline = x0
        self._out = 0.0
        self._seeded = True

    def reset(self):
        self._x1a = self._y1a = self._x1b = self._y1b = self._lp = self._out = self._baseline = 0.0
        self._seeded = False

    def process(self, x: float) -> float:
        if not self._seeded:
            self.seed(x)

        self._baseline = 0.97 * self._baseline + 0.03 * x
        if self._jump > 0.0 and abs(x - self._baseline) > self._jump:
            self.seed(x)
            return 0.0

        ya = self._hpR * (self._y1a + x - self._x1a)
        self._x1a = x
        self._y1a = ya

        yb = self._hpR * (self._y1b + ya - self._x1b)
        self._x1b = ya
        self._y1b = yb

        self._lp = self._lpBeta * self._lp + (1.0 - self._lpBeta) * yb
        self._out = self._lp
        return self._out

    def value(self) -> float:
        return self._out


class PeakEnvelope:
    def __init__(self, decay=0.05, floorV=5.0, attack=0.40):
        self._decay = decay
        self._floor = floorV
        self._attack = attack
        self._env = floorV

    def reset(self):
        self._env = self._floor

    def process(self, x: float, frozen: bool = False) -> float:
        if not frozen:
            if x > self._env:
                self._env += (x - self._env) * self._attack
            else:
                self._env -= (self._env - x) * self._decay
        if self._env < self._floor:
            self._env = self._floor
        return self._env

    def value(self) -> float:
        return self._env


class BeatDetector:
    def __init__(self, thresholdRatio=0.60, refractoryMs=400, maxIntervalMs=2000, rearmRatio=0.6):
        self._ratio = thresholdRatio
        self._refractory = refractoryMs
        self._maxInterval = maxIntervalMs
        self._rearmRatio = rearmRatio

        self._prev = 0.0
        self._lastBeatMs = 0
        self._hasLast = False
        self._rising = False
        self._peak = 0.0

    def reset(self):
        self._prev = 0.0
        self._lastBeatMs = 0
        self._hasLast = False
        self._rising = False
        self._peak = 0.0

    def process(self, x: float, envelope: float, nowMs: int, blocked: bool) -> int:
        threshold = envelope * self._ratio
        interval = 0

        if blocked:
            self._rising = False
            self._prev = x
            return 0

        if x > threshold:
            if x > self._prev:
                self._rising = True
                self._peak = x
            elif self._rising and x < self._prev:
                self._rising = False
                if self._hasLast:
                    delta = nowMs - self._lastBeatMs
                    if self._refractory <= delta <= self._maxInterval:
                        interval = delta
                        self._lastBeatMs = nowMs
                    elif delta > self._maxInterval:
                        self._lastBeatMs = nowMs
                else:
                    self._lastBeatMs = nowMs
                    self._hasLast = True
        else:
            self._rising = False

        self._prev = x
        return interval

    def syncTime(self, nowMs: int):
        self._lastBeatMs = nowMs
        self._hasLast = True
        self._rising = False


class MotionGate:
    def __init__(self, factor=4.0, holdMs=400, graceMs=1500, minEnv=8.0, absMin=60.0):
        self._factor = factor
        self._hold = holdMs
        self._grace = graceMs
        self._minEnv = minEnv
        self._absMin = absMin

        self._motionUntil = 0
        self._startMs = 0
        self._started = False

    def reset(self):
        self._motionUntil = 0
        self._startMs = 0
        self._started = False

    def process(self, absSignal: float, envelope: float, nowMs: int) -> bool:
        if not self._started:
            self._started = True
            self._startMs = nowMs

        inGrace = (nowMs - self._startMs) < self._grace

        if not inGrace and envelope >= self._minEnv:
            if absSignal > envelope * self._factor and absSignal > self._absMin:
                self._motionUntil = nowMs + self._hold

        return (nowMs < self._motionUntil)


class BpmEstimator:
    WINDOW = 5

    def __init__(self):
        self._hist = [0] * self.WINDOW
        self._fill = 0
        self._spot = 0
        self._bpm = 0
        self._rejectStreak = 0

    def reset(self):
        self._fill = 0
        self._spot = 0
        self._bpm = 0
        self._rejectStreak = 0

    def softReset(self):
        self._fill = 0
        self._spot = 0
        self._rejectStreak = 0

    def pushInterval(self, intervalMs: int) -> int:
        instant = 60000.0 / intervalMs
        plausible = True

        if self._bpm > 0:
            ratio = instant / self._bpm
            if ratio < 0.4 or ratio > 1.7:
                plausible = False

        if not plausible:
            self._rejectStreak += 1
            if self._rejectStreak >= 5:
                plausible = True
                self._fill = 0
                self._spot = 0
                self._bpm = 0
                self._rejectStreak = 0
            else:
                return self._bpm
        else:
            self._rejectStreak = 0

        self._hist[self._spot] = int(instant)
        self._spot = (self._spot + 1) % self.WINDOW
        if self._fill < self.WINDOW:
            self._fill += 1

        tmp = self._hist[:self._fill]
        tmp.sort()
        median = tmp[self._fill // 2]

        if self._bpm == 0:
            self._bpm = median
        else:
            self._bpm = round(0.5 * self._bpm + 0.5 * median)

        return self._bpm

    def bpm(self) -> int:
        return self._bpm

    def ready(self) -> bool:
        return self._fill >= 1


class HeartRateMonitor:
    def __init__(self):
        self._bandpass = BandPass()
        self._envelope = PeakEnvelope()
        self._detector = BeatDetector()
        self._motion = MotionGate()
        self._bpmEst = BpmEstimator()
        
        self._inMotion = False
        self._contactMs = 0
        self.reset()

    def reset(self):
        self._bandpass.reset()
        self._envelope.reset()
        self._detector.reset()
        self._motion.reset()
        self._bpmEst.reset()
        self._inMotion = False
        self._contactMs = 0

    def onContact(self, irValue: float, nowMs: int):
        self._bandpass.seed(irValue)
        self._envelope.reset()
        self._detector.reset()
        self._detector.syncTime(nowMs)
        self._motion.reset()
        self._bpmEst.softReset()
        self._contactMs = nowMs

    def update(self, irValue: float, nowMs: int) -> bool:
        bpf = self._bandpass.process(irValue)
        env = self._envelope.process(bpf, self._inMotion)
        
        self._inMotion = self._motion.process(abs(bpf), env, nowMs)
        
        interval = self._detector.process(bpf, env, nowMs, self._inMotion)
        
        if interval > 0:
            self._bpmEst.pushInterval(interval)
            return True
            
        return False

    def getBpm(self) -> int:
        return self._bpmEst.bpm() if self._bpmEst.ready() else 0
