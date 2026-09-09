package main

import "fmt"

func RangeSum(lo, hi int) int {
	if lo > hi {
		return 0
	}
	sum := 0
	for i := lo; i <= hi; i++ {
		sum += i
	}
	return sum
}

func main() {
	fmt.Println("RangeSum(1, 3):", RangeSum(1, 3)) // Should be 6 (1+2+3)
	fmt.Println("RangeSum(5, 5):", RangeSum(5, 5)) // Should be 5
	fmt.Println("RangeSum(3, 1):", RangeSum(3, 1)) // Should be 0 (empty range)
	fmt.Println("RangeSum(-2, 2):", RangeSum(-2, 2)) // Should be 0 (-2-1+0+1+2)
}