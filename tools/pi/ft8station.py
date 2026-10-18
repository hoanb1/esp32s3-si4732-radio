#!/usr/bin/env python3
"""FT8 station on a Raspberry Pi: RTL-SDR + jt9 receive, Ft8Pi (GPIO4) transmit.

  ft8station config [--call XV5QH/P] [--grid OL31] [--dial 3573000] [--audio 1250] [--gain 40]
  ft8station show
  ft8station cq                 call CQ and run the QSO with whoever answers
  ft8station answer [CALL]      wait for a CQ (from CALL, or anyone) and work that station
  ft8station send "MESSAGE"     send one message in the next slot
  ft8station listen             only print what is decoded

Settings live in /etc/ft8station.conf (JSON). OpenWebRX holds the RTL-SDR, so it is stopped while
this runs and started again afterwards. The QSO sequence matches ats-mini/Ft8Tx.cpp on the ESP32.
"""
import argparse, json, os, re, signal, subprocess, sys, threading, time, wave

CONF = '/etc/ft8station.conf'
DEFAULTS = {'call': '', 'grid': '', 'dial': 3573000, 'audio': 1250, 'gain': 40}
ENC = '/usr/local/lib/ft8station/enc'      # ft8_lib encoder: text -> 79 tones
TX = '/usr/local/lib/ft8station/ft8'       # Ft8Pi with ft8pi-tones.patch ("T:<79 digits>")
WORK = '/tmp/ft8station'
FT8PI_OFFSET = 1250                        # Ft8Pi puts tone 0 at freq + 1250 Hz
MAX_TRIES = 4


def log(*a):
    print(time.strftime('%H:%M:%S', time.gmtime()), *a, flush=True)


def load_conf():
    c = dict(DEFAULTS)
    try:
        with open(CONF) as f:
            c.update(json.load(f))
    except FileNotFoundError:
        pass
    return c


def valid_call(c):
    return re.fullmatch(r'[A-Z0-9/]{3,10}', c) is not None and re.search(r'\d', c) is not None


def valid_grid(g):
    return re.fullmatch(r'[A-R]{2}\d\d', g) is not None


# ---- transmit ----
def send(conf, text):
    r = subprocess.run([ENC, text], capture_output=True, text=True)
    tones = r.stdout.strip()
    if r.returncode or tones.count(',') != 78:
        log('cannot encode', repr(text))
        return False
    log('TX  ', text)
    freq = int(conf['dial']) + int(conf['audio']) - FT8PI_OFFSET
    subprocess.Popen([TX, '-x', '1', str(freq), 'T:' + tones.replace(',', '')],
                     stdout=open(WORK + '/tx.log', 'a'), stderr=subprocess.STDOUT)
    return True


# ---- receive: RTL-SDR stream mixed to 12 kHz baseband continuously, jt9 per slot ----
class Receiver:
    FS, AFS, OFF, REC = 240000.0, 12000.0, 20000.0, 13.4   # rates, tuner offset, seconds kept per slot

    def __init__(self, conf):
        import numpy as np, SoapySDR
        from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32
        from scipy.signal import firwin
        self.np = np
        self.d = SoapySDR.Device(dict(driver='rtlsdr'))
        self.d.writeSetting('direct_samp', '2')
        self.d.setSampleRate(SOAPY_SDR_RX, 0, self.FS)
        self.d.setFrequency(SOAPY_SDR_RX, 0, float(conf['dial']) - self.OFF)
        self.d.setGain(SOAPY_SDR_RX, 0, float(conf['gain']))
        self.st = self.d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32)
        self.d.activateStream(self.st)
        self.ring = np.zeros(int(self.AFS * 40), np.complex64)
        self.total, self.t_first = 0, None
        self.lock = threading.Lock()
        self.taps = firwin(161, 5000, fs=self.FS).astype(np.float32)
        threading.Thread(target=self._reader, daemon=True).start()
        while self.t_first is None:
            time.sleep(0.1)

    def _reader(self):
        from scipy.signal import lfilter
        np, dec = self.np, int(self.FS / self.AFS)
        buf = np.empty(dec * 1200, np.complex64)
        zi = np.zeros(len(self.taps) - 1, np.complex64)
        n_in = 0
        while True:
            r = self.d.readStream(self.st, [buf], len(buf))
            if r.ret <= 0:
                continue
            k = r.ret
            if self.t_first is None:
                self.t_first = time.time() - k / self.FS
            ph = np.exp(-2j * np.pi * self.OFF * (n_in + np.arange(k)) / self.FS).astype(np.complex64)
            y, zi = lfilter(self.taps, 1.0, buf[:k] * ph, zi=zi)
            y = y[(-n_in) % dec::dec].astype(np.complex64)   # keep samples aligned to the input count
            n_in += k
            with self.lock:
                i, m, R = self.total % len(self.ring), len(y), len(self.ring)
                a = min(m, R - i)
                self.ring[i:i + a] = y[:a]
                self.ring[:m - a] = y[a:]
                self.total += m

    def decode(self, b):
        """jt9 decodes of the slot that starts at wall time b: [(snr, dt, hz, text)]"""
        np = self.np
        with self.lock:
            s0 = int(round((b - self.t_first) * self.AFS))
            y = self.ring[np.arange(s0, s0 + int(self.REC * self.AFS)) % len(self.ring)].copy()
        Y = np.fft.fft(y)                              # USB: keep 100..3500 Hz above the dial
        fr = np.fft.fftfreq(len(y), 1 / self.AFS)
        Y[(fr < 100) | (fr > 3500)] = 0
        a = np.real(np.fft.ifft(Y))
        out = np.zeros(15 * 12000)
        out[:len(a)] = a / (np.max(np.abs(a)) + 1e-9) * 20000
        path = WORK + '/slot.wav'
        w = wave.open(path, 'wb')
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(12000)
        w.writeframes(out.astype(np.int16).tobytes()); w.close()
        r = subprocess.run(['jt9', '-8', '-d', '2', '-a', WORK, '-t', WORK, path],
                           capture_output=True, text=True, timeout=12, cwd=WORK)
        msgs = []
        for line in r.stdout.splitlines():
            m = re.match(r'\s*\d{6}\s+(-?\d+)\s+(-?[\d.]+)\s+(\d+)\s+~\s+(.*?)\s*$', line)
            if m:
                msgs.append((int(m.group(1)), float(m.group(2)), int(m.group(3)), m.group(4)))
        return msgs


