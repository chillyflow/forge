import unittest
from service import overlay
class Overlay(unittest.TestCase):
    def test_merge_and_delete(self):
        base = {'db': {'port': 1, 'host': 'local'}, 'tags': ['base'], 'remove': 1}
        changes = {'db': {'port': 2}, 'tags': ['new'], 'remove': None}
        result = overlay(base, changes)
        self.assertEqual(result, {'db': {'port': 2, 'host': 'local'}, 'tags': ['new']})
        result['tags'].append('later')
        result['db']['host'] = 'elsewhere'
        self.assertEqual(changes['tags'], ['new'])
        self.assertEqual(base['db']['host'], 'local')
    def test_container_replacement(self):
        changes = {'x': {'nested': []}}
        result = overlay({'x': 1}, changes)
        result['x']['nested'].append(3)
        self.assertEqual(changes['x']['nested'], [])
        self.assertEqual(overlay({'a': 2}, {'absent': None}), {'a': 2})
