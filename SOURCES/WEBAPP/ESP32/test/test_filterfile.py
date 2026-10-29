"""Tests for the Filter file reference implementation (SOURCES/filterfile, docs/filter-file.md).

Run: SOURCES/WEBAPP/ESP32/test/run.sh
"""
import json
import math
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
FILTERFILE = os.path.join(HERE, '..', '..', '..', 'filterfile')
sys.path.insert(0, FILTERFILE)

import aurorafilter as af  # noqa: E402


def db(secs, f):
    return 20 * math.log10(abs(af.response(secs, f)))


def deg(secs, f):
    return math.degrees(math.atan2(af.response(secs, f).imag, af.response(secs, f).real))


def example(*blocks, **kw):
    return af.write([(t, d, p, False) for t, d, p in blocks], **kw)


def errors(text, **kw):
    with pytest.raises(af.FilterFileError) as e:
        af.parse(text, **kw)
    return e.value.errors


# --- the formulas ------------------------------------------------------------

def test_the_committed_vectors_match_the_reference():
    with open(os.path.join(FILTERFILE, 'vectors.json')) as fh:
        committed = fh.read()
    assert committed == af.dump_vectors(af.vectors()), 'regenerate: python aurorafilter.py vectors > vectors.json'
    assert json.loads(committed)['vectors']


@pytest.mark.parametrize('f,gain,q', [(100, 6, 1), (1000, -12, 4), (10000, 6, 2), (18000, -3, 0.5)])
def test_peq_has_its_gain_at_the_centre_and_none_far_away(f, gain, q):
    s = [af.peq(f, gain, q)]
    assert db(s, f) == pytest.approx(gain, abs=1e-9)
    assert abs(db(s, 1)) < 0.01


def test_peq_uses_the_cookbook_q_not_the_bandwidth_form():
    w0 = 2 * math.pi * 10000 / af.FS
    alpha = math.sin(w0) / (2 * 2.0)
    b0, b1, b2, a1, a2 = af.peq(10000, 6, 2)
    A = 10 ** (6 / 40)
    assert b0 == pytest.approx((1 + alpha * A) / (1 + alpha / A), rel=1e-15)


@pytest.mark.parametrize('high', [False, True])
def test_shelves_reach_half_their_gain_at_f_and_the_full_gain_on_their_side(high):
    s = [(af.high_shelf if high else af.low_shelf)(1000, 12, 1)]
    assert db(s, 1000) == pytest.approx(6, abs=1e-9)
    assert db(s, 23000 if high else 5) == pytest.approx(12, abs=0.05)
    assert abs(db(s, 5 if high else 23000)) < 0.05


def test_phase_filter_is_flat_with_minus_180_degrees_at_f():
    s = [af.allpass(800, 0.7)]
    for f in af.grid():
        assert abs(db(s, f)) < 1e-9
    assert abs(abs(deg(s, 800)) - 180) < 1e-6


@pytest.mark.parametrize('design', list(af.BUTTERWORTH))
@pytest.mark.parametrize('high', [False, True])
def test_butterworth_matches_the_bilinear_closed_form(design, high):
    n, fc = af.BUTTERWORTH[design], 1000
    s = af.butterworth(high, n, fc)
    assert len(s) == (n + 1) // 2
    for f in (50, 300, 1000, 3000, 15000):
        t = math.tan(math.pi * f / af.FS) / math.tan(math.pi * fc / af.FS)
        t = 1 / t if high else t
        assert db(s, f) == pytest.approx(-10 * math.log10(1 + t ** (2 * n)), abs=1e-9)


@pytest.mark.parametrize('design', list(af.LINKWITZ_RILEY))
@pytest.mark.parametrize('high', [False, True])
def test_linkwitz_riley_is_minus_6_db_at_f_and_sums_flat(design, high):
    order = af.LINKWITZ_RILEY[design]
    lp, hp = af.linkwitz_riley(False, order, 2500), af.linkwitz_riley(True, order, 2500)
    assert len(lp) == order // 2
    assert db(hp if high else lp, 2500) == pytest.approx(-6.0206, abs=1e-3)
    for f in (100, 1000, 2500, 6000, 15000):
        # LR2n: |LP + HP| (LR24, LR48) or |LP - HP| (LR12) is 1 everywhere
        s = af.response(lp, f) + (af.response(hp, f) if order % 4 == 0 else -af.response(hp, f))
        assert abs(s) == pytest.approx(1, abs=1e-9)


