import unittest
from users.service import UserService

class StubRepository:
    def find(self, user_id):
        return {"id": user_id, "name": "  ada lovelace "} if user_id == "a" else None

class ServiceTests(unittest.TestCase):
    def test_protocol_and_missing(self):
        service = UserService(StubRepository())
        self.assertEqual(service.display_name("a"), "Ada Lovelace")
        self.assertIsNone(service.display_name("missing"))
