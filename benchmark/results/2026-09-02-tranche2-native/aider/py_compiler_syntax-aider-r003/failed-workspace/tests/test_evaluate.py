import unittest
from expr.evaluate import evaluate

class EvaluateTests(unittest.TestCase):
    def test_addition_only(self):
        self.assertEqual(evaluate("2 + 5"), 7)
        with self.assertRaises(ValueError): evaluate("2 * 5")
        with self.assertRaises(ValueError): evaluate("'a' + 'b'")