def rpt(snr):
    return '%+03d' % max(-24, min(24, snr))


def is_rpt(s):
    return re.fullmatch(r'[+-]\d\d', s) is not None


class Qso:
    """Same states as the ESP32 sequencer. step() gets one listening slot's decodes, returns the reply."""

    def __init__(self, conf, role, want=''):
        self.my, self.grid = conf['call'], conf['grid']
        self.state = 'cq' if role == 'cq' else 'wait'
        self.want, self.dx, self.sent, self.rcvd, self.tries = want, '', '', '', 0
        self.done = False

    def _heard(self, msgs):
        for snr, dt, hz, text in msgs:
            t = text.split()
            if len(t) < 2:
                continue
            if self.state == 'wait':
                if t[0] == 'CQ' and t[1] != self.my and (not self.want or t[1] == self.want):
                    return snr, t
            elif self.state == 'cq':
                if t[0] == self.my and len(t) == 3 and valid_grid(t[2]):
                    return snr, t
            elif t[0] == self.my and t[1] == self.dx:
                return snr, t
        return None

    def _set(self, state, why=''):
        self.state, self.tries = state, 0
        log('QSO', state + (': ' + why if why else ''))

    def _retry(self, msg):
        self.tries += 1
        if self.tries < MAX_TRIES:
            return msg
        log('QSO gave up in state', self.state)
        self.done = True
        return None

    def _complete(self, note=''):
        log('QSO COMPLETE with %s%s: sent %s, received %s' % (self.dx, note, self.sent, self.rcvd))

    def step(self, msgs):
        h = self._heard(msgs)
        x = h[1][2] if h and len(h[1]) > 2 else ''
        my, dx = self.my, self.dx
        if self.state == 'wait':
            if h:
                self.dx = h[1][1]
                self._set('sent_grid', self.dx)
                return '%s %s %s' % (self.dx, my, self.grid)
            return None
        if self.state == 'cq':
            if h:
                self.dx, self.sent = h[1][1], rpt(h[0])
                self._set('sent_rpt', self.dx)
                return '%s %s %s' % (self.dx, my, self.sent)
            self.tries += 1
            if self.tries < MAX_TRIES * 2:
                return 'CQ %s %s' % (my, self.grid)
            log('QSO no answer to CQ')
            self.done = True
            return None
        if self.state == 'sent_grid':
            if h and is_rpt(x):
                self.rcvd, self.sent = x, rpt(h[0])
                self._set('sent_rrpt')
                return '%s %s R%s' % (dx, my, self.sent)
            return self._retry('%s %s %s' % (dx, my, self.grid))
        if self.state == 'sent_rrpt':
            if h and x in ('RR73', 'RRR'):
                self._complete()
                self._set('sent_73')
                return '%s %s 73' % (dx, my)
            return self._retry('%s %s R%s' % (dx, my, self.sent))
        if self.state == 'sent_rpt':
            if h and x.startswith('R') and is_rpt(x[1:]):
                self.rcvd = x[1:]
                self._set('sent_rr73')
                return '%s %s RR73' % (dx, my)
            return self._retry('%s %s %s' % (dx, my, self.sent))
        if self.state == 'sent_rr73':
            if h and x == '73':
                self._complete()
            elif h and x.startswith('R') and self.tries < MAX_TRIES:
                self.tries += 1
                return '%s %s RR73' % (dx, my)
            else:
                self._complete(' (no 73)')
            self.done = True
            return None
        if self.state == 'sent_73':
            if h and x in ('RR73', 'RRR') and self.tries < MAX_TRIES:
                self.tries += 1
                return '%s %s 73' % (dx, my)
            self.done = True
        return None


