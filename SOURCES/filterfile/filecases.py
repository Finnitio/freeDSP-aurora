"""Accept/reject cases of the Filter file check (docs/filter-file.md), shared by the reference's
tests (SOURCES/WEBAPP/ESP32/test/test_filterfile.py) and the firmware's parser, which must give the
same result and the same error texts (test_filterfileparser.cpp reads cases.json).

Usage:
  python filecases.py   print the cases with the reference's outcome as JSON (cases.json)
"""
import json
import os
import sys

import aurorafilter as af

HERE = os.path.dirname(os.path.abspath(__file__))
HEAD = 'aurora-filter 1\nfs 48000\n'


def example(*blocks, **kw):
    """A file written by the reference; blocks are (type, design, params)."""
    return af.write([(t, d, p, False) for t, d, p in blocks], **kw)


def _old_bandwidth_peq():
    """Aurora's PEQ before the Filter file: alpha from the bandwidth, not the cookbook Q."""
    import math
    f, gain, q = 10000, 6, 2
    A = 10 ** (gain / 40)
    w0 = 2 * math.pi * f / af.FS
    bw = math.asinh(1 / q / 2) / (math.log(2) / 2)
    alpha = math.sin(w0) * math.sinh(math.log(2) / 2 * bw * w0 / math.sin(w0))
    c = math.cos(w0)
    s = af._norm(1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A)
    return HEAD + 'PEQ f=10000 gain=6 Q=2\n  %s\n' % ' '.join('%.17g' % v for v in s)


def _comments_crlf_bom_case():
    body = example(('PEQ', None, {'f': 1000, 'gain': 3, 'Q': 1})).split('\n')[2:]
    text = '\ufeff# from my tool\r\nAURORA-FILTER 1\r\n\r\nFS 48000\r\n  # PEQ follows\r\n' + '\r\n'.join(body)
    return text.replace('PEQ f=', 'peq F=').replace('Q=', 'q=')


def _ten_digits():
    text = example(('HP', 'LR48', {'f': 20}))
    return '\n'.join(' '.join('%.10g' % float(v) for v in l.split()) if l.startswith('  ') else l
                     for l in text.split('\n'))


def _sections_reversed():
    text = example(('HP', 'BW36', {'f': 50})).split('\n')
    head, secs = text[:3], [l for l in text[3:] if l]
    return '\n'.join(head + secs[::-1]) + '\n'


# test_invalid_lines_are_rejected_with_their_line_number: line after the header -> first error
INVALID_LINES = [
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
]