def test_lr48_is_the_bw24_twice():
    bw = af.butterworth(False, 4, 2500)
    assert af.linkwitz_riley(False, 8, 2500) == bw + bw


# --- the check -----------------------------------------------------------------

def test_a_written_file_parses_back():
    text = example(('LP', 'LR48', {'f': 2500}), ('PEQ', None, {'f': 120, 'gain': -4, 'Q': 2.5}),
                   ('HSHELF', None, {'f': 8000, 'gain': -2, 'S': 0.7}), ('PHASE', None, {'f': 800, 'Q': 0.7}),
                   name='Woofer L', gain=-3, delay_ms=1.25, polarity='inverted')
    r = af.parse(text, biquads=7)
    assert (r['name'], r['gain'], r['delay'], r['polarity']) == ('Woofer L', -3.0, 60, 'inverted')
    assert [(b['type'], b['design']) for b in r['blocks']] == [('LP', 'LR48'), ('PEQ', None), ('HSHELF', None),
                                                               ('PHASE', None)]
    assert r['blocks'][1]['params'] == {'f': 120, 'gain': -4, 'q': 2.5}


def test_the_example_file_is_valid():
    with open(os.path.join(FILTERFILE, 'example.txt'), 'rb') as fh:
        af.parse(fh.read())


def test_missing_lines_are_neutral_and_the_name_is_absent():
    r = af.parse('aurora-filter 1\nfs 48000\n')
    assert (r['name'], r['gain'], r['delay'], r['polarity'], r['blocks']) == (None, 0.0, 0, 'normal', [])


def test_comments_blank_lines_crlf_bom_and_case_are_accepted():
    body = example(('PEQ', None, {'f': 1000, 'gain': 3, 'Q': 1})).split('\n')[2:]
    text = '\ufeff# from my tool\r\nAURORA-FILTER 1\r\n\r\nFS 48000\r\n  # PEQ follows\r\n' + '\r\n'.join(body)
    r = af.parse(text.replace('PEQ f=', 'peq F=').replace('Q=', 'q='))
    assert r['blocks'][0]['type'] == 'PEQ'


def test_coefficients_with_10_significant_digits_pass():
    text = example(('HP', 'LR48', {'f': 20}))
    lines = [' '.join('%.10g' % float(v) for v in l.split()) if l.startswith('  ') else l for l in text.split('\n')]
    af.parse('\n'.join(lines))


def test_section_order_inside_a_block_does_not_matter():
    text = example(('HP', 'BW36', {'f': 50})).split('\n')
    head, secs = text[:3], [l for l in text[3:] if l]
    af.parse('\n'.join(head + secs[::-1]) + '\n')


def test_aurora_s_old_bandwidth_peq_is_rejected():
    f, gain, q = 10000, 6, 2
    A = 10 ** (gain / 40)
    w0 = 2 * math.pi * f / af.FS
    bw = math.asinh(1 / q / 2) / (math.log(2) / 2)
    alpha = math.sin(w0) * math.sinh(math.log(2) / 2 * bw * w0 / math.sin(w0))
    c = math.cos(w0)
    s = af._norm(1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A)
    text = 'aurora-filter 1\nfs 48000\nPEQ f=10000 gain=6 Q=2\n  %s\n' % ' '.join('%.17g' % v for v in s)
    (e,) = errors(text)
    assert e == 'line 3: PEQ deviates from the reference by 0.895 dB / 0.00359 deg at 12177 Hz'


def test_a_slightly_wrong_frequency_is_rejected_a_tiny_one_passes():
    good = example(('LP', 'LR24', {'f': 2500}))
    af.parse(good.replace('f=2500', 'f=2500.0001'))
    (e,) = errors(good.replace('f=2500', 'f=2510'))
    assert e.startswith('line 3: LP LR24 deviates')


