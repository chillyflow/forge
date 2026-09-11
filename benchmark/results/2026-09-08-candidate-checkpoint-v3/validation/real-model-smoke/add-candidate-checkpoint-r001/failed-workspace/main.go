package main

import (
	"fmt"
	"./"
)

func main() {
	fmt.Println("Testing Add function:")
	fmt.Printf("Add(2, 3) = %d\n", repair.Add(2, 3))
	fmt.Printf("Add(-2, 3) = %d\n", repair.Add(-2, 3))
	fmt.Printf("Add(-2, -3) = %d\n", repair.Add(-2, -3))
	fmt.Printf("Add(0, 5) = %d\n", repair.Add(0, 5))
}