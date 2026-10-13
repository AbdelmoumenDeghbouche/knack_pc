"""The menu entry that gives the launcher and the game window their icon on Linux."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import gow3_launcher_win as launcher  # noqa: E402


class DesktopIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.home = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.home, True)
        self.icon = self.home / 'source.png'
        self.icon.write_bytes(b'png one')
        self.data = self.home / 'data'
        self.entry = self.home / '.local' / 'share' / 'applications' / 'gow3.desktop'

    def integrate(self, enabled=True, appimage='/games/gow3.AppImage'):
        return launcher.integrate_desktop(enabled, appimage, self.icon, self.data, home=self.home)

    def test_entry_names_the_appimage_the_icon_and_the_window_class(self):
        self.assertEqual(self.integrate(), self.entry)
        text = self.entry.read_text()
        self.assertIn('Exec="/games/gow3.AppImage"', text)
        self.assertIn(f'Icon={self.data / "gow3.png"}', text)
        self.assertIn('StartupWMClass=gow3', text)
        self.assertEqual((self.data / 'gow3.png').read_bytes(), b'png one')

    def test_a_moved_appimage_or_a_new_icon_updates_the_entry(self):
        self.integrate()
        self.icon.write_bytes(b'png two')
        self.integrate(appimage='/elsewhere/gow3.AppImage')
        self.assertIn('Exec="/elsewhere/gow3.AppImage"', self.entry.read_text())
        self.assertEqual((self.data / 'gow3.png').read_bytes(), b'png two')

    def test_turning_it_off_removes_only_our_entry(self):
        self.integrate()
        self.integrate(enabled=False)
        self.assertFalse(self.entry.exists())
        self.entry.write_text('[Desktop Entry]\nName=made by hand\n')
        self.integrate(enabled=False)
        self.assertTrue(self.entry.exists())

    def test_an_entry_made_by_hand_is_left_alone(self):
        self.entry.parent.mkdir(parents=True)
        self.entry.write_text('[Desktop Entry]\nName=made by hand\n')
        self.assertIsNone(self.integrate())
        self.assertEqual(self.entry.read_text(), '[Desktop Entry]\nName=made by hand\n')

    def test_without_an_appimage_nothing_is_written(self):
        self.assertIsNone(self.integrate(appimage=None))
        self.assertFalse(self.entry.exists())


if __name__ == '__main__':
    unittest.main()
