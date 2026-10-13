"""The game's output goes to <user folder>/last_run.log from both launch paths."""
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import gow3_launcher_win as launcher  # noqa: E402


class RunLogTest(unittest.TestCase):
    def test_log_path_follows_the_saves_folder_setting(self):
        self.assertEqual(launcher.run_log_path({'user_dir': ''}),
                         launcher.DATA_DIR / 'user' / 'last_run.log')
        self.assertEqual(launcher.run_log_path({'user_dir': r'D:\saves'}),
                         Path(r'D:\saves') / 'last_run.log')
        self.assertEqual(launcher.run_log_path({}), launcher.DATA_DIR / 'user' / 'last_run.log')

    def test_open_run_log_creates_the_folder_and_truncates(self):
        with tempfile.TemporaryDirectory() as tmp:
            settings = {'user_dir': str(Path(tmp) / 'nested' / 'user')}
            with launcher.open_run_log(settings) as log:
                log.write('first\n')
            path = launcher.run_log_path(settings)
            self.assertEqual(path.read_text(encoding='utf-8'), 'first\n')
            with launcher.open_run_log(settings) as log:
                log.write('second\n')
            self.assertEqual(path.read_text(encoding='utf-8'), 'second\n')

    def test_open_run_log_failure_returns_a_null_log(self):
        # A read-only or invalid folder must not stop the game from starting.
        settings = {'user_dir': str(Path(tempfile.gettempdir()) / 'nul:\x00invalid')}
        with launcher.open_run_log(settings) as log:
            log.write('ignored\n')  # no exception


if __name__ == '__main__':
    unittest.main()
