from abc import ABC, abstractmethod

class UserRepository(ABC):
    @abstractmethod
    def find(self, user_id):
        pass

class MemoryRepository(UserRepository):
    def __init__(self, users):
        self.users = {user["id"]: user for user in users}

    def find(self, user_id):
        return self.users.get(user_id)
