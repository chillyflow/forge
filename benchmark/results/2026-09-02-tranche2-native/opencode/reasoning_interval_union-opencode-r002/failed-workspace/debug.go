package main

import (
	"fmt"
	"repair"
)

func main() {
	in := []repair.Interval{{8, 6}, {1, 2}, {4, 5}, {2, 3}, {20, 20}, {21, 22}}
	fmt.Printf("Input: %v\n", in)
	result := repair.UnionIntervals(in)
	fmt.Printf("Result: %v\n", result)
}