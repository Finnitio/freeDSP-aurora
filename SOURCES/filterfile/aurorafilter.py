"""Reference implementation of the Aurora Filter file (docs/filter-file.md).

The coefficient formulas here are the shared truth: the Aurora firmware and any
design software that writes Filter files must produce the same filters, which
vectors.json pins down.

Usage:
  python aurorafilter.py check FILE [--bus] [--biquads N]   parse and verify a Filter file
  python aurorafilter.py vectors                            print the test vectors as JSON
"""
import cmath
import json
import math
import sys

FS = 48000
VERSION = 1
NAME_LEN = 16           # bytes of UTF-8
FREQ = (20.0, 20000.0)  # Hz
Q = (0.1, 20.0)
FILTER_GAIN = (-24.0, 24.0)    # dB, PEQ and shelves
SLOPE = (0.1, 1.0)             # shelf S
CHANNEL_GAIN = (-80.0, 12.0)   # dB
MAX_DELAY_SAMPLES = 1920       # 40 ms at 48 kHz; a Plugin may allow less
MAX_FILE_BYTES = 16384
MAX_ERRORS = 20

TOL_DB = 0.01
TOL_DEG = 0.1
FLOOR_DB = -80.0  # below this reference magnitude only the magnitude is checked

BUTTERWORTH = {'BW6': 1, 'BW12': 2, 'BW18': 3, 'BW24': 4, 'BW30': 5, 'BW36': 6, 'BW42': 7, 'BW48': 8}
LINKWITZ_RILEY = {'LR12': 2, 'LR24': 4, 'LR48': 8}

# Block keyword -> the parameters it takes (besides HP/LP's design token)
PARAMS = {
    'PEQ': ('f', 'gain', 'q'),
    'LSHELF': ('f', 'gain', 's'),
    'HSHELF': ('f', 'gain', 's'),
    'PHASE': ('f', 'q'),
    'HP': ('f',),
    'LP': ('f',),
}
RANGES = {'f': FREQ, 'q': Q, 'gain': FILTER_GAIN, 's': SLOPE}
PARAM_NAMES = {'f': 'f', 'q': 'Q', 'gain': 'gain', 's': 'S'}


# ---------------------------------------------------------------------------
# Coefficients. A section is (b0, b1, b2, a1, a2), normalized to a0 = 1, for
# H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2).

def _norm(b0, b1, b2, a0, a1, a2):
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def peq(f, gain, q, fs=FS):
    A = 10.0 ** (gain / 40.0)
    w0 = 2.0 * math.pi * f / fs
    alpha = math.sin(w0) / (2.0 * q)
    c = math.cos(w0)
    return _norm(1.0 + alpha * A, -2.0 * c, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * c, 1.0 - alpha / A)


def _shelf(high, f, gain, s, fs):
    A = 10.0 ** (gain / 40.0)
    w0 = 2.0 * math.pi * f / fs
    c = math.cos(w0)
    alpha = math.sin(w0) / 2.0 * math.sqrt((A + 1.0 / A) * (1.0 / s - 1.0) + 2.0)
    r = 2.0 * math.sqrt(A) * alpha
    if high:
        return _norm(A * ((A + 1) + (A - 1) * c + r), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - r),
                     (A + 1) - (A - 1) * c + r, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - r)
    return _norm(A * ((A + 1) - (A - 1) * c + r), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - r),
                 (A + 1) + (A - 1) * c + r, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - r)


def low_shelf(f, gain, s, fs=FS):
    return _shelf(False, f, gain, s, fs)


def high_shelf(f, gain, s, fs=FS):
    return _shelf(True, f, gain, s, fs)


def allpass(f, q, fs=FS):
    w0 = 2.0 * math.pi * f / fs
    alpha = math.sin(w0) / (2.0 * q)
    c = math.cos(w0)
    return _norm(1.0 - alpha, -2.0 * c, 1.0 + alpha, 1.0 + alpha, -2.0 * c, 1.0 - alpha)


def first_order(high, f, fs=FS):
    """Bilinear transform with prewarping; b2 = a2 = 0."""
    K = math.tan(math.pi * f / fs)
    if high:
        return _norm(1.0, -1.0, 0.0, 1.0 + K, K - 1.0, 0.0)
    return _norm(K, K, 0.0, 1.0 + K, K - 1.0, 0.0)


