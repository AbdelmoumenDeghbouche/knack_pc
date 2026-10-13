"""Launcher setting for the game's multisampling (GOW3_NO_MSAA): environment, page and texts."""
import os
import sys
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import gow3_lang  # noqa: E402
import gow3_launcher_win as launcher  # noqa: E402


def added(env):
    return {key for key in env if key not in os.environ}


class NoMsaaEnvironmentTest(unittest.TestCase):
    def test_auto_leaves_the_game_default_and_keeps_an_inherited_value(self):
        self.assertEqual(launcher.APP_DEFAULTS['no_msaa'], '')
        self.assertNotIn('GOW3_NO_MSAA', added(launcher.game_environment(dict(launcher.APP_DEFAULTS))))
        # A GOW3_NO_MSAA set by a .bat before the launcher still reaches the game on Auto.
        with mock.patch.dict(os.environ, {'GOW3_NO_MSAA': '1'}):
            self.assertEqual(launcher.game_environment(dict(launcher.APP_DEFAULTS))['GOW3_NO_MSAA'], '1')

    def test_off_and_on_set_the_variable(self):
        for value in ('1', '0'):
            with self.subTest(value=value):
                env = launcher.game_environment({**launcher.APP_DEFAULTS, 'no_msaa': value})
                self.assertEqual(env['GOW3_NO_MSAA'], value)

    def test_choices_and_legacy_settings(self):
        self.assertEqual([value for value, _text in launcher.NO_MSAA], ['', '1', '0'])
        legacy = dict(launcher.APP_DEFAULTS)
        legacy.pop('no_msaa')
        self.assertNotIn('GOW3_NO_MSAA', added(launcher.game_environment(legacy)))

    def test_portuguese_texts(self):
        table = gow3_lang.table('pt')
        for text in ('Game multisampling',) + tuple(text[0] for _value, text in launcher.NO_MSAA):
            with self.subTest(text=text):
                self.assertTrue(table.get(text), f'no Portuguese text for {text!r}')


class NoMsaaLauncherTest(unittest.TestCase):
    def test_performance_page_offers_the_choice(self):
        try:
            import tkinter as tk
            from tkinter import ttk
            root = tk.Tk()
        except ImportError as error:
            self.skipTest(f'Tk unavailable: {error}')
        except tk.TclError as error:
            self.skipTest(f'Tk display unavailable: {error}')
        root.withdraw()
        self.addCleanup(root.destroy)
        with mock.patch.dict(os.environ, {'GOW3_CONFIG': os.devnull}):
            app = launcher.Launcher.__new__(launcher.Launcher)
            app.tk, app.ttk, app.root, app.dpi = tk, ttk, root, 1.0
            app.content, app.pages, app.vars = ttk.Frame(root), {}, {}
            app.app = dict(launcher.APP_DEFAULTS)
            app.ini, app.ini_lines = launcher.load_ini()
            app.control_vars, app.mod_order, app.patch_vars = {}, [], {}
            app.pad_style = tk.StringVar(root, value='playstation')
            app.build_performance()

        def widgets(parent):
            for child in parent.winfo_children():
                yield child
                yield from widgets(child)

        labels = tuple(launcher._(*text) for _value, text in launcher.NO_MSAA)
        boxes = [widget for widget in widgets(app.pages['performance'])
                 if isinstance(widget, ttk.Combobox) and tuple(widget['values']) == labels]
        self.assertEqual(len(boxes), 1)
        self.assertEqual(app.vars['no_msaa'].get(), '')
        boxes[0].current(1)
        boxes[0].event_generate('<<ComboboxSelected>>')
        self.assertEqual(app.vars['no_msaa'].get(), '1')

        # The Portuguese page shows the translated title and hint (the hint is one EXTRA key).
        table = gow3_lang.table('pt')
        hint = next(key for key in table if key.startswith("The game's 2-sample depth buffer"))
        with mock.patch.object(launcher, 'LANG', 'pt'), mock.patch.dict(os.environ, {'GOW3_CONFIG': os.devnull}):
            app.pages, app.vars = {}, {}
            app.build_performance()
        texts = {widget.cget('text') for widget in widgets(app.pages['performance'])
                 if isinstance(widget, ttk.Label)}
        self.assertIn(table[hint], texts)
        self.assertTrue(any(text.startswith(table['Game multisampling']) for text in texts))


if __name__ == '__main__':
    unittest.main()
