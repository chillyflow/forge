import unittest
import sys
import os

# Add the expr directory to the path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'expr'))

from evaluate import evaluate

class TestEvaluate(unittest.TestCase):
    def test_valid_addition(self):
        self.assertEqual(evaluate("1 + 2"), 3)
        self.assertEqual(evaluate("5 + 10"), 15)
        
    def test_invalid_expressions(self):
        with self.assertRaises(ValueError):
            evaluate("1 - 2")
        with self.assertRaises(ValueError):
            evaluate("1 * 2")
        with self.assertRaises(ValueError):
            evaluate("1 / 2")
        with self.assertRaises(ValueError):
            evaluate("1 + 2 + 3")

if __name__ == '__main__':
    unittest.main()