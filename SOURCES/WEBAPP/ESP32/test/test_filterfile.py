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
import filecases  # noqa: E402


def db(secs, f):
    return 20 * math.log10(abs(af.response(secs, f)))


def deg(secs, f):
    return math.degrees(math.atan2(af.response(secs, f).imag, af.response(secs, f).real))


CASES = filecases.cases()


def case(name):
    """parse() of a shared case (filecases.py), which the firmware's parser runs too."""
    c = CASES[name]
    return af.parse(c['text'], **{k: c[k] for k in ('bus', 'biquads', 'max_delay_samples') if k in c})


def errors(name):
    with pytest.raises(af.FilterFileError) as e:
        case(name)
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
# The inputs are the shared cases of filecases.py; cases.json pins the reference's outcome of each
# for the firmware's parser (test_filterfileparser.cpp).

def test_the_committed_cases_match_the_reference():
    with open(os.path.join(FILTERFILE, 'cases.json'), encoding='utf-8') as fh:
        committed = fh.read()
    assert committed == filecases.dump(CASES), 'regenerate: python filecases.py > cases.json'


def test_a_written_file_parses_back():
    r = case('written_file')
    assert (r['name'], r['gain'], r['delay'], r['polarity']) == ('Woofer L', -3.0, 60, 'inverted')
    assert [(b['type'], b['design']) for b in r['blocks']] == [('LP', 'LR48'), ('PEQ', None), ('HSHELF', None),
                                                               ('PHASE', None)]
    assert r['blocks'][1]['params'] == {'f': 120, 'gain': -4, 'q': 2.5}


def test_the_example_file_is_valid():
    with open(os.path.join(FILTERFILE, 'example.txt'), 'rb') as fh:
        af.parse(fh.read())
    case('example_file')


def test_missing_lines_are_neutral_and_the_name_is_absent():
    r = case('header_only')
    assert (r['name'], r['gain'], r['delay'], r['polarity'], r['blocks']) == (None, 0.0, 0, 'normal', [])


def test_comments_blank_lines_crlf_bom_and_case_are_accepted():
    assert case('comments_crlf_bom_case')['blocks'][0]['type'] == 'PEQ'


def test_coefficients_with_10_significant_digits_pass():
    case('ten_significant_digits')


def test_section_order_inside_a_block_does_not_matter():
    case('sections_reversed')


def test_aurora_s_old_bandwidth_peq_is_rejected():
    (e,) = errors('old_bandwidth_peq')
    assert e == 'line 3: PEQ deviates from the reference by 0.895 dB / 0.00359 deg at 12177 Hz'


def test_a_slightly_wrong_frequency_is_rejected_a_tiny_one_passes():
    case('tiny_frequency_change')
    (e,) = errors('small_frequency_change')
    assert e.startswith('line 3: LP LR24 deviates')


@pytest.mark.parametrize('n', range(len(filecases.INVALID_LINES)))
def test_invalid_lines_are_rejected_with_their_line_number(n):
    assert errors('invalid_line_%d' % n)[0] == 'line 3: ' + filecases.INVALID_LINES[n][1]


def test_a_bus_has_no_delay_and_no_polarity():
    assert errors('bus_delay_polarity') == ['line 3: a Bus has no delay', 'line 4: a Bus has no polarity']


def test_the_plugin_delay_limit_applies():
    errors('plugin_delay_limit_exceeded')
    assert case('plugin_delay_limit_reached')['delay'] == 240


def test_the_biquad_budget_applies():
    case('budget_fits')
    assert errors('budget_exceeded') == ['the filters need 6 biquads, the Channel has 5']


def test_header_fs_and_coefficient_count_are_enforced():
    assert errors('no_header') == ['line 1: the file must start with "aurora-filter 1"']
    assert errors('version_2') == ['line 1: unsupported version 2 (this is version 1)']
    assert errors('fs_44100') == ['line 2: fs must be 48000']
    assert errors('fs_missing')[0] == 'line 2: "fs 48000" must come right after the header'
    assert errors('too_few_coefficient_lines') == ['line 3: LP LR48 needs 4 coefficient lines, found 3']
    assert errors('six_coefficients')[0] == 'line 4: a coefficient line has 5 numbers: b0 b1 b2 a1 a2'


def test_all_errors_are_listed_up_to_the_maximum():
    e = errors('too_many_errors')
    assert len(e) == af.MAX_ERRORS and e[1].startswith('line 5: ')


def test_duplicate_channel_lines_are_rejected():
    assert errors('gain_twice') == ['line 4: gain given twice']


def test_too_large_or_non_utf8_files_are_rejected():
    assert errors('not_utf8') == ['file is not UTF-8']
    assert errors('too_large') == ['file larger than %d bytes' % af.MAX_FILE_BYTES]


def test_bypass_is_kept_and_still_verified():
    assert case('bypass')['blocks'][0]['bypass'] is True
    (e,) = errors('bypass_deviates')
    assert 'deviates' in e