def cases():
    """name -> {'text': str or bytes, and the parse() options 'bus', 'biquads', 'max_delay_samples'}."""
    lr48 = example(('LP', 'LR48', {'f': 2500}))
    lr24 = example(('LP', 'LR24', {'f': 2500}))
    budget = example(('LP', 'LR48', {'f': 2500}), ('HP', 'BW18', {'f': 30}))
    bypass = example(('PEQ', None, {'f': 1000, 'gain': 3, 'Q': 1})).replace('Q=1', 'Q=1 bypass')
    with open(os.path.join(HERE, 'example.txt'), 'rb') as fh:
        example_file = fh.read().decode('utf-8')
    c = {
        'written_file': {'text': example(
            ('LP', 'LR48', {'f': 2500}), ('PEQ', None, {'f': 120, 'gain': -4, 'Q': 2.5}),
            ('HSHELF', None, {'f': 8000, 'gain': -2, 'S': 0.7}), ('PHASE', None, {'f': 800, 'Q': 0.7}),
            name='Woofer L', gain=-3, delay_ms=1.25, polarity='inverted'), 'biquads': 7},
        'example_file': {'text': example_file},
        'header_only': {'text': HEAD},
        'comments_crlf_bom_case': {'text': _comments_crlf_bom_case()},
        'ten_significant_digits': {'text': _ten_digits()},
        'sections_reversed': {'text': _sections_reversed()},
        'old_bandwidth_peq': {'text': _old_bandwidth_peq()},
        'tiny_frequency_change': {'text': lr24.replace('f=2500', 'f=2500.0001')},
        'small_frequency_change': {'text': lr24.replace('f=2500', 'f=2510')},
        'bus_delay_polarity': {'text': HEAD + 'delay 1 ms\npolarity inverted\n', 'bus': True},
        'plugin_delay_limit_exceeded': {'text': HEAD + 'delay 10 ms\n', 'max_delay_samples': 240},
        'plugin_delay_limit_reached': {'text': HEAD + 'delay 5 ms\n', 'max_delay_samples': 240},
        'budget_fits': {'text': budget, 'biquads': 6},
        'budget_exceeded': {'text': budget, 'biquads': 5},
        'no_header': {'text': 'fs 48000\n'},
        'version_2': {'text': 'aurora-filter 2\n'},
        'fs_44100': {'text': 'aurora-filter 1\nfs 44100\n'},
        'fs_missing': {'text': 'aurora-filter 1\nname x\n'},
        'too_few_coefficient_lines': {'text': '\n'.join(lr48.split('\n')[:-2]) + '\n'},
        'six_coefficients': {'text': lr48.replace('\n  ', '\n  1 ', 1)},
        'too_many_errors': {'text': HEAD + 'PEQ f=1 gain=0 Q=1\n  1 0 0 0 0\n' * 30},
        'gain_twice': {'text': HEAD + 'gain 0 dB\ngain 1 dB\n'},
        'not_utf8': {'text': b'\xff\xfe'},
        'too_large': {'text': b'#' * (af.MAX_FILE_BYTES + 1)},
        'bypass': {'text': bypass},
        'bypass_deviates': {'text': bypass.replace('gain=3', 'gain=4')},
    }
    for i, (line, _) in enumerate(INVALID_LINES):
        c['invalid_line_%d' % i] = {'text': HEAD + line + '\n'}
    # further corners of the syntax, for the firmware's parser only
    lr12 = example(('HP', 'LR12', {'f': 1000}))
    c.update({
        'empty_file': {'text': ''},
        'header_without_fs': {'text': 'aurora-filter 1\n'},
        'version_missing': {'text': '# x\naurora-filter\n'},
        'fs_twice': {'text': HEAD + 'fs 48000\n'},
        'fs_as_exponent': {'text': 'aurora-filter 1\nfs 4.8e4\n'},
        'coefficients_before_fs': {'text': 'aurora-filter 1\n  1 0 0 0 0\n'},
        'bypass_twice': {'text': bypass.replace('bypass', 'bypass BYPASS')},
        'nan_and_inf_are_not_numbers': {'text': HEAD + 'PEQ f=nan gain=inf Q=1\n  nan 0 0 0 0\n'},
        'name_empty': {'text': HEAD + 'name   \n'},
        'name_with_control_character': {'text': HEAD + 'name a\tb\n'},
        'name_utf8_16_bytes': {'text': HEAD + 'name ' + 'ä' * 8 + '\n'},
        'number_forms': {'text': HEAD + 'gain -3E0 db\ndelay .5 MS\npolarity NORMAL\n', 'max_delay_samples': 1920},
        'delay_rounding': {'text': HEAD + 'delay 0.0104 ms\n'},
        'negative_delay': {'text': HEAD + 'delay -0.01 ms\n'},
        'bus_gain_and_name': {'text': HEAD + 'name  Sub  \ngain 12 dB\n', 'bus': True},
        'lowercase_design_and_tabs': {'text': lr12.replace('HP LR12 f=1000', 'hp\tlr12\tF=1000')},
        'coefficients_after_unknown_design': {'text': lr12.replace('LR12', 'LR13')},
        'too_many_coefficient_lines': {'text': lr12 + lr12.split('\n')[3] + '\n'},
        'block_errors_before_deviations': {'text': lr12.replace('f=1000', 'f=1001') + 'NOTCH\n'},
        'header_with_trailing_token': {'text': 'aurora-filter 1 x\n'},
        'parameter_without_value': {'text': HEAD + 'PEQ f gain=1 Q=1\n  1 0 0 0 0\n'},
    })
    return c


def outcome(case):
    """The reference's result: {'errors': [...]} or {'result': {...}} without the sections."""
    kw = {k: case[k] for k in ('bus', 'biquads', 'max_delay_samples') if k in case}
    try:
        r = af.parse(case['text'], **kw)
    except af.FilterFileError as e:
        return {'errors': e.errors}
    for b in r['blocks']:
        del b['sections']
    return {'result': r}


def dump(c):
    """JSON with one case per line; a non-UTF-8 text is given as "hex"."""
    rows = []
    for name in sorted(c):
        case = dict(c[name])
        text = case.pop('text')
        if isinstance(text, bytes):
            try:
                case['text'] = text.decode('utf-8')
            except UnicodeDecodeError:
                case['hex'] = text.hex()
        else:
            case['text'] = text
        case.update(outcome(c[name]))
        # UTF-8 as is: ArduinoJson (the firmware's host test) does not decode \u escapes
        rows.append('  ' + json.dumps(dict(name=name, **case), sort_keys=True, ensure_ascii=False))
    return '{"cases": [\n%s\n]}\n' % ',\n'.join(rows)


if __name__ == '__main__':
    sys.stdout.buffer.write(dump(cases()).encode('utf-8'))
