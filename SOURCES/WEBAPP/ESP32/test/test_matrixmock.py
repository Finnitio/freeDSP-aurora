"""Tests for the Matrix page mock server (../mock/matrixmock.py).

Run: SOURCES/WEBAPP/ESP32/test/run.sh
"""
import json
import math
import os
import sys
import threading
import urllib.error
import urllib.request

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'mock'))

import matrixmock  # noqa: E402
import aurorafilter as af  # noqa: E402  (put on the path by matrixmock)

EXAMPLE = os.path.join(HERE, '..', '..', '..', 'filterfile', 'example.txt')


@pytest.fixture
def url():
    m = matrixmock.Matrix()
    matrixmock.demo(m)
    srv = matrixmock.make_server(port=0, matrix=m, quiet=True)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    yield 'http://127.0.0.1:%d' % srv.server_address[1]
    srv.shutdown()
    srv.server_close()


def call(url, path, body=None):
    """(status, text) of a GET, or of a POST of body (a dict is sent as JSON)."""
    if isinstance(body, dict):
        body = json.dumps(body)
    req = urllib.request.Request(url + path, None if body is None else body.encode('utf-8'))
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, r.read().decode('utf-8')
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8')


def state(url):
    return json.loads(call(url, '/matrix')[1])


def test_serves_the_firmware_page_and_css(url):
    status, html = call(url, '/')
    assert status == 200
    assert html.startswith('<!DOCTYPE html>\n<html')
    assert '<script>' in html and html.rstrip().endswith('</html>')
    assert call(url, '/dark.css')[0] == 200


def test_state_has_what_the_page_reads(url):
    s = state(url)
    for key in ('preset', 'presetname', 'sources', 'routes', 'links', 'spdifout', 'spdifopts', 'types', 'choices',
                'delaymax', 'buses', 'outs'):
        assert key in s
    assert len(s['choices']) == 26 and len(s['buses']) == 2 and len(s['outs']) == 8
    out = s['outs'][0]
    for key in ('name', 'route', 'mute', 'gain', 'gainBypass', 'link', 'polarity', 'delayUs', 'delaySamples',
                'delayMm', 'delayBypass', 'slots', 'budget', 'used', 'free'):
        assert key in out
    assert 'avail' in out['slots'][0]


def test_patch_is_checked_like_the_firmware(url):
    assert call(url, '/matrix', {'ch': 'out', 'idx': 1, 'gain': -6.5, 'preset': 0})[0] == 200
    assert state(url)['outs'][1]['gain'] == -6.5
    assert call(url, '/matrix', {'ch': 'out', 'idx': 1, 'gain': 13, 'preset': 0}) == (400, 'gain out of range')
    assert call(url, '/matrix', {'ch': 'out', 'idx': 1, 'gain': 0, 'preset': 2})[0] == 409
    assert call(url, '/matrix', {'ch': 'bus', 'idx': 0, 'polarity': True}) == (400, 'buses have no polarity')
    assert call(url, '/matrix', {'ch': 'out', 'idx': 1, 'name': 'ääääääääX'}) == (400, 'name too long')


def test_slots_respect_the_biquad_budget(url):
    for _ in range(10):
        assert call(url, '/matrix', {'ch': 'out', 'idx': 7, 'slot': {'op': 'add'}})[0] == 200
    assert call(url, '/matrix', {'ch': 'out', 'idx': 7, 'slot': {'op': 'add'}}) == (400, 'no biquads left')
    assert state(url)['outs'][7]['free'] == 0


def test_linked_gain_follows(url):
    for i in (4, 5):
        call(url, '/matrix', {'ch': 'out', 'idx': i, 'link': 'B'})
    call(url, '/matrix', {'linkGroup': 'B', 'gain': True})
    call(url, '/matrix', {'ch': 'out', 'idx': 4, 'gain': -9})
    assert state(url)['outs'][5]['gain'] == -9


def test_filter_file_round_trip_and_plot(url):
    with open(EXAMPLE, encoding='utf-8') as f:
        text = f.read()
    status, body = call(url, '/matrix/filterfile?ch=out&idx=3&preset=0', text)
    assert status == 200
    out = json.loads(body)['outs'][3]
    assert out['name'] == 'Woofer L' and out['used'] == 9
    exported = call(url, '/matrix/filterfile?ch=out&idx=3')[1]
    ref = af.parse(text)
    again = af.parse(exported)
    assert [(b['type'], b['design'], b['params']) for b in again['blocks']] == \
        [(b['type'], b['design'], b['params']) for b in ref['blocks']]
    # the page's formula on /matrix/coeffs gives the reference response
    cf = json.loads(call(url, '/matrix/coeffs?ch=out&idx=3')[1])
    secs = [s for b in ref['blocks'] if not b['bypass'] for s in b['sections']]  # bypassed: not on the DSP
    for f in (50.0, 1000.0, 5000.0):
        w = 2 * math.pi * f / cf['fs']
        db = 0.0
        for b0, b1, b2, a1, a2 in cf['bq']:
            nr, ni = b0 + b1 * math.cos(w) + b2 * math.cos(2 * w), -b1 * math.sin(w) - b2 * math.sin(2 * w)
            dr, di = 1 - a1 * math.cos(w) - a2 * math.cos(2 * w), a1 * math.sin(w) + a2 * math.sin(2 * w)
            db += 10 * math.log10((nr * nr + ni * ni) / (dr * dr + di * di))
        assert abs(db - 20 * math.log10(abs(af.response(secs, f)))) < 0.05  # 8.24 truncation: about 0.01 dB near the 30 Hz high pass


def test_bad_filter_file_lists_errors(url):
    status, body = call(url, '/matrix/filterfile?ch=out&idx=0&preset=0', 'aurora-filter 1\nfs 44100\n')
    assert status == 400
    assert json.loads(body)['errors']


def test_preset_switch_keeps_edits(url):
    call(url, '/matrix', {'ch': 'out', 'idx': 6, 'name': 'Sub', 'preset': 0})
    assert call(url, '/preset', {'pre': 2})[0] == 200
    s = state(url)
    assert s['preset'] == 2 and s['outs'][6]['name'] == 'Output 7'
    call(url, '/preset', {'pre': 0})
    assert state(url)['outs'][6]['name'] == 'Sub'
    assert json.loads(call(url, '/preset')[1])['used'][2] is True


def test_preset_export_imports_back(url):
    exported = call(url, '/preset.param')[1]
    call(url, '/preset', {'pre': 3})
    status, body = call(url, '/matrix/import?preset=3', exported)
    assert status == 200
    assert json.loads(body)['presetname'] == json.loads(exported).get('name', '')
