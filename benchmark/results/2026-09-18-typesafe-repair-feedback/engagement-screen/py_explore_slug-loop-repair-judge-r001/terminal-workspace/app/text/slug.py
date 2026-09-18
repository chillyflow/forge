import re
import unicodedata

def slugify(value):
    # Normalize unicode characters (transliterate accents)
    normalized = unicodedata.normalize('NFKD', value)
    ascii_value = normalized.encode('ascii', 'ignore').decode('ascii')
    
    # Convert to lowercase
    ascii_value = ascii_value.lower()
    
    # Replace non-alphanumeric characters with hyphens
    slug = re.sub(r'[^a-z0-9]+', '-', ascii_value)
    
    # Remove leading and trailing hyphens
    slug = slug.strip('-')
    
    # Remove multiple consecutive hyphens
    slug = re.sub(r'-+', '-', slug)
    
    return slug
