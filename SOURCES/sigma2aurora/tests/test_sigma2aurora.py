"""Tests for sigma2aurora: run the converter on committed SigmaStudio exports.

Run from anywhere: python -m pytest SOURCES/sigma2aurora/tests
"""
import json
import os
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
S2A_DIR = os.path.dirname(HERE)
SOURCES = os.path.dirname(S2A_DIR)
WEBAPP = os.path.join(SOURCES, 'WEBAPP')
SIGMASTUDIO = os.path.join(SOURCES, 'SIGMASTUDIO')


def convert(tmp_path, project):
    """Run sigma2aurora on a project in a scratch tree and return the plugin dir.

    The converter finds the web assets relative to its working directory and
    needs the (git-ignored, built) aurora.jgz, so the scratch tree mimics it.
    """
    web = tmp_path / 'WEBAPP'
    (web / 'js').mkdir(parents=True)
    (web / 'js' / 'aurora.jgz').write_bytes(b'')
    (web / 'css').mkdir()
    shutil.copy(os.path.join(WEBAPP, 'css', 'dark.css'), web / 'css')
    (web / 'plugins' / 'template').mkdir(parents=True)
    shutil.copy(os.path.join(WEBAPP, 'plugins', 'template', 'dsp.html'),
                web / 'plugins' / 'template')
    cwd = tmp_path / 'sigma2aurora'
    cwd.mkdir()
    shutil.copy(os.path.join(S2A_DIR, 'sigma2aurora.py'), cwd)
    out = tmp_path / 'out'
    xml = os.path.join(SIGMASTUDIO, project, project + '.xml')
    subprocess.run([sys.executable, '-I', 'sigma2aurora.py', xml, project,
                    '--version', 'v2.2.2', '--outputdir', str(out)],
                   cwd=cwd, check=True, capture_output=True)
    return out / project


def read_ini(plugin_dir):
    with open(plugin_dir / 'plugin.ini', encoding='utf8') as f:
        return json.load(f)


@pytest.fixture(scope='module')
def matrix(tmp_path_factory):
    plugin_dir = convert(tmp_path_factory.mktemp('matrix'), 'matrix')
    return plugin_dir, read_ini(plugin_dir)


def test_matrix_kind_and_counts(matrix):
    _, ini = matrix
    assert ini['kind'] == 'matrix'
    assert ini['nbus'] == 2
    assert ini['nout'] == 8
    assert ini['nbiquad'] == 30
    assert ini['maxdly'] == 1920


def test_matrix_biquad_first_coefficient_addresses(matrix):
    # Addresses from SigmaStudio's matrix_IC_1_PARAM.h (B2 of the stage).
    # Layout: Channel-major, 30 stages per Channel; part _2 follows part _1
    # but is not adjacent in memory.
    _, ini = matrix
    bus, out = ini['busbiquad'], ini['outbiquad']
    assert len(bus) == 2 * 30
    assert len(out) == 8 * 30
    assert bus[0] == 15548 and bus[1] == 15553   # Bus 1 stage 0, 1
    assert bus[15] == 15698                      # BusChain1_2 stage 0
    assert bus[30] != bus[0] and bus[45] == 15773  # Bus 2, BusChain2_2
    assert out[0] == 15904 and out[14] == 15974  # OutChain1_1
    assert out[15] == 16504                      # OutChain1_2
    assert out[8 * 30 - 15] == 16954             # OutChain8_2 stage 0
    assert None not in bus and None not in out


def test_matrix_per_channel_addresses(matrix):
    _, ini = matrix
    assert ini['busgain'] == [15543, 15546]
    assert ini['outgain'][0] == 15873 and len(ini['outgain']) == 8
    assert ini['outpol'] == list(range(17168, 17176))
    assert ini['outdly'] == list(range(15896, 15904))
    assert ini['outroute'] == list(range(17160, 17168))


def test_matrix_keeps_existing_addresses(matrix):
    _, ini = matrix
    assert ini['ninputs'] == 2
    assert ini['master'] == 17105
    assert len(ini['spdifout']) == 14


def test_matrix_has_no_generated_dsp_html(matrix):
    plugin_dir, _ = matrix
    assert not (plugin_dir / 'dsp.html').exists()
    assert (plugin_dir / 'dsp.fw').exists()


@pytest.mark.parametrize('project', ['8channels', '4FIRs', 'HomeCinema71'])
def test_existing_plugins_unchanged(tmp_path, project):
    plugin_dir = convert(tmp_path, project)
    with open(os.path.join(HERE, 'reference', project + '.plugin.ini'),
              encoding='utf8') as f:
        expected = f.read()
    with open(plugin_dir / 'plugin.ini', encoding='utf8') as f:
        assert f.read() == expected
