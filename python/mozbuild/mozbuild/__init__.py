# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import os
import sys

# GeckoW10m: the objdir may be on another drive than the source tree (a VRAM
# disk at R:, the source on C:). The build runs in the objdir, and the many
# os.path.relpath calls that make a source path relative -- to the current
# directory or to the objdir -- raise ValueError across drives on Windows.
# There is no relative path between two drives; the absolute one names the
# same file, so that is what they get. Everything that imports mozbuild, mach
# and the build's Python actions included, is covered.
if sys.platform == "win32" and not getattr(os.path, "_gecko_w10m_relpath", None):
    _relpath = os.path.relpath

    def _gecko_w10m_relpath(path, start=None):
        try:
            return _relpath(path, start)
        except ValueError:
            if os.path.splitdrive(os.path.abspath(path))[0].lower() == (
                os.path.splitdrive(os.path.abspath(start or os.curdir))[0].lower()
            ):
                raise
            return os.path.abspath(path)

    os.path.relpath = _gecko_w10m_relpath
    os.path._gecko_w10m_relpath = _relpath
