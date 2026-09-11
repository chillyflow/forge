package main

import "fmt"

type Interval struct{ Lo, Hi int }

func main() {
	// Simulate the exact test case
	in := []Interval{{8, 6}, {1, 2}, {4, 5}, {2, 3}, {20, 20}, {21, 22}}
	
	// Normalize
	out := append([]Interval{}, in...)
	for i := range out {
		if out[i].Lo > out[i].Hi {
			out[i].Lo, out[i].Hi = out[i].Hi, out[i].Lo
		}
	}
	
	fmt.Println("After normalization:", out)
	
	// Sort (simulating the sort.Slice)
	// Manual sort for clarity
	for i := 0; i < len(out)-1; i++ {
		for j := i + 1; j < len(out); j++ {
			if out[i].Lo > out[j].Lo || (out[i].Lo == out[j].Lo && out[i].Hi > out[j].Hi) {
				out[i], out[j] = out[j], out[i]
			}
		}
	}
	
	fmt.Println("After sorting:", out)
	
	// Manual merging to see what happens
	merged := make([]Interval, 0, len(out))
	for _, current := range out {
		if len(merged) == 0 || current.Lo > merged[len(merged)-1].Hi {
			merged = append(merged, current)
			fmt.Printf("Added new interval: %v\n", current)
			continue
		}
		fmt.Printf("Merging %v with %v\n", current, merged[len(merged)-1])
		if current.Hi > merged[len(merged)-1].Hi {
			merged[len(merged)-1].Hi = current.Hi
			fmt.Printf("Extended to: %v\n", merged[len(merged)-1])
		}
	}
	
	fmt.Println("Final merged:", merged)
}