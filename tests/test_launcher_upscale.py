"""The Graphics page's Upscaler choice: one mode over two gow3.ini keys (upscaler and fsr1)."""
import sys
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import gow3_launcher_win as launcher  # noqa: E402


class UpscaleModeTest(unittest.TestCase):
    def test_choices_are_fsr3_taa_fsr1_off(self):
        self.assertEqual([value for value, _text in launcher.UPSCALERS], ['fsr3', 'taa', 'fsr1', 'off'])

    def test_default_upscaler_is_fsr3(self):
        self.assertEqual(launcher.INI_DEFAULTS['upscaler'], 'fsr3')

    def test_mode_of_the_ini_keys(self):
        self.assertEqual(launcher.upscale_mode('fsr3', '0'), 'fsr3')
        self.assertEqual(launcher.upscale_mode('taa', '0'), 'taa')
        self.assertEqual(launcher.upscale_mode('off', '0'), 'off')
        self.assertEqual(launcher.upscale_mode('off', '1'), 'fsr1')

    def test_a_temporal_upscaler_wins_over_fsr1(self):
        self.assertEqual(launcher.upscale_mode('fsr3', '1'), 'fsr3')

    def test_removed_upscalers_fall_back_to_fsr3(self):
        for old in ('fsr4', 'fsr411', 'dlss', ''):
            self.assertEqual(launcher.upscale_mode(old, '0'), 'fsr3', old)

    def test_ini_keys_of_a_mode(self):
        self.assertEqual(launcher.upscale_settings('fsr1'), ('off', '1'))
        self.assertEqual(launcher.upscale_settings('fsr3'), ('fsr3', '0'))
        self.assertEqual(launcher.upscale_settings('taa'), ('taa', '0'))
        self.assertEqual(launcher.upscale_settings('off'), ('off', '0'))


if __name__ == '__main__':
    unittest.main()
