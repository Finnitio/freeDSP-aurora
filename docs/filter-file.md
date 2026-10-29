# Filter file, version 1

A Filter file describes the Filter row of one Channel (a Bus or an Output of the matrix
Plugin): its filters, gain, delay, polarity and name. External design software writes it, and
Aurora imports it into one Channel.

The reference implementation is `SOURCES/filterfile/aurorafilter.py`
(`python aurorafilter.py check FILE`). The test vectors in `SOURCES/filterfile/vectors.json`
pin the coefficients, and `SOURCES/filterfile/example.txt` is a complete example. If this
document and the reference implementation disagree, that is a bug in one of them.

## Principle

Each filter block carries its **parameters** and the **biquad coefficients** the design software
computed for it. Aurora keeps only the parameters, as ordinary Slots: an LR48 shows up as one
LR48 Slot. Aurora computes the coefficients itself with the formulas below and compares them
with the file's (see [Verification](#verification)). The import is rejected if any block
deviates. This keeps the parameters shown in the web GUI identical to what the design software
calculated.

## Import

- The file **replaces** the Channel's whole Filter row: all Slots, gain, delay and polarity.
  A missing `gain`, `delay` or `polarity` line resets that value to 0 dB, 0 ms or normal.
  Gain and delay bypass are switched off. A missing `name` line keeps the current name.
- **All or nothing.** If the file has any error, nothing changes. Aurora lists every error with
  its line number, up to 20.
- The import is one change to the active Preset, like a manual edit: it goes through the
  Channel's Link group, Autosave stores it, and it conflicts if a Preset switch is in progress.
  There is no undo.
- The blocks take the Channel's biquads in file order. They must fit the Channel's Biquad budget.

## Syntax

- The file is UTF-8 text, at most 16384 bytes. A leading BOM is ignored. Lines end in LF or CRLF.
- Blank lines, and lines whose first non-blank character is `#`, are ignored. There are no
  comments at the end of a line.
- Tokens are separated by spaces or tabs. Keywords, block names, design names and parameter
  names are case-insensitive.
- Numbers are decimal with `.` as separator and an optional exponent (`-3`, `2.5`, `1.25e-3`).
  `nan` and `inf` are not numbers.

The first line must be `aurora-filter 1`. The next must be `fs 48000`, and any other sample rate
is rejected. After that come, in any order:

| Line | Meaning | Range |
|---|---|---|
| `name <text>` | Channel name: the rest of the line, trimmed | 1–16 bytes UTF-8, no control characters |
| `gain <number> dB` | Channel gain | −80 to +12 dB |
| `delay <number> ms` | Delay, rounded to whole samples (`floor(ms · 48 + 0.5)`); **Outputs only** | 0 to 40 ms, or the Plugin's smaller limit |
| `polarity normal\|inverted` | Polarity; **Outputs only** | |
| a filter block | see below | |

The `name`, `gain`, `delay` and `polarity` lines may each appear at most once. On a Bus,
`delay` and `polarity` are errors.

## Filter blocks

A block line is followed directly by its coefficient lines, one per section. A coefficient
line has five numbers, `b0 b1 b2 a1 a2`, and indenting it is optional. Any line that starts
with a number is a coefficient line.

```
<BLOCK> [<design>] <param>=<value> ... [bypass]
  b0 b1 b2 a1 a2
  ...
```

| Block | Parameters | Sections |
|---|---|---|
| `PEQ` | `f` `gain` `Q` | 1 |
| `LSHELF`, `HSHELF` | `f` `gain` `S` | 1 |
| `PHASE` (2nd-order allpass) | `f` `Q` | 1 |
| `HP`, `LP` with design `BW6` `BW12` `BW18` `BW24` `BW30` `BW36` `BW42` `BW48` (Butterworth, order = dB/oct ÷ 6) | `f` | ceil(order/2) |
| `HP`, `LP` with design `LR12` `LR24` `LR48` (Linkwitz-Riley) | `f` | order/2 |

Ranges: `f` 20–20000 Hz, `Q` 0.1–20, `gain` −24 to +24 dB, `S` 0.1–1.
Each parameter is required and appears once. `bypass` imports the Slot as bypassed, but its
coefficients are still verified.

Nothing else is accepted, for example LR36, Bessel, a notch, a 1st-order allpass, HP/LP with a
free Q, or raw biquads without a block. Such a line is an error.

## Coefficients

A section is normalized to a0 = 1 and uses the RBJ sign convention:

    H(z) = (b0 + b1·z⁻¹ + b2·z⁻²) / (1 + a1·z⁻¹ + a2·z⁻²)
    y[n] = b0·x[n] + b1·x[n−1] + b2·x[n−2] − a1·y[n−1] − a2·y[n−2]

Write at least 10 significant digits; `%.17g` is recommended. Compute in double precision.
Rounding to single precision is already enough to fail the check for filters near 20 Hz.

