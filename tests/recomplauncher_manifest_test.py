"""recomplauncher.json agrees with itself and with the installer's command line. Stdlib only.

Run: python tests/recomplauncher_manifest_test.py
"""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = json.loads((ROOT / 'recomplauncher.json').read_text(encoding='utf-8'))
CLI = (ROOT / 'installer' / 'cli.py').read_text(encoding='utf-8')


class ManifestTest(unittest.TestCase):
    def test_prepare_arguments_only_use_declared_inputs(self):
        declared = {i['id'] for i in MANIFEST['inputs']}
        used = set(re.findall(r'\{input:([^}]+)\}', ' '.join(MANIFEST['prepare']['args'])))
        self.assertEqual(sorted(used - declared), [])

    def test_prepare_options_exist_in_the_installer_command_line(self):
        options = [a.split('=')[0] for a in MANIFEST['prepare']['args'] if a.startswith('--')]
        for option in options:
            self.assertIn(f"'{option}'", CLI, option)

    def test_every_optional_input_is_passed_in_its_own_argument(self):
        for item in MANIFEST['inputs']:
            if item.get('optional'):
                token = '{input:%s}' % item['id']
                self.assertIn(token, [a.split('=', 1)[-1] for a in MANIFEST['prepare']['args']], item['id'])

    def test_every_setting_has_its_default_among_the_choices(self):
        for setting in MANIFEST['settings']:
            first = setting['choices'][0]['set']
            self.assertEqual(set(setting['defaults']), set(first), setting['id'])


if __name__ == '__main__':
    unittest.main()
