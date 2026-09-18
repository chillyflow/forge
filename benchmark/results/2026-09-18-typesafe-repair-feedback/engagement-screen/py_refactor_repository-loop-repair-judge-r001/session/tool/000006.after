from .repository import UserRepository

class UserService:
    def __init__(self, repository: UserRepository):
        self.repository = repository

    def display_name(self, user_id):
        user = self.repository.find(user_id)
        if user is None:
            return None
        return user["name"].strip().title()