def run(conf, role, want='', first=None, max_slots=40):
    rx = Receiver(conf)
    log('receiver ready on %.3f kHz USB as %s %s' % (conf['dial'] / 1e3, conf['call'], conf['grid']))
    qso = Qso(conf, role, want) if role in ('cq', 'answer') else None
    b = (time.time() // 15 + 1) * 15
    tx_slot = None
    if first:
        if send(conf, first):
            tx_slot = b
    elif role == 'cq':
        if send(conf, 'CQ %s %s' % (conf['call'], conf['grid'])):
            tx_slot = b
    for _ in range(max_slots):
        while time.time() < b + Receiver.REC + 0.05:
            time.sleep(0.02)
        msgs = rx.decode(b)
        for snr, dt, hz, text in msgs:
            log('RX  %+3d %+.1f %4d  %s' % (snr, dt, hz, text))
        nb = b + 15
        if qso and tx_slot != b:          # answer only after a slot we listened in
            reply = qso.step(msgs)
            if reply and send(conf, reply):
                tx_slot = nb
            if qso.done:
                break
        elif role == 'send' and tx_slot != b:
            break
        b = nb
    if tx_slot is not None and tx_slot >= b:
        time.sleep(max(0, tx_slot + 13.5 - time.time()))   # let the last message finish


def main():
    ap = argparse.ArgumentParser(description='FT8 station: RTL-SDR + jt9 receive, Ft8Pi GPIO4 transmit')
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('config', help='save station settings')
    c.add_argument('--call'); c.add_argument('--grid')
    c.add_argument('--dial', type=int, help='USB dial in Hz (default 3573000)')
    c.add_argument('--audio', type=int, help='TX audio offset in Hz above the dial (default 1250)')
    c.add_argument('--gain', type=float, help='RTL-SDR gain in dB')
    sub.add_parser('show', help='print the settings')
    sub.add_parser('cq', help='call CQ and work whoever answers')
    a = sub.add_parser('answer', help='answer a CQ'); a.add_argument('call', nargs='?', default='')
    s = sub.add_parser('send', help='send one message next slot'); s.add_argument('message')
    sub.add_parser('listen', help='print decodes only')
    args = ap.parse_args()

    if args.cmd != 'show' and os.geteuid() != 0:
        sys.exit('ft8station needs root (it writes %s, drives the GPIO clock and stops/starts openwebrx):\n'
                 '  sudo ft8station %s' % (CONF, ' '.join(sys.argv[1:])))
    conf = load_conf()
    if args.cmd == 'config':
        for k in ('call', 'grid', 'dial', 'audio', 'gain'):
            v = getattr(args, k)
            if v is not None:
                conf[k] = v.upper() if isinstance(v, str) else v
        if conf['call'] and not valid_call(conf['call']):
            sys.exit('callsign %r looks wrong' % conf['call'])
        if conf['grid'] and not valid_grid(conf['grid']):
            sys.exit('grid %r must be 4 characters like OL31' % conf['grid'])
        with open(CONF, 'w') as f:
            json.dump(conf, f, indent=2)
        print(json.dumps(conf, indent=2))
        return
    if args.cmd == 'show':
        print(json.dumps(conf, indent=2))
        return
    if args.cmd != 'listen' and not (conf['call'] and conf['grid']):
        sys.exit('set the station first: ft8station config --call <CALL> --grid <GRID>')

    os.makedirs(WORK, exist_ok=True)
    owrx = subprocess.run(['systemctl', 'is-active', '--quiet', 'openwebrx']).returncode == 0
    if owrx:
        log('stopping openwebrx (it holds the RTL-SDR)')
        subprocess.run(['systemctl', 'stop', 'openwebrx'])
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        if args.cmd == 'send':
            run(conf, 'send', first=args.message.upper(), max_slots=1)
        elif args.cmd == 'listen':
            run(conf, 'listen', max_slots=10 ** 9)
        else:
            run(conf, args.cmd, want=getattr(args, 'call', '').upper())
    except KeyboardInterrupt:
        pass
    finally:
        if owrx:
            log('starting openwebrx again')
            subprocess.run(['systemctl', 'start', 'openwebrx'])


if __name__ == '__main__':
    main()
