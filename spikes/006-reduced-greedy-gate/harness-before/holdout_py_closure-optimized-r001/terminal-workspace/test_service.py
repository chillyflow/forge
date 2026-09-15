import unittest
from service import requirements
class Closure(unittest.TestCase):
    def test_diamond(self):
        graph = {'a': ['b', 'c'], 'b': ['d'], 'c': ['d'], 'd': [], 'unused': ['missing']}
        self.assertEqual(requirements(graph, ['a']), {'b', 'c', 'd'})
        self.assertEqual(requirements(graph, ['a', 'b']), {'c', 'd'})
        self.assertEqual(requirements(graph, []), set())
    def test_rejections(self):
        for graph in [{'a': ['b'], 'b': ['a']}, {'a': ['missing']}, {'a': ['a']}]:
            with self.assertRaises(ValueError):
                requirements(graph, ['a'])
