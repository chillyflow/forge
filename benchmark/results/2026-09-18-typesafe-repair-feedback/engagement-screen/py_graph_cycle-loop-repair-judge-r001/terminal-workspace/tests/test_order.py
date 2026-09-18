import unittest
from graph.order import topological_order

class OrderTests(unittest.TestCase):
    def test_dependencies_and_cycles(self):
        got=topological_order({"build":["compile"],"compile":["generate"],"test":["compile"]})
        self.assertEqual(set(got),{"build","compile","generate","test"})
        self.assertLess(got.index("generate"),got.index("compile")); self.assertLess(got.index("compile"),got.index("build"))
        with self.assertRaises(ValueError): topological_order({"a":["b"],"b":["a"]})