def second_order(high, f, q, fs=FS):
    """RBJ cookbook HPF/LPF."""
    w0 = 2.0 * math.pi * f / fs
    alpha = math.sin(w0) / (2.0 * q)
    c = math.cos(w0)
    if high:
        return _norm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha)
    return _norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha)


def butterworth(high, order, f, fs=FS):
    """ceil(order/2) sections: the 1st-order one first if the order is odd, then the pole pairs, lowest Q first."""
    out = [first_order(high, f, fs)] if order % 2 else []
    for k in range(order // 2, 0, -1):
        out.append(second_order(high, f, 1.0 / (2.0 * math.sin(math.pi * (2 * k - 1) / (2.0 * order))), fs))
    return out


def linkwitz_riley(high, order, f, fs=FS):
    """order/2 sections: LR12 is one section with Q = 0.5, otherwise the Butterworth of order/2 twice."""
    if order == 2:
        return [second_order(high, f, 0.5, fs)]
    half = butterworth(high, order // 2, f, fs)
    return half + half


def sections(block, fs=FS):
    """The reference sections of a block dict as returned by parse()."""
    t, p = block['type'], block['params']
    if t == 'PEQ':
        return [peq(p['f'], p['gain'], p['q'], fs)]
    if t == 'LSHELF':
        return [low_shelf(p['f'], p['gain'], p['s'], fs)]
    if t == 'HSHELF':
        return [high_shelf(p['f'], p['gain'], p['s'], fs)]
    if t == 'PHASE':
        return [allpass(p['f'], p['q'], fs)]
    high = t == 'HP'
    d = block['design']
    if d in BUTTERWORTH:
        return butterworth(high, BUTTERWORTH[d], p['f'], fs)
    return linkwitz_riley(high, LINKWITZ_RILEY[d], p['f'], fs)


def section_count(t, design):
    if t in ('HP', 'LP'):
        if design in BUTTERWORTH:
            return (BUTTERWORTH[design] + 1) // 2
        return LINKWITZ_RILEY[design] // 2
    return 1


# ---------------------------------------------------------------------------
# Verification

def response(secs, f, fs=FS):
    """Complex response of cascaded sections at f Hz."""
    z1 = cmath.exp(-2j * math.pi * f / fs)
    h = 1.0 + 0j
    for b0, b1, b2, a1, a2 in secs:
        h *= (b0 + b1 * z1 + b2 * z1 * z1) / (1.0 + a1 * z1 + a2 * z1 * z1)
    return h


def grid(fs=FS):
    """24 points per octave from 10 Hz up to (excluding) fs/2."""
    out, k = [], 0
    while True:
        f = 10.0 * 2.0 ** (k / 24.0)
        if f >= fs / 2.0:
            return out
        out.append(f)
        k += 1


def _db(x):
    return 20.0 * math.log10(x) if x > 0 else -math.inf


def deviation(file_secs, ref_secs, fs=FS):
    """The worst grid point as (f, dB deviation, degrees deviation), or None if all are within tolerance.

    Where the reference is below FLOOR_DB only the file's magnitude is checked: it must stay below
    FLOOR_DB + TOL_DB."""
    worst, score = None, 1.0
    for f in grid(fs):
        hr, hf = response(ref_secs, f, fs), response(file_secs, f, fs)
        ref_db, file_db = _db(abs(hr)), _db(abs(hf))
        if ref_db < FLOOR_DB:
            if file_db >= FLOOR_DB + TOL_DB:
                x = (file_db - ref_db) / TOL_DB
                if x > score:
                    worst, score = (f, file_db - ref_db, 0.0), x
            continue
        if hf == 0:
            return (f, -math.inf, 0.0)
        r = hf / hr
        ddb, ddeg = _db(abs(r)), math.degrees(cmath.phase(r))
        x = max(abs(ddb) / TOL_DB, abs(ddeg) / TOL_DEG)
        if x > score:
            worst, score = (f, ddb, ddeg), x
    return worst


# ---------------------------------------------------------------------------
# Parsing

def _num(tok):
    """A decimal number (point as separator, optional exponent), else None."""
    try:
        v = float(tok)
    except ValueError:
        return None
    if not math.isfinite(v) or tok.strip().lower() in ('nan', 'inf', '+inf', '-inf', 'infinity'):
        return None
    return v


def _fmt(v):
    return ('%.6g' % v)


class FilterFileError(Exception):
    def __init__(self, errors):
        Exception.__init__(self, '\n'.join(errors))
        self.errors = errors


def parse(text, bus=False, biquads=None, max_delay_samples=MAX_DELAY_SAMPLES, fs=FS):
    """Parses and verifies a Filter file.

    bus: the target is a Bus (no delay, no polarity). biquads: the Channel's Biquad budget (None = unchecked).
    Returns {'name', 'gain', 'delay', 'polarity', 'blocks'} (name None if absent; delay in samples;
    each block {'line', 'type', 'design', 'params', 'bypass', 'sections'}), or raises FilterFileError
    with up to MAX_ERRORS messages "line N: ...".
    """
    errors = []

    def err(n, msg):
        if len(errors) < MAX_ERRORS:
            errors.append('line %d: %s' % (n, msg) if n else msg)

    if isinstance(text, bytes):
        if len(text) > MAX_FILE_BYTES:
            raise FilterFileError(['file larger than %d bytes' % MAX_FILE_BYTES])
        try:
            text = text.decode('utf-8')
        except UnicodeDecodeError:
            raise FilterFileError(['file is not UTF-8'])
    elif len(text.encode('utf-8')) > MAX_FILE_BYTES:
        raise FilterFileError(['file larger than %d bytes' % MAX_FILE_BYTES])
    if text.startswith('\ufeff'):
        text = text[1:]

    result = {'name': None, 'gain': 0.0, 'delay': 0, 'polarity': 'normal', 'blocks': []}
    seen = set()
    header = False
    have_fs = False
    block = None  # the block whose coefficient lines are being read

    def close_block(n):
        if block is not None and len(block['coefs']) != block['count']:
            err(block['line'], '%s needs %d coefficient line%s, found %d'
                % (block['label'], block['count'], '' if block['count'] == 1 else 's', len(block['coefs'])))

    lines = text.split('\n')
    for n, raw in enumerate(lines, 1):
        line = raw.rstrip('\r').strip()
        if not line or line.startswith('#'):
            continue
        toks = line.split()
        if not header:
            if [t.lower() for t in toks] != ['aurora-filter', str(VERSION)]:
                if toks and toks[0].lower() == 'aurora-filter':
                    raise FilterFileError(['line %d: unsupported version %s (this is version %d)'
                                           % (n, ' '.join(toks[1:]) or '(none)', VERSION)])
                raise FilterFileError(['line %d: the file must start with "aurora-filter %d"' % (n, VERSION)])
            header = True
            continue

        if _num(toks[0]) is not None:  # coefficient line
            if block is None:
                err(n, 'coefficients without a filter block')
                continue
            vals = [_num(t) for t in toks]
            if len(vals) != 5 or None in vals:
                err(n, 'a coefficient line has 5 numbers: b0 b1 b2 a1 a2')
            else:
                block['coefs'].append(tuple(vals))  # too many or too few: close_block()
            continue

        close_block(n)
        block = None
        key = toks[0].lower()
        rest = line[len(toks[0]):].strip()

        if key == 'fs':
            if 'fs' in seen:
                err(n, 'fs given twice')
            seen.add('fs')
            v = _num(rest) if len(toks) == 2 else None
            if v != fs:
                err(n, 'fs must be %d' % fs)
            have_fs = True
            continue
        if not have_fs:
            err(n, '"fs %d" must come right after the header' % fs)
            have_fs = True  # report once

        if key in ('name', 'gain', 'delay', 'polarity'):
            if key in seen:
                err(n, '%s given twice' % key)
                continue
            seen.add(key)
            if key == 'name':
                size = len(rest.encode('utf-8'))
                if size == 0:
                    err(n, 'name is empty (leave the line out to keep the current name)')
                elif size > NAME_LEN:
                    err(n, 'name longer than %d bytes' % NAME_LEN)
                elif any(ord(c) < 32 or ord(c) == 127 for c in rest):
                    err(n, 'name contains control characters')
                else:
                    result['name'] = rest
            elif key == 'gain':
                v = _num(toks[1]) if len(toks) == 3 and toks[2].lower() == 'db' else None
                if v is None:
                    err(n, 'gain is written "gain <number> dB"')
                elif not CHANNEL_GAIN[0] <= v <= CHANNEL_GAIN[1]:
                    err(n, 'gain %s dB outside %g to %g dB' % (_fmt(v), CHANNEL_GAIN[0], CHANNEL_GAIN[1]))
                else:
                    result['gain'] = v
            elif key == 'delay':
                v = _num(toks[1]) if len(toks) == 3 and toks[2].lower() == 'ms' else None
                if bus:
                    err(n, 'a Bus has no delay')
                elif v is None:
                    err(n, 'delay is written "delay <number> ms"')
                else:
                    samples = math.floor(v * fs / 1000.0 + 0.5) if v >= 0 else -1
                    if samples < 0 or samples > max_delay_samples:
                        err(n, 'delay %s ms outside 0 to %g ms' % (_fmt(v), max_delay_samples * 1000.0 / fs))
                    else:
                        result['delay'] = samples
            else:
                v = rest.lower()
                if bus:
                    err(n, 'a Bus has no polarity')
                elif v not in ('normal', 'inverted'):
                    err(n, 'polarity is "normal" or "inverted"')
                else:
                    result['polarity'] = v
            continue

        t = toks[0].upper()
        if t not in PARAMS:
            err(n, 'unknown line "%s"' % toks[0])
            continue
        args = toks[1:]
        design = None
        if t in ('HP', 'LP'):
            d = args[0].upper() if args else ''
            if d not in BUTTERWORTH and d not in LINKWITZ_RILEY:
                known = ', '.join(list(BUTTERWORTH) + list(LINKWITZ_RILEY))
                err(n, ('%s: unknown design "%s" (%s)' % (t, args[0], known)) if args else
                    '%s needs a design (%s)' % (t, known))
                continue
            design = d
            args = args[1:]
        label = t + (' ' + design if design else '')
        params, bypass, ok = {}, False, True
        for a in args:
            if a.lower() == 'bypass':
                if bypass:
                    err(n, 'bypass given twice')
                    ok = False
                bypass = True
                continue
            k, eq, v = a.partition('=')
            k = k.lower()
            if not eq or k not in PARAMS[t]:
                err(n, '%s does not take "%s"' % (label, a))
                ok = False
                continue
            if k in params:
                err(n, '%s given twice' % PARAM_NAMES[k])
                ok = False
                continue
            x = _num(v)
            lo, hi = RANGES[k]
            if x is None:
                err(n, '%s is not a number' % PARAM_NAMES[k])
                ok = False
            elif not lo <= x <= hi:
                err(n, '%s %s outside %g to %g' % (PARAM_NAMES[k], _fmt(x), lo, hi))
                ok = False
            params[k] = x
        missing = [PARAM_NAMES[k] for k in PARAMS[t] if k not in params]
        if missing:
            err(n, '%s needs %s' % (label, ', '.join(missing)))
            ok = False
        block = {'line': n, 'label': label, 'type': t, 'design': design, 'params': params,
                 'bypass': bypass, 'count': section_count(t, design), 'coefs': [], 'ok': ok}
        result['blocks'].append(block)
    close_block(len(lines))

    if not header:
        raise FilterFileError(['the file must start with "aurora-filter %d"' % VERSION])
    if not have_fs:
        err(0, 'missing "fs %d"' % fs)

    used = 0
    for b in result['blocks']:
        used += b['count']
        if not b['ok'] or len(b['coefs']) != b['count']:
            continue
        ref = sections(b, fs)
        worst = deviation(b['coefs'], ref, fs)
        if worst:
            f, ddb, ddeg = worst
            err(b['line'], '%s deviates from the reference by %.3g dB / %.3g deg at %.0f Hz' % (b['label'], ddb, ddeg, f))
    if biquads is not None and used > biquads:
        err(0, 'the filters need %d biquads, the Channel has %d' % (used, biquads))

    if errors:
        raise FilterFileError(errors)
    for b in result['blocks']:
        b['sections'] = b.pop('coefs')
        for k in ('label', 'count', 'ok'):
            del b[k]
    return result


# ---------------------------------------------------------------------------
# Writing

def block_line(t, design=None, bypass=False, **params):
    """The block line, e.g. block_line('LP', 'LR48', f=2500) -> 'LP LR48 f=2500'."""
    params = {k.lower(): v for k, v in params.items()}
    parts = [t] + ([design] if design else [])
    for k in PARAMS[t]:
        parts.append('%s=%.12g' % (PARAM_NAMES[k], params[k]))
    if bypass:
        parts.append('bypass')
    return ' '.join(parts)


def write(blocks, name=None, gain=None, delay_ms=None, polarity=None, fs=FS):
    """A Filter file with the reference coefficients. blocks: [(type, design, {params}, bypass)]."""
    out = ['aurora-filter %d' % VERSION, 'fs %d' % fs]
    if name is not None:
        out.append('name ' + name)
    if gain is not None:
        out.append('gain %s dB' % repr(float(gain)))
    if delay_ms is not None:
        out.append('delay %s ms' % repr(float(delay_ms)))
    if polarity is not None:
        out.append('polarity ' + polarity)
    for t, design, params, bypass in blocks:
        out.append(block_line(t, design, bypass, **params))
        b = {'type': t, 'design': design, 'params': {k.lower(): v for k, v in params.items()}}
        for s in sections(b, fs):
            out.append('  ' + ' '.join('%.17g' % v for v in s))
    return '\n'.join(out) + '\n'


# ---------------------------------------------------------------------------
# Test vectors

VECTOR_BLOCKS = (
    [('PEQ', None, {'f': f, 'gain': g, 'q': q}) for f, g, q in
     [(20, 6, 0.7), (120, -4, 2.5), (1000, 3, 1), (1000, -12, 10), (10000, 6, 2), (20000, -24, 0.1), (60, 24, 20)]]
    + [(t, None, {'f': f, 'gain': g, 's': s}) for t in ('LSHELF', 'HSHELF') for f, g, s in
       [(80, 6, 1), (1000, -12, 0.5), (8000, -2, 0.7), (20000, 24, 0.1)]]
    + [('PHASE', None, {'f': f, 'q': q}) for f, q in [(20, 0.5), (800, 0.7), (5000, 3), (20000, 20)]]
    + [(t, d, {'f': f}) for t in ('HP', 'LP') for d in list(BUTTERWORTH) + list(LINKWITZ_RILEY)
       for f in (20, 2500, 20000)]
)


def vectors(fs=FS):
    out = []
    for t, design, params in VECTOR_BLOCKS:
        b = {'type': t, 'design': design, 'params': params}
        out.append({'block': block_line(t, design, **params), 'sections': [list(s) for s in sections(b, fs)]})
    return {'fs': fs, 'convention': 'b0 b1 b2 a1 a2, a0 = 1, y = b0 x0 + b1 x1 + b2 x2 - a1 y1 - a2 y2',
            'vectors': out}


def dump_vectors(v):
    """JSON with one vector per line, so a diff shows which block changed."""
    rows = ',\n'.join('  ' + json.dumps(x) for x in v['vectors'])
    return '{"fs": %d,\n "convention": %s,\n "vectors": [\n%s\n ]}\n' % (v['fs'], json.dumps(v['convention']), rows)


def main(argv):
    if len(argv) >= 1 and argv[0] == 'vectors':
        print(dump_vectors(vectors()), end='')
        return 0
    if len(argv) >= 2 and argv[0] == 'check':
        bus = '--bus' in argv
        biquads = None
        if '--biquads' in argv:
            biquads = int(argv[argv.index('--biquads') + 1])
        with open(argv[1], 'rb') as fh:
            data = fh.read()
        try:
            r = parse(data, bus=bus, biquads=biquads)
        except FilterFileError as e:
            print('\n'.join(e.errors))
            return 1
        print('OK: %d block%s, %d biquads' % (len(r['blocks']), '' if len(r['blocks']) == 1 else 's',
                                             sum(len(b['sections']) for b in r['blocks'])))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
