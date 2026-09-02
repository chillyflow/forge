package main

import "fmt"

func main() {
	fmt.Println("Debugging the allocation logic")
	
	// Test case from the failing test
	requests := []struct {
		id   string
		min  int
		max  int
		weight int
	}{
		{"a", 0, 1, 100},
		{"b", 0, 9, 1},
	}
	
	capacity := 5
	
	fmt.Printf("Requests: %+v\n", requests)
	fmt.Printf("Capacity: %d\n", capacity)
	
	// Manual calculation
	minTotal := 0
	maxTotal := 0
	for _, req := range requests {
		minTotal += req.min
		maxTotal += req.max
	}
	
	fmt.Printf("Min total: %d, Max total: %d\n", minTotal, maxTotal)
	
	// Check if capacity is valid
	if capacity < minTotal || capacity > maxTotal {
		fmt.Println("Capacity is invalid")
		return
	}
	
	left := capacity - minTotal
	fmt.Printf("Left to distribute: %d\n", left)
	
	// Calculate proportional shares
	weightTotal := 0
	for _, req := range requests {
		weightTotal += req.weight
	}
	
	fmt.Printf("Weight total: %d\n", weightTotal)
	
	// For each request, calculate proportional share
	for _, req := range requests {
		proportion := float64(req.weight) / float64(weightTotal)
		share := int(float64(left) * proportion)
		remainder := int64(left) * int64(req.weight) % int64(weightTotal)
		fmt.Printf("Request %s: weight=%d, proportion=%.4f, share=%d, remainder=%d\n", 
			req.id, req.weight, proportion, share, remainder)
	}
}