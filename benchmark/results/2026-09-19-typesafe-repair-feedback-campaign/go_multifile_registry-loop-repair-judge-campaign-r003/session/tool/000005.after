package names

import (
	"strings"
	"unicode"
)

func Normalize(value string) string {
	// Trim whitespace
	value = strings.TrimSpace(value)
	// Convert to lowercase
	return strings.Map(unicode.ToLower, value)
}
