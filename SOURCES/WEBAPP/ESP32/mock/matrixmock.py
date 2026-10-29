#!/usr/bin/env python3
"""Mock of the DSP's web server for the Matrix page, to try the page in a browser without a DSP.

Serves the page straight from aurora/matrixpage.h (re-read on every load, so an edit shows on a
browser reload) and dark.css from SOURCES/WEBAPP/css, and answers the routes the page calls with
an in-memory matrix model. Patches follow the rules of matrixmodel.cpp (ranges, Biquad budget,
Link groups, the Preset check); Filter files are parsed, exported and plotted with the reference
implementation SOURCES/filterfile/aurorafilter.py. Python standard library only.

Not covered: the DSP itself, the firmware's float rounding, its timing and heap. Preset files are
read leniently, not with the firmware's checks.

Run: python3 SOURCES/WEBAPP/ESP32/mock/matrixmock.py [--port 8080] [--nout 8] [--nbiquad 10]
then open http://localhost:8080/
"""
import argparse
import copy
import json
import math
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, '..', 'aurora', 'matrixpage.h')
CSS = os.path.join(HERE, '..', '..', 'css', 'dark.css')
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'filterfile'))

import aurorafilter as af  # noqa: E402

FS = 48000
NAME_LEN = 16  # bytes of UTF-8 (MATRIX_NAME_LEN)
GAIN = (-80.0, 12.0)
FREQ = (20.0, 20000.0)
Q = (0.1, 20.0)
SLOT_GAIN = (-24.0, 24.0)
SLOPE = (0.1, 1.0)
MAX_DELAY = 1920
OUT_LINKS = 4
NUMPRESETS = 4
TYPES = ['peq', 'hp', 'lp', 'lshelf', 'hshelf', 'phase']
ROUTES = ['Bus 1', 'Bus 2', 'Bus 1+2']
MAX_ORDER = 8
PORT_ANALOG, PORT_USB, PORT_EXP, PORT_SPDIF, PORT_OUT = 0, 1, 2, 4, 5


def page_html():
    """The page as the firmware sends it: the C string of matrixpage.h."""
    with open(PAGE, encoding='utf-8') as f:
        m = re.search(r'matrixpage_html\[\]\s*=\s*"(.*?)"\s*R"!\^!\((.*?)\)!\^!"', f.read(), re.S)
    if not m:
        raise RuntimeError('matrixpage_html not found in ' + PAGE)
    return m.group(1) + m.group(2)


# ---- Sources, SPDIF out pairs and Slot choices as in matrixmodel.cpp

def source_code(n):
    if n < 8:
        return (PORT_ANALOG << 16) | n
    if n < 16:
        return (PORT_USB << 16) | (n - 8)
    if n < 24:
        return (PORT_EXP << 16) | (n - 16)
    return (PORT_SPDIF << 16) | (n - 24)


def source_name(n):
    if n < 8:
        return 'Analog %d' % (n + 1)
    if n < 16:
        return 'USB %d' % (n - 7)
    if n < 24:
        return 'Exp %d' % (n - 15)
    return 'SPDIF L' if n == 24 else 'SPDIF R'


SOURCES = [{'n': source_name(n), 'v': source_code(n)} for n in range(26)]


def spdif_code(n):
    if n < 4:
        return (PORT_ANALOG << 16) | (2 * n)
    if n < 8:
        return (PORT_USB << 16) | (2 * (n - 4))
    if n < 12:
        return (PORT_EXP << 16) | (2 * (n - 8))
    if n < 13:
        return PORT_SPDIF << 16
    return (PORT_OUT << 16) | (2 * (n - 13))


def spdif_name(n):
    if n < 12:
        return '%s %d+%d' % ('Analog' if n < 4 else 'USB' if n < 8 else 'Exp', 2 * (n % 4) + 1, 2 * (n % 4) + 2)
    if n < 13:
        return 'SPDIF In'
    return 'Output %d+%d' % (2 * (n - 13) + 1, 2 * (n - 13) + 2)