@pytest.mark.parametrize('line,msg', [
    ('LP LR36 f=1000', 'LP: unknown design "LR36" (BW6, BW12, BW18, BW24, BW30, BW36, BW42, BW48, LR12, LR24, LR48)'),
    ('HP', 'HP needs a design (BW6, BW12, BW18, BW24, BW30, BW36, BW42, BW48, LR12, LR24, LR48)'),
    ('BESSEL f=1000', 'unknown line "BESSEL"'),
    ('NOTCH f=1000 Q=5', 'unknown line "NOTCH"'),
    ('HP BW12 f=1000 Q=0.9', 'HP BW12 does not take "Q=0.9"'),
    ('PEQ f=10 gain=3 Q=1', 'f 10 outside 20 to 20000'),
    ('PEQ f=1000 gain=30 Q=1', 'gain 30 outside -24 to 24'),
    ('PEQ f=1000 gain=3 Q=25', 'Q 25 outside 0.1 to 20'),
    ('LSHELF f=1000 gain=3 S=2', 'S 2 outside 0.1 to 1'),
    ('PEQ f=1000 gain=3', 'PEQ needs Q'),
    ('PEQ f=1k gain=3 Q=1', 'f is not a number'),
    ('PEQ f=1000 f=900 gain=3 Q=1', 'f given twice'),
    ('gain -90 dB', 'gain -90 dB outside -80 to 12 dB'),
    ('gain -3', 'gain is written "gain <number> dB"'),
    ('delay 41 ms', 'delay 41 ms outside 0 to 40 ms'),
    ('delay 1 s', 'delay is written "delay <number> ms"'),
    ('polarity reversed', 'polarity is "normal" or "inverted"'),
    ('name ' + 'x' * 17, 'name longer than 16 bytes'),
    ('name ' + 'ä' * 9, 'name longer than 16 bytes'),
    ('1 0 0 0 0', 'coefficients without a filter block'),
])
def test_invalid_lines_are_rejected_with_their_line_number(line, msg):
    assert errors('aurora-filter 1\nfs 48000\n' + line + '\n')[0] == 'line 3: ' + msg


def test_a_bus_has_no_delay_and_no_polarity():
    e = errors('aurora-filter 1\nfs 48000\ndelay 1 ms\npolarity inverted\n', bus=True)
    assert e == ['line 3: a Bus has no delay', 'line 4: a Bus has no polarity']


def test_the_plugin_delay_limit_applies():
    errors('aurora-filter 1\nfs 48000\ndelay 10 ms\n', max_delay_samples=240)
    assert af.parse('aurora-filter 1\nfs 48000\ndelay 5 ms\n', max_delay_samples=240)['delay'] == 240


def test_the_biquad_budget_applies():
    text = example(('LP', 'LR48', {'f': 2500}), ('HP', 'BW18', {'f': 30}))
    af.parse(text, biquads=6)
    assert errors(text, biquads=5) == ['the filters need 6 biquads, the Channel has 5']


def test_header_fs_and_coefficient_count_are_enforced():
    assert errors('fs 48000\n') == ['line 1: the file must start with "aurora-filter 1"']
    assert errors('aurora-filter 2\n') == ['line 1: unsupported version 2 (this is version 1)']
    assert errors('aurora-filter 1\nfs 44100\n') == ['line 2: fs must be 48000']
    assert errors('aurora-filter 1\nname x\n')[0] == 'line 2: "fs 48000" must come right after the header'
    text = example(('LP', 'LR48', {'f': 2500}))
    short = '\n'.join(text.split('\n')[:-2]) + '\n'
    assert errors(short) == ['line 3: LP LR48 needs 4 coefficient lines, found 3']
    assert errors(text.replace('\n  ', '\n  1 ', 1))[0] == 'line 4: a coefficient line has 5 numbers: b0 b1 b2 a1 a2'


def test_all_errors_are_listed_up_to_the_maximum():
    text = 'aurora-filter 1\nfs 48000\n' + 'PEQ f=1 gain=0 Q=1\n  1 0 0 0 0\n' * 30
    e = errors(text)
    assert len(e) == af.MAX_ERRORS and e[1].startswith('line 5: ')


def test_duplicate_channel_lines_are_rejected():
    assert errors('aurora-filter 1\nfs 48000\ngain 0 dB\ngain 1 dB\n') == ['line 4: gain given twice']


def test_too_large_or_non_utf8_files_are_rejected():
    assert errors(b'\xff\xfe') == ['file is not UTF-8']
    assert errors(b'#' * (af.MAX_FILE_BYTES + 1)) == ['file larger than %d bytes' % af.MAX_FILE_BYTES]


def test_bypass_is_kept_and_still_verified():
    text = example(('PEQ', None, {'f': 1000, 'gain': 3, 'Q': 1})).replace('Q=1', 'Q=1 bypass')
    assert af.parse(text)['blocks'][0]['bypass'] is True
    (e,) = errors(text.replace('gain=3', 'gain=4'))
    assert 'deviates' in e
