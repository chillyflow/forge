package main

import (
	"fmt"
	"forgebench/repair"
)

func main() {
	fmt.Println("Testing addition with negative numbers:")
	fmt.Printf("Add(2, 3) = %d\n", repair.Add(2, 3))
	fmt.Printf("Add(-2, 3) = %d\n", repair.Add(-2, 3))
	fmt.Printf("Add(-2, -3) = %d\n", repair.Add(-2, -3))
	fmt.Printf("Add(5, -3) = %d\n", repair.Add(5, -3))
}