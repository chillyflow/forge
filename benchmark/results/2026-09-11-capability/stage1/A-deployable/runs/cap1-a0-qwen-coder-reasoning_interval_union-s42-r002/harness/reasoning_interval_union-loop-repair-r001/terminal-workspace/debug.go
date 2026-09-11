package main

import "fmt"

type Interval struct{ Lo, Hi int }

func main() {
	// Test the exact case from the test
	in := []Interval{{8, 6}, {1, 2}, {4, 5}, {2, 3}, {20, 20}, {21, 22}}
	fmt.Printf("Input: %v\n", in)
	
	// Normalize
	out := append([]Interval{}, in...)
	for i := range out {
		if out[i].Lo > out[i].Hi {
			out[i].Lo, out[i].Hi = out[i].Hi, out[i].Lo
		}
	}
	fmt.Printf("After normalization: %v\n", out)
	
	// Sort
	for i := 0; i < len(out)-1; i++ {
		for j := i + 1; j < len(out); j++ {
			if out[i].Lo > out[j].Lo || (out[i].Lo == out[j].Lo && out[i].Hi > out[j].Hi) {
				out[i], out[j] = out[j], out[i]
			}
		}
	}
	fmt.Printf("After sorting: %v\n", out)
	
	// Merge
	merged := make([]Interval, 0, len(out))
	for _, current := range out {
		fmt.Printf("Processing %v\n", current)
		if len(merged) == 0 {
			fmt.Println("  Adding to merged (empty)")
			merged = append(merged, current)
			continue
		}
		fmt.Printf("  Last merged interval: %v\n", merged[len(merged)-1])
		fmt.Printf("  Condition check: %v > %v = %v\n", current.Lo, merged[len(merged)-1].Hi, current.Lo > merged[len(merged)-1].Hi)
		if current.Lo > merged[len(merged)-1].Hi {
			fmt.Println("  Adding to merged (no overlap)")
			merged = append(merged, current)
			continue
		}
		fmt.Printf("  Merging with last interval\n")
		if current.Hi > merged[len(merged)-1].Hi {
			fmt.Printf("  Extending hi from %v to %v\n", merged[len(merged)-1].Hi, current.Hi)
			merged[len(merged)-1].Hi = current.Hi
		}
	}
	
	fmt.Printf("Final result: %v\n", merged)
}