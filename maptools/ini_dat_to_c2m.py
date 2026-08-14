#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# ini_dat_to_c2m.py
#
# Copyright (C) 2026 CulturesGameExtend & CulturesGameLocalization 贡献者
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software Foundation,
# either version 3 of the License, or (at your option) any later version.
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.
# You should have received a copy of the GNU General Public License along with this
# program. If not, see <https://www.gnu.org/licenses/>.
#
# ---------------------------------------------------------------------------
# Upstream / third-party attribution (original licenses retained):
#
#   * The map format libraries used here (map_data.MapData, supplements.Library,
#     and the cif<->ini codec in supplements.initialization) are taken from the
#     "Cultures2-dat-format" project and the "Mikulus6/Cultures-map-editor"
#     project (converters.py). Both are GPL-3.0 and derive from the 2010 XeNTaX
#     forums cif research. Their original copyright notices are preserved in the
#     corresponding source files.
#
#   * This script itself (assembling a standalone map.ini + map.dat into a single
#     .c2m while preserving the user's .ini configuration) is an addition by the
#     CulturesGameExtend / CulturesGameLocalization contributors and is licensed
#     under GPL-3.0-or-later.
#
# Why this script exists:
#   The upstream MapData.save() regenerates map.cif from a template and DISCARDS the
#   user's map.ini settings (only keeps mapsize). This script instead builds the c2m
#   container manually with Library(), injecting the user's OWN encrypted map.cif
#   (produced from map.ini) plus the raw map.dat, so the editor opens the map with the
#   intended name / players / win-conditions intact.
# ---------------------------------------------------------------------------
#
# Usage:
#   python ini_dat_to_c2m.py <map_dir> [out.c2m]
#     <map_dir>  directory containing map.ini and map.dat
#     [out.c2m]  output path; default = <map_dir>.c2m
#
# Note: c2e and c2m share the SAME container format; rename the output to .c2e if your
# editor requires that extension. Terrain lives in the c2m/c2e container, NOT in the
# 20x20 .c2e project shell.

import sys
import os
import re

# --- Dependencies (GPL-3.0 upstream, kept intact) -------------------------
# Point these at your local copy of Cultures2-dat-format / Cultures-map-editor.
CULTURES2_DAT_FORMAT_DIR = r'G:\Projects\Cultures2-dat-format'
CIF2INI_SOURCE = r'G:\Projects\Cultures_Saga_CN\cif2ini.py'  # your own cif<->ini codec (MIT-friendly)
sys.path.insert(0, CULTURES2_DAT_FORMAT_DIR)
sys.path.insert(0, os.path.dirname(CIF2INI_SOURCE))

import cif2ini                       # from cif2ini.py
from supplements import Library       # from Cultures2-dat-format (GPL-3.0)


def convert(map_dir, out_path=None):
    ini_path = os.path.join(map_dir, 'map.ini')
    dat_path = os.path.join(map_dir, 'map.dat')
    for p in (ini_path, dat_path):
        if not os.path.isfile(p):
            raise SystemExit('missing file: %s' % p)

    if out_path is None:
        out_path = map_dir.rstrip('/\\') + '.c2m'

    # 1) map.ini -> map.cif (encrypt, preserving user config)
    cif_bytes = cif2ini.ini2cif_content(open(ini_path, 'rb').read(),
                                        cultures1=False, tab_file=False)

    # 2) map.dat read verbatim
    dat_bytes = open(dat_path, 'rb').read()

    # 3) pack into Library (c2m container) and save
    lib = Library()
    # key MUST use a single backslash, else MapData.load cannot find it
    lib[r'currentusermap\map.cif'] = cif_bytes
    lib[r'currentusermap\map.dat'] = dat_bytes
    lib.save(out_path, cultures_1=False)

    # 4) round-trip verification
    lib2 = Library()
    lib2.load(out_path, cultures_1=False)
    inner_cif = lib2[r'currentusermap\map.cif']
    dec = cif2ini.cif2ini_content(inner_cif, False)
    m = re.search(rb'mapsize\s+(\d+)\s+(\d+)', dec)
    size = (m.group(1).decode(), m.group(2).decode()) if m else ('?', '?')

    print('OK -> %s' % out_path)
    print('  map.ini config encrypted into map.cif (mapsize %sx%s)' % size)
    print('  map.dat %d bytes packed verbatim' % len(dat_bytes))
    return out_path


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
