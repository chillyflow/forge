package repair

import "fmt"

func main() {
	events := []Event{{1, "a", "Add", "x", 3}, {2, "b", "Add", "x", -1}, {2, "b", "Add", "x", -1}, {3, "c", "Delete", "x", 0}, {4, "d", "Add", "y", 5}}
	fmt.Println("Testing with events:", events)
	result, ok := ReplayEvents(events)
	fmt.Printf("Result: %v, ok: %v\n", result, ok)
	
	// Test the invalid cases
	invalid := [][]Event{{{2, "a", "Add", "x", 1}}, {{1, "a", "Add", "x", 1}, {1, "a", "Add", "x", 2}}, {{1, "a", "Delete", "x", 1}}, {{1, "a", "Other", "x", 0}}, {{1, "a", "Add", "x", 1000000}, {2, "b", "Add", "x", 1}}}
	for i, stream := range invalid {
		result, ok := ReplayEvents(stream)
		fmt.Printf("Invalid case %d: %v -> result: %v, ok: %v\n", i, stream, result, ok)
	}
}