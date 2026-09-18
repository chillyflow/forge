import unittest
from app.text.slug import slugify

class SlugTests(unittest.TestCase):
    def test_public_form(self):
        self.assertEqual(slugify("  Café, déjà vu!  "), "cafe-deja-vu")
        self.assertEqual(slugify("one---two"), "one-two")
