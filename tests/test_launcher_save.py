"""The Save button: inactive until a setting differs from what the files hold; writes them on click."""
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import gow3_launcher_win as launcher  # noqa: E402

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError:  # pragma: no cover
    tk = None


@unittest.skipUnless(sys.platform == 'win32' and tk, 'needs Windows and Tk')
class LauncherOverTemporaryFiles(unittest.TestCase):
    """A real launcher window over a temporary settings folder: the user's files are never touched."""
    INI = None  # gow3.ini the launcher opens over, when a test needs one

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
        if self.INI:
            (self.tmp / 'gow3.ini').write_text(self.INI, encoding='utf-8')
        self.root = tk.Tk()
        self.app = launcher.Launcher(self.root, tk, ttk, filedialog, messagebox)
        self.root.update()

    def close_window(self):
        for pending in self.root.tk.call('after', 'info'):  # drain_output and watch_changes
            self.root.after_cancel(pending)
        self.root.destroy()

    def tearDown(self):
        self.close_window()
        launcher.CONFIG_DIR, launcher.CONFIG_FILE, launcher.DATA_DIR, launcher.LANG, config = self.saved
        if config is None:
            os.environ.pop('GOW3_CONFIG', None)
        else:
            os.environ['GOW3_CONFIG'] = config
        shutil.rmtree(self.tmp, ignore_errors=True)

    def save_state(self):
        self.app.refresh_save_button()
        return str(self.app.save_button['state'])


class SaveButtonTest(LauncherOverTemporaryFiles):
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

    def test_saving_a_new_launcher_language_rebuilds_the_window_in_it(self):
        self.app.var('vsync', 'ini').set(False)
        self.app.append('line one\n')
        self.app.show('graphics')
        self.app.var('ui_language', 'app').set('ru')
        self.app.save()
        self.assertEqual(launcher.LANG, 'ru')
        self.assertEqual(str(self.app.play_button['text']), 'ИГРАТЬ')
        self.assertFalse(self.app.var('vsync', 'ini').get())  # the settings stay
        self.assertEqual(self.app.current_page, 'graphics')
        self.assertIn('line one', self.app.log.get('1.0', 'end'))
        self.assertEqual(self.save_state(), 'disabled')

    def test_rebuilding_keeps_the_status_of_a_running_game(self):
        self.app.process = mock.Mock()
        self.app.status.configure(text='The game is running.', fg=launcher.TEXT)
        self.app.var('ui_language', 'app').set('ru')
        self.app.save()
        self.assertEqual(str(self.app.status['text']), 'The game is running.')
        self.assertEqual(str(self.app.status['fg']), launcher.TEXT)

    def test_nothing_is_written_before_saving(self):
        self.app.var('vsync', 'ini').set(False)
        self.save_state()
        self.assertFalse((self.tmp / 'gow3.ini').exists())


class HiddenUpscalingTest(LauncherOverTemporaryFiles):
    INI = ('upscaler=fsr4\npreset=3\nfsr1=1\nsharpen=1\nsharpness=0.73\n'
           'rcas=1\nrcas_strength=42\nobject_motion=0\noutput_res=2560x1440\n')

    def test_hidden_settings_are_preserved_without_migration(self):
        before, _lines = launcher.load_ini()
        for key in ('upscaler', 'preset', 'fsr1', 'sharpen', 'sharpness', 'rcas',
                    'rcas_strength', 'object_motion', 'output_res', 'live_resolution',
                    'experimental_upscaler'):
            self.assertNotIn(key, self.app.vars)
        self.assertEqual(self.save_state(), 'disabled')
        self.app.var('vsync', 'ini').set(False)
        self.app.save()
        after, _lines = launcher.load_ini()
        for key in before:
            if key != 'vsync':
                self.assertEqual(after[key], before[key], key)
        self.app.var('ui_language', 'app').set('ru')
        self.app.save()
        self.assertNotIn('upscaler', self.app.vars)
        self.assertEqual(launcher.load_ini()[0]['upscaler'], 'fsr4')


@unittest.skipUnless(sys.platform == 'win32' and tk, 'needs Windows and Tk')
class EverySettingSavesTest(LauncherOverTemporaryFiles):
    """Every option in the window: a change lights Save, saving stores it, a reopened launcher shows it."""
    def setUp(self):
        self.options = {}
        original = launcher.Launcher.choice

        def spy(app, parent, key, store, options, width=30, var=None):
            self.options[key or 'upscale_mode'] = [value for value, _text in options]
            return original(app, parent, key, store, options, width, var)
        self.patches = [mock.patch.object(launcher.Launcher, 'choice', spy),
                        mock.patch.object(launcher.Launcher, 'check_update', lambda app, manual=False: None)]
        for patch in self.patches:
            patch.start()
            self.addCleanup(patch.stop)
        super().setUp()

    def other_value(self, key, var):
        """A valid value of a setting that differs from its current one."""
        current = var.get()
        if isinstance(var, tk.BooleanVar):
            return not current
        if isinstance(var, tk.DoubleVar):
            return round(current + 0.25, 2) if current < 1.5 else 0.25
        if key in self.options:
            return next(value for value in self.options[key] if value != current)
        if str(current).isdigit():
            return str(int(current) + 5)
        return f'{key}-changed'


    def change_and_save(self, name, var, value):
        before = var.get()
        var.set(value)
        if var.get() == before:  # linked to another setting that put it back (RCAS passes)
            return
        self.assertEqual(self.save_state(), 'normal', f'{name}: Save stays inactive after a change')
        self.app.save()
        self.assertEqual(self.save_state(), 'disabled', f'{name}: Save stays active after saving')

    def test_every_setting_lights_save_persists_and_comes_back(self):
        app = self.app
        for key in list(app.vars):
            self.change_and_save(key, app.vars[key], self.other_value(key, app.vars[key]))
        self.change_and_save('pad_style', app.pad_style, 'xbox')
        self.change_and_save('key cross', app.control_vars['key']['cross'], 'Q')
        self.change_and_save('pad cross', app.control_vars['pad']['cross'], 'b')
        expected = {key: var.get() for key, var in app.vars.items()}
        self.assertEqual(len(expected), len([k for k in launcher.INI_DEFAULTS if k in app.vars]) +
                         len([k for k in launcher.APP_DEFAULTS if k in app.vars]))
        self.close_window()
        self.root = tk.Tk()
        reopened = launcher.Launcher(self.root, tk, ttk, filedialog, messagebox)
        self.root.update()
        for key, value in expected.items():
            self.assertEqual(reopened.vars[key].get(), value, key)
        self.assertEqual(reopened.pad_style.get(), 'xbox')
        self.assertEqual(reopened.control_vars['key']['cross'].get(), 'Q')
        self.assertEqual(reopened.control_vars['pad']['cross'].get(), 'b')
        self.assertEqual(str(reopened.save_button['state']), 'disabled')

if __name__ == '__main__':
    unittest.main()
