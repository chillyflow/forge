package main

import "fmt"

type Interval struct{ Lo, Hi int }

func UnionIntervals(in []Interval) []Interval {
	out := append([]Interval{}, in...)
	for i := range out {
		if out[i].Lo > out[i].Hi {
			out[i].Lo, out[i].Hi = out[i].Hi, out[i].Lo
		}
	}
	
	// Manual sort for debugging
	for i := 0; i < len(out)-1; i++ {
		for j := i + 1; j < len(out); j++ {
			if out[i].Lo > out[j].Lo || (out[i].Lo == out[j].Lo && out[i].Hi > out[j].Hi) {
				out[i], out[j] = out[j], out[i]
			}
		}
	}
	
	fmt.Println("After normalization and sorting:", out)
	
	merged := make([]Interval, 0, len(out))
	for _, current := range out {
		if len(merged) == 0 || current.Lo > merged[len(merged)-1].Hi {
			fmt.Printf("Adding new interval %v\n", current)
			merged = append(merged, current)
		} else {
			fmt.Printf("Merging %v with %v\n", current, merged[len(merged)-1])
			if current.Hi > merged[len(merged)-1].Hi {
				merged[len(merged)-1].Hi = current.Hi
				fmt.Printf("Updated to %v\n", merged[len(merged)-1])
			}
		}
	}
	return merged
}

func main() {
	in := []Interval{{8, 6}, {1, 2}, {4, 5}, {2, 3}, {20, 20}, {21, 22}}
	result := UnionIntervals(in)
	fmt.Println("Result:", result)
}