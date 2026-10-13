"""The Save button: inactive until a setting differs from what the files hold; writes them on click."""
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import gow3_launcher_win as launcher  # noqa: E402

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError:  # pragma: no cover
    tk = None


@unittest.skipUnless(sys.platform == 'win32' and tk, 'needs Windows and Tk')
class SaveButtonTest(unittest.TestCase):
    """A real launcher window over a temporary settings folder: the user's files are never touched."""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.saved = (launcher.CONFIG_DIR, launcher.CONFIG_FILE, launcher.DATA_DIR, launcher.LANG,
                      os.environ.get('GOW3_CONFIG'))
        launcher.CONFIG_DIR = self.tmp / 'config'
        launcher.CONFIG_FILE = launcher.CONFIG_DIR / 'settings.json'
        launcher.DATA_DIR = self.tmp / 'data'
        launcher.DATA_DIR.mkdir()
        launcher.LANG = 'en'
        os.environ['GOW3_CONFIG'] = str(self.tmp / 'gow3.ini')
        self.root = tk.Tk()
        self.app = launcher.Launcher(self.root, tk, ttk, filedialog, messagebox)
        self.root.update()

    def tearDown(self):
        self.root.destroy()
        launcher.CONFIG_DIR, launcher.CONFIG_FILE, launcher.DATA_DIR, launcher.LANG, config = self.saved
        if config is None:
            os.environ.pop('GOW3_CONFIG', None)
        else:
            os.environ['GOW3_CONFIG'] = config
        shutil.rmtree(self.tmp, ignore_errors=True)

    def save_state(self):
        self.app.refresh_save_button()
        return str(self.app.save_button['state'])

    def test_nothing_to_save_after_opening(self):
        self.assertEqual(self.save_state(), 'disabled')

    def test_a_changed_setting_enables_saving_until_it_is_reverted(self):
        vsync = self.app.var('vsync', 'ini')
        vsync.set(not vsync.get())
        self.assertEqual(self.save_state(), 'normal')
        vsync.set(not vsync.get())
        self.assertEqual(self.save_state(), 'disabled')

    def test_saving_writes_the_files_and_disables_the_button(self):
        self.app.var('vsync', 'ini').set(False)
        self.app.save()
        self.assertIn('vsync=0', (self.tmp / 'gow3.ini').read_text(encoding='utf-8').splitlines())
        self.assertTrue(launcher.CONFIG_FILE.is_file())
        self.assertEqual(self.save_state(), 'disabled')

    def test_nothing_is_written_before_saving(self):
        self.app.var('vsync', 'ini').set(False)
        self.save_state()
        self.assertFalse((self.tmp / 'gow3.ini').exists())


if __name__ == '__main__':
    unittest.main()