### Formulas

fs = 48000, w0 = 2π·f/fs, c = cos w0, A = 10^(gain/40). All sections are divided by their a0.

| Block | alpha | b0 | b1 | b2 | a0 | a1 | a2 |
|---|---|---|---|---|---|---|---|
| PEQ | sin w0 / (2Q) | 1 + alpha·A | −2c | 1 − alpha·A | 1 + alpha/A | −2c | 1 − alpha/A |
| PHASE | sin w0 / (2Q) | 1 − alpha | −2c | 1 + alpha | 1 + alpha | −2c | 1 − alpha |
| 2nd-order LP (quality q) | sin w0 / (2q) | (1 − c)/2 | 1 − c | (1 − c)/2 | 1 + alpha | −2c | 1 − alpha |
| 2nd-order HP (quality q) | sin w0 / (2q) | (1 + c)/2 | −(1 + c) | (1 + c)/2 | 1 + alpha | −2c | 1 − alpha |

Shelves (RBJ cookbook, slope S), with alpha = sin w0 / 2 · √((A + 1/A)(1/S − 1) + 2) and
r = 2√A·alpha:

| | b0 | b1 | b2 | a0 | a1 | a2 |
|---|---|---|---|---|---|---|
| LSHELF | A((A+1) − (A−1)c + r) | 2A((A−1) − (A+1)c) | A((A+1) − (A−1)c − r) | (A+1) + (A−1)c + r | −2((A−1) + (A+1)c) | (A+1) + (A−1)c − r |
| HSHELF | A((A+1) + (A−1)c + r) | −2A((A−1) + (A+1)c) | A((A+1) + (A−1)c − r) | (A+1) − (A−1)c + r | 2((A−1) − (A+1)c) | (A+1) − (A−1)c − r |

1st-order section (bilinear with prewarping), K = tan(π·f/fs), with b2 = a2 = 0:
LP b0 = b1 = K, HP b0 = 1 and b1 = −1. For both, a0 = 1 + K and a1 = K − 1.

**Butterworth of order n:** if n is odd, a 1st-order section first. Then, for k = n/2 down
to 1 (integer division), a 2nd-order section with q = 1 / (2·sin((2k − 1)·π / (2n))), so the
lowest Q comes first.

**Linkwitz-Riley:** LR12 is one 2nd-order section with q = 0.5. LR24 and LR48 are the
Butterworth of half the order twice (LR24 = BW12 + BW12, LR48 = BW24 + BW24).

The section order is how Aurora builds a block. A file may list a block's sections in any order,
because only the cascade is verified.

## Verification

For each block, Aurora computes the reference sections from the parameters and compares the
complex response of the cascaded file sections, H_file, with that of the reference cascade,
H_ref. The comparison runs on the frequencies f_k = 10 Hz · 2^(k/24) for k = 0, 1, … while
f_k < 24000 Hz.

- Where |H_ref| ≥ −80 dB: |20·log10|H_file/H_ref|| ≤ **0.01 dB** and |arg(H_file/H_ref)| ≤ **0.1°**.
- Where |H_ref| < −80 dB: |H_file| must stay below −79.99 dB. The phase is not compared.

A failure names the block's line and the worst point, for example
`line 9: PEQ deviates from the reference by 0.895 dB / 0.00359 deg at 12177 Hz`.

## Example

```
aurora-filter 1
fs 48000
name Woofer L
gain -3.0 dB
delay 1.25 ms
polarity inverted
LP LR48 f=2500
  0.020459153085372186 0.040918306170744372 0.020459153085372186 -1.4602175054265478 0.54205411776803658
  0.023628414438915203 0.047256828877830406 0.023628414438915203 -1.6864150849844293 0.78092874274009016
  0.020459153085372186 0.040918306170744372 0.020459153085372186 -1.4602175054265478 0.54205411776803658
  0.023628414438915203 0.047256828877830406 0.023628414438915203 -1.6864150849844293 0.78092874274009016
PEQ f=120 gain=-4 Q=2.5
  0.9985462343948549 -1.9918756592939013 0.99357518797515521 -1.9918756592939013 0.99212142237001
```

## Test vectors

`vectors.json` holds one block per line, as `{"block": "<block line>", "sections": [[b0, b1, b2, a1, a2], …]}`.
It covers every block type and design at the range limits. Design software should reproduce
each entry, within the tolerance above, from its block line. `python aurorafilter.py vectors`
regenerates the file, and the host tests (`SOURCES/WEBAPP/ESP32/test/run.sh`) fail if it is out
of date.

`cases.json` holds whole files with the outcome of the check: either the imported row or the list
of errors, word for word. Aurora's parser must give the same outcome for each, and design software
can use the rejected ones to see the messages. `python filecases.py > cases.json` regenerates it
from `filecases.py`, and the host tests fail if it is out of date.
