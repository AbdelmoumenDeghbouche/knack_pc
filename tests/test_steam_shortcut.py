from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import binascii
import shutil
import tempfile
import unittest
from pathlib import Path

import steam_shortcut as steam

try:
    from PIL import Image
except ImportError:  # the pictures need Pillow
    Image = None


class VdfTests(unittest.TestCase):
    def test_round_trip_keeps_strings_numbers_and_nested_maps(self):
        tree = {'shortcuts': {'0': {'appid': 0x80000001, 'AppName': 'A', 'tags': {'0': 'favorite'}, 'IsHidden': 0}}}
        data = steam.dump_vdf(tree)
        self.assertEqual(steam.parse_vdf(data), tree)
        self.assertEqual(steam.dump_vdf(steam.parse_vdf(data)), data)

    def test_empty_file_of_steam(self):
        # What Steam writes when there are no shortcuts.
        self.assertEqual(steam.parse_vdf(b'\x00shortcuts\x00\x08\x08'), {'shortcuts': {}})
        self.assertEqual(steam.dump_vdf({'shortcuts': {}}), b'\x00shortcuts\x00\x08\x08')

    def test_app_id_is_the_crc_of_exe_and_name_with_the_top_bit(self):
        exe = '"/home/user/God of War III.AppImage"'
        expected = binascii.crc32((exe + steam.APP_NAME).encode()) | 0x80000000
        self.assertEqual(steam.shortcut_appid(exe, steam.APP_NAME), expected)
        self.assertGreaterEqual(expected, 0x80000000)


@unittest.skipIf(Image is None, 'Pillow is needed for the pictures')
class AddToSteamTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, True)
        self.game = self.root / 'game'
        (self.game / 'sce_sys').mkdir(parents=True)
        Image.new('RGB', (1920, 1080), (90, 20, 20)).save(self.game / 'sce_sys' / 'pic1.png')
        self.icon = self.root / 'icon.png'
        Image.new('RGBA', (256, 256), (230, 180, 40, 255)).save(self.icon)
        self.steam = self.root / 'steam'
        for account in ('1111', '2222'):
            (self.steam / 'userdata' / account / 'config').mkdir(parents=True)
        (self.steam / 'userdata' / '0').mkdir()  # the anonymous account is skipped
        other = {'shortcuts': {'0': {'appid': 5, 'AppName': 'Other', 'Exe': '"/x"', 'tags': {'0': 'keep'}}}}
        (self.steam / 'userdata' / '2222' / 'config' / 'shortcuts.vdf').write_bytes(steam.dump_vdf(other))
        self.command = ('/home/user/gow3.AppImage', '--play', '/home/user')

    def add(self, **kwargs):
        return steam.add_to_steam(self.game, self.command, self.icon, steam_root=self.steam, running=False, **kwargs)

    def shortcuts(self, account):
        return steam.parse_vdf((self.steam / 'userdata' / account / 'config' / 'shortcuts.vdf').read_bytes())['shortcuts']

    def test_every_account_gets_the_shortcut_and_the_five_pictures(self):
        result = self.add()
        self.assertEqual((result['accounts'], result['updated']), (2, False))
        app_id = result['app_id']
        entry = next(iter(self.shortcuts('1111').values()))
        self.assertEqual((entry['AppName'], entry['Exe'], entry['LaunchOptions']),
                         (steam.APP_NAME, '"/home/user/gow3.AppImage"', '--play'))
        self.assertEqual(entry['appid'], app_id)
        grid = self.steam / 'userdata' / '1111' / 'config' / 'grid'
        sizes = {f'{app_id}p.png': steam.CAPSULE, f'{app_id}.png': steam.WIDE, f'{app_id}_hero.png': steam.HERO}
        for name, size in sizes.items():
            with Image.open(grid / name) as picture:
                self.assertEqual(picture.size, size, name)
        with Image.open(grid / f"{app_id}_logo.png") as logo:
            self.assertEqual(logo.mode, "RGBA")
        self.assertTrue((grid / f'{app_id}.json').is_file())
        self.assertEqual(entry['icon'], str(grid / f'{app_id}_icon.png'))

    def test_other_shortcuts_stay_and_a_backup_is_made(self):
        self.add()
        shortcuts = self.shortcuts('2222')
        self.assertEqual(shortcuts['0']['AppName'], 'Other')
        self.assertEqual(shortcuts['0']['tags'], {'0': 'keep'})
        self.assertEqual(shortcuts['1']['AppName'], steam.APP_NAME)
        self.assertTrue((self.steam / 'userdata' / '2222' / 'config' / 'shortcuts.vdf.gow3-backup').is_file())

    def test_adding_again_updates_the_same_shortcut(self):
        self.add()
        self.command = ('/home/user/gow3.AppImage', '--play', '/home/user')
        result = self.add()
        self.assertTrue(result['updated'])
        self.assertEqual(len([v for v in self.shortcuts('2222').values() if v['AppName'] == steam.APP_NAME]), 1)

    def test_steam_open_and_steam_missing_are_refused(self):
        with self.assertRaises(steam.SteamError) as caught:
            steam.add_to_steam(self.game, self.command, self.icon, steam_root=self.steam, running=True)
        self.assertEqual(caught.exception.code, 'running')
        with self.assertRaises(steam.SteamError) as caught:
            steam.add_to_steam(self.game, self.command, self.icon, steam_root=self.root / 'none', running=False)
        self.assertEqual(caught.exception.code, 'no_steam')
        self.assertFalse((self.steam / 'userdata' / '1111' / 'config' / 'shortcuts.vdf').exists())

    def test_a_game_folder_without_the_background_is_refused(self):
        (self.game / 'sce_sys' / 'pic1.png').unlink()
        with self.assertRaises(steam.SteamError) as caught:
            self.add()
        self.assertEqual(caught.exception.code, 'no_art')


if __name__ == '__main__':
    unittest.main()
