package main

import (
	"fmt"
	"repair"
)

func main() {
	b := map[string]int{"a": 10, "b": 0, "c": 2}
	fmt.Printf("Initial balances: %v\n", b)
	
	result := repair.ApplyTransfers(b, []repair.Transfer{{"a", "b", 7}, {"b", "c", 3}})
	fmt.Printf("Result: %v\n", result)
	fmt.Printf("Final balances: %v\n", b)
}