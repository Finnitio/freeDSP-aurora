"""Tests for makefullrelease.sh: run the release script on a scratch copy.

The scratch tree holds the parts of the repo the script reads, plus stand-ins
for the built firmware and the manuals. Nothing is published or pushed.

Run from anywhere: python -m pytest SOURCES/sigma2aurora/tests
"""
import filecmp
import os
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
S2A_DIR = os.path.dirname(HERE)
SOURCES = os.path.dirname(S2A_DIR)
REPO = os.path.dirname(SOURCES)
PLUGINS = ['8channels', 'HomeCinema71', '4FIRs', 'matrix']


def scratch_repo(root):
    """Copy what makefullrelease.sh reads into root and return root."""
    shutil.copy(os.path.join(REPO, 'makefullrelease.sh'), root)
    src = root / 'SOURCES'
    (src / 'sigma2aurora').mkdir(parents=True)
    shutil.copy(os.path.join(S2A_DIR, 'sigma2aurora.py'), src / 'sigma2aurora')
    for project in PLUGINS:
        shutil.copytree(os.path.join(SOURCES, 'SIGMASTUDIO', project),
                        src / 'SIGMASTUDIO' / project)
    web = src / 'WEBAPP'
    shutil.copytree(os.path.join(SOURCES, 'WEBAPP', 'css'), web / 'css')
    shutil.copytree(os.path.join(SOURCES, 'WEBAPP', 'plugins'), web / 'plugins')
    (web / 'js').mkdir()
    shutil.copy(os.path.join(SOURCES, 'WEBAPP', 'js', 'aurora.js'), web / 'js')
    firmware = web / 'ESP32' / 'aurora'
    firmware.mkdir(parents=True)
    (firmware / 'aurora.ino.esp32.bin').write_bytes(b'firmware')
    (src / 'updater').mkdir()
    (src / 'updater' / 'update.sh').write_text('')
    (root / 'DOCUMENTATION').mkdir()
    (root / 'DOCUMENTATION' / 'UserManual_EN_test.pdf').write_bytes(b'')
    return root


def release(root, version='v9.9.9'):
    return subprocess.run(['bash', 'makefullrelease.sh', version], cwd=root,
                          capture_output=True, text=True)


def convert_by_hand(root, project, outputdir):
    """Run the converter the way the user would for a matrix plugin."""
    subprocess.run([sys.executable, '-I', 'sigma2aurora.py',
                    '../SIGMASTUDIO/%s/%s.xml' % (project, project), project,
                    '--version', 'v2.2.2', '--outputdir', outputdir],
                   cwd=root / 'SOURCES' / 'sigma2aurora', check=True,
                   capture_output=True)
    return root / 'SOURCES' / 'sigma2aurora' / outputdir / project


@pytest.fixture(scope='module')
def released(tmp_path_factory):
    root = scratch_repo(tmp_path_factory.mktemp('release'))
    result = release(root)
    assert result.returncode == 0, result.stdout + result.stderr
    return root, root / 'v9.9.9' / 'plugins'


def test_release_contains_all_plugins(released):
    _, plugins = released
    assert sorted(os.listdir(plugins)) == sorted(PLUGINS)


def test_release_matrix_plugin_matches_hand_conversion(released):
    root, plugins = released
    by_hand = convert_by_hand(root, 'matrix', 'byhand')
    cmp = filecmp.dircmp(plugins / 'matrix', by_hand)
    assert sorted(os.listdir(plugins / 'matrix')) == sorted(os.listdir(by_hand))
    assert cmp.diff_files == [] and cmp.funny_files == []
    match, mismatch, errors = filecmp.cmpfiles(
        plugins / 'matrix', by_hand, os.listdir(by_hand), shallow=False)
    assert mismatch == [] and errors == []


@pytest.mark.parametrize('project', PLUGINS)
def test_release_fails_when_a_conversion_fails(tmp_path, project):
    root = scratch_repo(tmp_path)
    os.remove(root / 'SOURCES' / 'SIGMASTUDIO' / project / 'TxBuffer_IC_1.dat')
    result = release(root)
    assert result.returncode != 0
    assert project in result.stdout + result.stderr
