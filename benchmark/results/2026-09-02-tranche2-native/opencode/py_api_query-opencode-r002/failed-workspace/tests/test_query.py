import unittest
from web.query import parse_query

class QueryTests(unittest.TestCase):
    def test_contract(self):
        self.assertEqual(parse_query("a=1&a=2&empty="), {"a": "2", "empty": ""})
        with self.assertRaises(ValueError):
            parse_query("bad=%ZZ")