def choice(n):
    """Selectable Slot n: PEQ, low shelf, high shelf, Phase, then HP and LP with BW 1-8 and LR 2, 4, 8."""
    s = {'type': 'peq', 'lr': False, 'order': 1, 'freq': 1000.0, 'q': 1.0, 'gain': 0.0, 'slope': 1.0, 'bypass': False}
    if n < 4:
        s['type'] = ['peq', 'lshelf', 'hshelf', 'phase'][n]
        return s
    n -= 4
    s['type'] = 'hp' if n < MAX_ORDER + 3 else 'lp'
    n %= MAX_ORDER + 3
    s['lr'] = n >= MAX_ORDER
    s['order'] = 2 << (n - MAX_ORDER) if s['lr'] else n + 1
    return s


CHOICES = [choice(n) for n in range(4 + 2 * (MAX_ORDER + 3))]


def crossover(s):
    return s['type'] in ('hp', 'lp')


def cost(s):
    if not crossover(s):
        return 1
    return s['order'] // 2 if s['lr'] else (s['order'] + 1) // 2


def used(c):
    return sum(cost(s) for s in c['slots'])


def sections(s):
    """The reference biquads (b0, b1, b2, a1, a2) of a Slot, H = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)."""
    block = {'type': s['type'].upper(), 'params': {'f': s['freq'], 'q': s['q'], 'gain': s['gain'], 's': s['slope']}}
    if crossover(s):
        block['design'] = ('LR%d' if s['lr'] else 'BW%d') % (6 * s['order'])
    return af.sections(block, FS)


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def name_ok(s):
    return len(s.encode('utf-8')) <= NAME_LEN


class EditError(Exception):
    def __init__(self, reason, status=400):
        super().__init__(reason)
        self.reason, self.status = reason, status


# ---- The matrix model

class Matrix:
    def __init__(self, nbus=2, nout=8, nbiquad=10, maxdly=MAX_DELAY):
        self.nbus, self.nout, self.nbiquad = nbus, nout, nbiquad
        self.maxdly = maxdly if 0 < maxdly < MAX_DELAY else MAX_DELAY
        self.preset = 0
        self.files = [None] * NUMPRESETS  # per Preset slot its stored Preset (savePreset()), None = no file
        self.vol = -30.0
        self.reset()

    def reset(self):
        self.name = ''
        self.spdif = spdif_code(0)
        self.bus_link = {'mute': False, 'filters': False, 'gain': False}
        self.out_links = [{'mute': False, 'filters': False, 'gain': False, 'delay': False} for _ in range(OUT_LINKS)]
        ch = {'mute': False, 'gain': 0.0, 'gainBypass': False, 'polarity': False, 'delay': 0, 'delayBypass': False,
              'link': -1, 'slots': []}
        self.buses = [dict(copy.deepcopy(ch), name='Bus %d' % (b + 1), sel=source_code(b)) for b in range(self.nbus)]
        self.outs = [dict(copy.deepcopy(ch), name='Output %d' % (o + 1), sel=0, mute=True) for o in range(self.nout)]

    # -- JSON as the firmware sends it

    def channel_json(self, c, is_bus, limits):
        o = {'name': c['name'], 'source' if is_bus else 'route': c['sel'], 'mute': c['mute'], 'gain': c['gain'],
             'gainBypass': c['gainBypass'], 'link': self.link_id(c['link'], is_bus)}
        if not is_bus:
            o.update(polarity=c['polarity'], delaySamples=c['delay'], delayBypass=c['delayBypass'])
            if limits:
                o.update(delayUs=c['delay'] * 1e6 / FS, delayMm=c['delay'] * 343000.0 / FS)
        o['slots'] = []
        for s in c['slots']:
            j = {'type': s['type']}
            if crossover(s):
                j.update(design='lr' if s['lr'] else 'bw', order=s['order'])
            j.update(freq=s['freq'], q=s['q'], gain=s['gain'], slope=s['slope'], bypass=s['bypass'])
            if limits:
                j['avail'] = self.nbiquad - used(c) + cost(s)
            o['slots'].append(j)
        if limits:
            o.update(budget=self.nbiquad, used=used(c), free=self.nbiquad - used(c))
        return o

    @staticmethod
    def link_id(link, is_bus):
        return '' if link < 0 else 'bus' if is_bus else 'ABCD'[link]

    def state(self):
        choices = []
        for s in CHOICES:
            o = {'t': s['type']}
            if crossover(s):
                o.update(d='lr' if s['lr'] else 'bw', o=s['order'])
            o['c'] = cost(s)
            choices.append(o)
        return {
            'preset': self.preset, 'presetname': self.name, 'nbus': self.nbus, 'nout': self.nout,
            'nbiquad': self.nbiquad, 'gainmin': GAIN[0], 'gainmax': GAIN[1], 'delaymax': self.maxdly,
            'peq': {'fmin': FREQ[0], 'fmax': FREQ[1], 'qmin': Q[0], 'qmax': Q[1], 'gmin': SLOT_GAIN[0], 'gmax': SLOT_GAIN[1]},
            'slopemin': SLOPE[0], 'slopemax': SLOPE[1], 'types': TYPES, 'choices': choices, 'sources': SOURCES,
            'routes': ROUTES,
            'links': {'bus': dict(self.bus_link, id='bus', delay=False),
                      'outs': [dict(l, id='ABCD'[g]) for g, l in enumerate(self.out_links)]},
            'spdifout': self.spdif,
            'spdifopts': [{'n': spdif_name(n), 'v': spdif_code(n)} for n in range(13 + self.nout // 2)],
            'buses': [self.channel_json(c, True, True) for c in self.buses],
            'outs': [self.channel_json(c, False, True) for c in self.outs],
        }

    def save_preset(self):
        doc = {'v': 3}
        if self.name:
            doc['name'] = self.name
        doc.update(spdif=self.spdif, links={'bus': dict(self.bus_link), 'outs': copy.deepcopy(self.out_links)},
                   buses=[self.channel_json(c, True, False) for c in self.buses],
                   outs=[self.channel_json(c, False, False) for c in self.outs])
        return doc

    def coeffs(self, ch, idx):
        """GET /matrix/coeffs: the active biquads, a1 and a2 negated and floored to 8.24 as the DSP stores them."""
        c = self.channel(ch, idx)
        bq = []
        for s in c['slots']:
            if s['bypass']:
                continue
            for b0, b1, b2, a1, a2 in sections(s):
                bq.append([math.floor(v * 16777216.0) / 16777216.0 for v in (b0, b1, b2, -a1, -a2)])
        return {'fs': FS, 'bq': bq}

    def channel(self, ch, idx):
        lst = self.buses if ch == 'bus' else self.outs if ch == 'out' else None
        if lst is None or not is_int(idx) or not 0 <= idx < len(lst):
            raise EditError('no such channel')
        return lst[idx]

    # -- patches (MatrixModel::apply)

    def apply(self, patch):
        patch = dict(patch)
        if 'preset' in patch:
            if not is_int(patch['preset']) or patch['preset'] != self.preset:
                raise EditError('preset changed; refresh the matrix', 409)
            del patch['preset']
        if 'linkGroup' in patch:
            return self.apply_link_group(patch)
        kind = patch.get('ch')
        if not isinstance(kind, str):
            raise EditError('missing ch')
        if kind == 'spdif':
            if not is_int(patch.get('sel')) or patch['sel'] not in [spdif_code(n) for n in range(13 + self.nout // 2)]:
                raise EditError('no such SPDIF out')
            self.spdif = patch['sel']
            return
        if kind == 'preset':
            for k in patch:
                if k not in ('ch', 'name'):
                    raise EditError('unknown field ' + k)
            if 'name' not in patch:
                raise EditError('missing name')
            if not isinstance(patch['name'], str):
                raise EditError('name must be a string')
            if not name_ok(patch['name']):
                raise EditError('name too long')
            self.name = patch['name']
            return
        if kind == 'addon':
            return  # no Add-on B here
        if kind not in ('bus', 'out'):
            raise EditError('unknown channel kind')
        is_bus = kind == 'bus'
        if not is_int(patch.get('idx')):
            raise EditError('missing idx')
        idx = patch['idx']
        lst = self.buses if is_bus else self.outs
        if not 0 <= idx < len(lst):
            raise EditError('no such channel')
        nxt = copy.deepcopy(lst[idx])
        prev_link = nxt['link']
        changed = set()  # of mute, gain, delay, filters
        for key, v in patch.items():
            if key in ('ch', 'idx'):
                continue
            if key == 'name':
                if not isinstance(v, str):
                    raise EditError('name must be a string')
                if not name_ok(v):
                    raise EditError('name too long')
                nxt['name'] = v
            elif key == 'mute':
                nxt['mute'] = self.boolean(key, v)
                changed.add('mute')
            elif key == 'gain':
                if not is_num(v):
                    raise EditError('gain must be a number')
                if not GAIN[0] <= v <= GAIN[1]:
                    raise EditError('gain out of range')
                nxt['gain'] = float(v)
                changed.add('gain')
            elif key == 'gainBypass':
                nxt['gainBypass'] = self.boolean(key, v)
                changed.add('gain')
            elif key in ('polarity', 'delayUs', 'delayBypass'):
                if is_bus:
                    raise EditError('buses have no ' + key)
                if key == 'polarity':
                    nxt['polarity'] = self.boolean(key, v)
                elif key == 'delayBypass':
                    nxt['delayBypass'] = self.boolean(key, v)
                    changed.add('delay')
                else:
                    if not is_num(v):
                        raise EditError('delayUs must be a number')
                    if not 0.0 <= v <= 1e6:
                        raise EditError('delay out of range')
                    samples = int(math.floor(v * FS / 1e6 + 0.5))
                    if samples > self.maxdly:
                        raise EditError('delay out of range')
                    nxt['delay'] = samples
                    changed.add('delay')
            elif key == 'slot':
                if not isinstance(v, dict):
                    raise EditError('slot must be an object')
                self.apply_slot(v, nxt)
                changed.add('filters')
            elif key == 'slots':
                if not isinstance(v, list):
                    raise EditError('slots must be an array')
                nxt['slots'] = []
                for s in v:
                    if not isinstance(s, dict):
                        raise EditError('slots must hold objects')
                    self.edit_slot(s, nxt, -1)
                changed.add('filters')
            elif key == 'link':
                if not isinstance(v, str):
                    raise EditError('link must be a string')
                nxt['link'] = self.link_from_id(v, is_bus)
            elif key == 'source' and is_bus:
                if not is_int(v) or v not in [s['v'] for s in SOURCES]:
                    raise EditError('no such source')
                nxt['sel'] = v
            elif key == 'route' and not is_bus:
                if not is_int(v) or not 0 <= v < len(ROUTES):
                    raise EditError('no such route')
                nxt['sel'] = v
            else:
                raise EditError('unknown field ' + key)

        # A joining Channel takes the linked aspects from the Link group's first other member.
        if nxt['link'] >= 0 and nxt['link'] != prev_link:
            linked = self.bus_link if is_bus else self.out_links[nxt['link']]
            for i, other in enumerate(lst):
                if i == idx or other['link'] != nxt['link']:
                    continue
                for aspect in ('mute', 'gain', 'delay', 'filters'):
                    if linked.get(aspect) and aspect in changed:
                        raise EditError('a Channel joining a Link group takes over its linked ' + aspect)
                self.copy_linked(linked, other, nxt, ('mute', 'gain', 'delay', 'filters'))
                break
        lst[idx] = nxt
        if nxt['link'] >= 0:
            linked = self.bus_link if is_bus else self.out_links[nxt['link']]
            for i, other in enumerate(lst):
                if i != idx and other['link'] == nxt['link']:
                    self.copy_linked(linked, nxt, other, changed)

    @staticmethod
    def copy_linked(linked, src, dst, aspects):
        if linked.get('mute') and 'mute' in aspects:
            dst['mute'] = src['mute']
        if linked.get('gain') and 'gain' in aspects:
            dst['gain'], dst['gainBypass'] = src['gain'], src['gainBypass']
        if linked.get('delay') and 'delay' in aspects:
            dst['delay'], dst['delayBypass'] = src['delay'], src['delayBypass']
        if linked.get('filters') and 'filters' in aspects:
            dst['slots'] = copy.deepcopy(src['slots'])

    @staticmethod
    def boolean(key, v):
        if not isinstance(v, bool):
            raise EditError(key + ' must be true or false')
        return v

    @staticmethod
    def link_from_id(v, is_bus):
        if v == '':
            return -1
        if is_bus and v == 'bus':
            return 0
        if not is_bus and len(v) == 1 and 'A' <= v <= 'ABCD'[OUT_LINKS - 1]:
            return ord(v) - ord('A')
        raise EditError("Link group outside the Channel's scope")

    def apply_link_group(self, patch):
        gid = patch['linkGroup']
        if gid == 'bus':
            is_bus, g, nxt = True, 0, dict(self.bus_link)
        elif isinstance(gid, str) and len(gid) == 1 and 'A' <= gid <= 'ABCD'[OUT_LINKS - 1]:
            is_bus, g, nxt = False, ord(gid) - ord('A'), dict(self.out_links[ord(gid) - ord('A')])
        else:
            raise EditError('no such Link group')
        old = dict(nxt)
        touched = False
        for key, v in patch.items():
            if key == 'linkGroup':
                continue
            if key not in ('mute', 'filters', 'gain', 'delay'):
                raise EditError('unknown field ' + key)
            v = self.boolean(key, v)
            if is_bus and v and key == 'delay':
                raise EditError('Buses have no delay link')
            if not is_bus or key != 'delay':
                nxt[key] = v
            touched = True
        if not touched:
            raise EditError('missing linked aspect')
        if is_bus:
            self.bus_link = nxt
        else:
            self.out_links[g] = nxt
        # When an aspect becomes linked, the first member supplies its value.
        lst = self.buses if is_bus else self.outs
        members = [c for c in lst if c['link'] == g]
        newly = [a for a in ('mute', 'gain', 'delay', 'filters') if nxt.get(a) and not old.get(a)]
        for c in members[1:]:
            self.copy_linked(nxt, members[0], c, newly)

    def apply_slot(self, op, c):
        name = op.get('op')
        if not isinstance(name, str):
            raise EditError('missing slot op')
        if name not in ('add', 'del', 'set'):
            raise EditError('unknown slot op')
        if name == 'add':
            return self.edit_slot(op, c, -1)
        i = op.get('i')
        if not is_int(i) or not 0 <= i < len(c['slots']):
            raise EditError('no such slot')
        if name == 'del':
            del c['slots'][i]
        else:
            self.edit_slot(op, c, i)

    def edit_slot(self, op, c, at):
        add = at < 0
        s = dict(CHOICES[0] if add else c['slots'][at])
        for key in ('freq', 'q', 'gain', 'slope'):
            if key not in op:
                continue
            if not is_num(op[key]):
                raise EditError(key + ' must be a number')
            s[key] = float(op[key])
            bad = self.out_of_range(s)
            if bad:
                raise EditError(bad + ' out of range')
        if 'bypass' in op:
            s['bypass'] = self.boolean('bypass', op['bypass'])
        if 'type' in op:
            if op['type'] not in TYPES:
                raise EditError('no such slot type')
            if not add and op['type'] != s['type']:  # a hidden design/order of the old type must not carry over
                s['lr'], s['order'] = False, 1
            s['type'] = op['type']
        if 'design' in op:
            if op['design'] not in ('bw', 'lr'):
                raise EditError('design must be bw or lr')
            s['lr'] = op['design'] == 'lr'
        if 'order' in op:
            if not is_int(op['order']):
                raise EditError('order must be a number')
            s['order'] = op['order']
        if not crossover(s):
            s['lr'], s['order'] = False, 1
        bad = self.out_of_range(s)
        if bad:
            raise EditError(bad + ' out of range')
        if crossover(s) and not (s['order'] in (2, 4, 8) if s['lr'] else 1 <= s['order'] <= MAX_ORDER):
            raise EditError('Linkwitz-Riley order must be 2, 4 or 8' if s['lr'] else 'Butterworth order must be 1 to 8')
        if used(c) - (0 if add else cost(c['slots'][at])) + cost(s) > self.nbiquad:
            raise EditError('no biquads left' if add else 'not enough biquads left')
        if add:
            c['slots'].append(s)
        else:
            c['slots'][at] = s

    @staticmethod
    def out_of_range(s):
        for key, (lo, hi) in (('freq', FREQ), ('q', Q), ('gain', SLOT_GAIN), ('slope', SLOPE)):
            if not lo <= s[key] <= hi:
                return key
        return None

    # -- Filter files

    def filter_file(self, ch, idx):
        """GET /matrix/filterfile: the Channel as a Filter file with the reference coefficients."""
        c = self.channel(ch, idx)
        blocks = []
        for s in c['slots']:
            t = s['type'].upper()
            params = {k: {'f': s['freq'], 'q': s['q'], 'gain': s['gain'], 's': s['slope']}[k] for k in af.PARAMS[t]}
            design = (('LR%d' if s['lr'] else 'BW%d') % (6 * s['order'])) if crossover(s) else None
            blocks.append((t, design, params, s['bypass']))
        if ch == 'bus':
            return af.write(blocks, name=c['name'], gain=c['gain'])
        return af.write(blocks, name=c['name'], gain=c['gain'], delay_ms=c['delay'] * 1000.0 / FS,
                        polarity='inverted' if c['polarity'] else 'normal')

    def import_filter_file(self, ch, idx, preset, text):
        """POST /matrix/filterfile: the Filter file as one patch (filterFilePatch()). Raises EditError or FilterFileError."""
        if ch not in ('bus', 'out'):
            raise af.FilterFileError(['unknown channel kind'])
        row = af.parse(text, bus=ch == 'bus', biquads=self.nbiquad, max_delay_samples=self.maxdly, fs=FS)
        patch = {'ch': ch, 'idx': idx, 'preset': preset, 'gain': row['gain'], 'gainBypass': False}
        if row['name'] is not None:
            patch['name'] = row['name']
        if ch == 'out':
            patch.update(polarity=row['polarity'] == 'inverted', delayUs=row['delay'] * 1e6 / FS, delayBypass=False)
        slots = []
        for b in row['blocks']:
            p = b['params']
            s = {'type': b['type'].lower(), 'freq': p['f']}
            if b['type'] in ('HP', 'LP'):
                lr = b['design'] in af.LINKWITZ_RILEY
                s.update(design='lr' if lr else 'bw',
                         order=(af.LINKWITZ_RILEY if lr else af.BUTTERWORTH)[b['design']])
            for key, name in (('q', 'q'), ('gain', 'gain'), ('s', 'slope')):
                if key in p:
                    s[name] = p[key]
            s['bypass'] = b['bypass']
            slots.append(s)
        patch['slots'] = slots
        self.apply(patch)

    # -- Presets

    def load_preset(self, doc):
        """Replaces the matrix by a Preset file (savePreset() format); lenient, raises EditError on a bad one."""
        if not isinstance(doc, dict) or not isinstance(doc.get('buses'), list) or not isinstance(doc.get('outs'), list):
            raise EditError('bad preset file')
        before = copy.deepcopy(self.__dict__)
        try:
            self.reset()
            self.name = doc.get('name', '')
            if not isinstance(self.name, str) or not name_ok(self.name):
                raise EditError('bad name')
            if 'spdif' in doc:
                self.apply({'ch': 'spdif', 'sel': doc['spdif']})
            links = doc.get('links', {})
            for c, lst, kind in ((doc['buses'], self.buses, 'bus'), (doc['outs'], self.outs, 'out')):
                for i, o in enumerate(c[:len(lst)]):
                    if not isinstance(o, dict):
                        raise EditError('bad channel')
                    patch = {k: v for k, v in o.items() if k not in ('delaySamples', 'link')}
                    if kind == 'out' and 'delaySamples' in o:
                        if not is_int(o['delaySamples']):
                            raise EditError('bad delay')
                        patch['delayUs'] = o['delaySamples'] * 1e6 / FS
                    patch.update(ch=kind, idx=i)
                    self.apply(patch)
                    lst[i]['link'] = self.link_from_id(o.get('link', ''), kind == 'bus')
            if isinstance(links.get('bus'), dict):
                self.bus_link.update({k: bool(links['bus'].get(k)) for k in self.bus_link})
            for g, l in enumerate(links.get('outs', [])[:OUT_LINKS]):
                if isinstance(l, dict):
                    self.out_links[g].update({k: bool(l.get(k)) for k in self.out_links[g]})
        except EditError as e:
            self.__dict__.update(before)
            raise EditError(e.reason)

    def switch_preset(self, p):
        self.files[self.preset] = self.save_preset()  # Autosave
        self.preset = p
        if self.files[p] is None:
            self.reset()
        else:
            self.load_preset(self.files[p])

    def presets_json(self):
        names = [(f or {}).get('name', '') for f in self.files]
        names[self.preset] = self.name
        used_ = [f is not None for f in self.files]
        used_[self.preset] = True
        return {'pre': self.preset, 'used': used_, 'names': names}


def demo(m):
    """Something to look at: a two-way speaker in Preset A, the same with a bass boost in Preset B."""
    example = os.path.join(HERE, '..', '..', '..', 'filterfile', 'example.txt')
    m.apply({'ch': 'preset', 'name': 'Two-way'})
    for o, (name, route) in enumerate([('Woofer L', 0), ('Woofer R', 1), ('Tweeter L', 0), ('Tweeter R', 1)][:m.nout]):
        m.apply({'ch': 'out', 'idx': o, 'name': name, 'route': route, 'mute': False})
    if m.nout >= 1 and m.nbiquad >= 7:
        with open(example, encoding='utf-8') as f:
            m.import_filter_file('out', 0, m.preset, f.read())
    if m.nout >= 3:
        m.apply({'ch': 'out', 'idx': 2, 'gain': -2.0,
                 'slots': [{'type': 'hp', 'design': 'lr', 'order': 4, 'freq': 2500}]})
    m.switch_preset(1)
    m.load_preset(m.files[0])
    m.apply({'ch': 'preset', 'name': 'Bass boost'})
    m.apply({'ch': 'bus', 'idx': 0, 'slots': [{'type': 'lshelf', 'freq': 120, 'gain': 4.0, 'slope': 0.7}]})
    m.switch_preset(0)


# ---- HTTP

class Handler(BaseHTTPRequestHandler):
    server_version = 'AuroraMock/1'

    def log_message(self, fmt, *args):
        if not self.server.quiet:
            sys.stderr.write('%s %s\n' % (self.command, fmt % args))

    def send(self, status, body=b'', ctype='text/plain', headers=()):
        if self.server.latency:
            time.sleep(self.server.latency)
        if isinstance(body, str):
            body = body.encode('utf-8')
        elif not isinstance(body, bytes):
            body, ctype = json.dumps(body).encode('utf-8'), 'application/json'
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def args(self):
        url = urlparse(self.path)
        return url.path, {k: v[0] for k, v in parse_qs(url.query).items()}

    def int_arg(self, q, key):
        try:
            return int(q[key])
        except (KeyError, ValueError):
            return None

    def body(self):
        return self.rfile.read(int(self.headers.get('Content-Length') or 0))

    def do_GET(self):
        path, q = self.args()
        m = self.server.matrix
        with self.server.lock:
            if path == '/':
                return self.send(200, page_html(), 'text/html')
            if path == '/dark.css':
                with open(CSS, 'rb') as f:
                    return self.send(200, f.read(), 'text/css')
            if path == '/matrix':
                return self.send(200, m.state())
            if path in ('/matrix/coeffs', '/matrix/filterfile'):
                ch, idx = q.get('ch'), self.int_arg(q, 'idx')
                try:
                    if path == '/matrix/coeffs':
                        return self.send(200, m.coeffs(ch, idx))
                    return self.send(200, m.filter_file(ch, idx), 'text/plain; charset=utf-8',
                                     [('Content-Disposition', 'attachment; filename="filter-%s%d.txt"' % (ch, idx + 1))])
                except EditError:
                    return self.send(400, 'no such channel')
            if path == '/preset':
                return self.send(200, m.presets_json())
            if path == '/preset.param':
                return self.send(200, json.dumps(m.save_preset()), 'application/json',
                                 [('Content-Disposition', 'attachment; filename="preset-%s.json"' % 'ABCD'[m.preset])])
            if path == '/mvol':
                return self.send(200, {'vol': m.vol})
            if path == '/idle':
                return self.send(200, '1')
            if path == '/wificonfig':
                return self.send(200, self.server.wifi)
        self.send(404, 'Not found')

    def do_POST(self):
        path, q = self.args()
        m = self.server.matrix
        raw = self.body()
        with self.server.lock:
            if path == '/matrix/filterfile':
                ch, idx, preset = q.get('ch'), self.int_arg(q, 'idx'), self.int_arg(q, 'preset')
                if ch is None or idx is None or preset is None:
                    return self.send(400, {'errors': ['missing ch, idx or preset']})
                try:
                    m.import_filter_file(ch, idx, preset, raw)
                except af.FilterFileError as e:
                    return self.send(400, {'errors': e.errors})
                except EditError as e:
                    return self.send(e.status, {'errors': [e.reason]})
                return self.send(200, m.state())
            if path == '/matrix/import':
                preset = self.int_arg(q, 'preset')
                if preset is None:
                    return self.send(400, 'missing preset')
                if preset != m.preset:
                    return self.send(409, 'preset changed; refresh the matrix')
                try:
                    m.load_preset(json.loads(raw))
                except ValueError:
                    return self.send(400, 'bad preset file')
                except EditError as e:
                    return self.send(e.status, e.reason)
                return self.send(200, m.state())
            try:
                doc = json.loads(raw or b'{}')
            except ValueError:
                return self.send(400, 'Bad JSON')
            if not isinstance(doc, dict):
                return self.send(400, 'Bad JSON')
            if path == '/matrix':
                try:
                    m.apply(doc)
                except EditError as e:
                    return self.send(e.status, e.reason)
                return self.send(200, m.state())
            if path == '/preset':
                p = doc.get('pre')
                if not is_int(p) or not 0 <= p < NUMPRESETS:
                    return self.send(400, 'bad preset')
                m.switch_preset(p)
                return self.send(200)
            if path == '/mvol':
                if is_num(doc.get('vol')):
                    m.vol = max(-80.0, min(0.0, float(doc['vol'])))
                return self.send(200)
            w = self.server.wifi
            if path == '/wifi':
                w['ssid'] = doc.get('ssid', '')
                w['connected'] = bool(w['ssid'])
                if w['connected']:
                    w['ip'] = '192.168.1.42'
                else:
                    w.pop('ip', None)
                return self.send(200)
            if path == '/pwdap':
                w['apname'], w['apchannel'] = doc.get('apname', w['apname']), doc.get('apchannel', w['apchannel'])
                return self.send(200)
            if path == '/apoff':
                w['apoff'] = bool(doc.get('apoff'))
                w['apup'] = not (w['apoff'] and w['connected'])
                return self.send(200)
        self.send(404, 'Not found')


def make_server(host='127.0.0.1', port=8080, matrix=None, latency=0.0, quiet=False):
    srv = ThreadingHTTPServer((host, port), Handler)
    srv.matrix = matrix or Matrix()
    srv.lock = threading.Lock()
    srv.latency = latency
    srv.quiet = quiet
    srv.wifi = {'apname': 'AURORA', 'apchannel': 6, 'ssid': '', 'connected': False, 'apopenfallback': False,
                'apoff': False, 'apup': True}
    return srv


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--host', default='127.0.0.1', help='address to listen on (0.0.0.0: reachable from a phone)')
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--nbus', type=int, default=2, choices=(1, 2))
    ap.add_argument('--nout', type=int, default=8, choices=range(1, 9), metavar='1-8')
    ap.add_argument('--nbiquad', type=int, default=10, help='Biquad budget per Channel (default 10)')
    ap.add_argument('--maxdly', type=int, default=MAX_DELAY, help='Output delay limit in samples')
    ap.add_argument('--latency', type=float, default=0.0, help='seconds before every reply, to try slow replies')
    ap.add_argument('--empty', action='store_true', help='start with default Presets instead of the demo')
    ap.add_argument('-q', '--quiet', action='store_true', help='do not log requests')
    a = ap.parse_args(argv)
    m = Matrix(a.nbus, a.nout, a.nbiquad, a.maxdly)
    if not a.empty:
        demo(m)
    srv = make_server(a.host, a.port, m, a.latency, a.quiet)
    print('Matrix page mock on http://%s:%d/ (Ctrl+C stops it)' % ('localhost' if a.host == '127.0.0.1' else a.host,
                                                                   srv.server_address[1]))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
