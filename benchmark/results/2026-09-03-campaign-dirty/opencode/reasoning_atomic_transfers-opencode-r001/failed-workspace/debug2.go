package main

import (
    "fmt"
    "reflect"
)

type Transfer struct {
    From, To string
    Amount   int
}

func ApplyTransfers(balances map[string]int, transfers []Transfer) bool {
    next := make(map[string]int, len(balances))
    for key, value := range balances {
        next[key] = value
    }
    maxInt := int(^uint(0) >> 1)
    for _, tr := range transfers {
        from, fromOK := balances[tr.From]
        to, toOK := balances[tr.To]
        fmt.Printf("Checking transfer: %v, fromOK: %v, toOK: %v, from: %d, to: %d, amount: %d\n", tr, fromOK, toOK, from, to, tr.Amount)
        if !fromOK || !toOK || tr.Amount < 0 || from < tr.Amount {
            fmt.Println("Returning false due to validation")
            return false
        }
        if tr.From == tr.To {
            continue
        }
        if to > maxInt-tr.Amount {
            fmt.Println("Returning false due to overflow")
            return false
        }
        next[tr.From] = from - tr.Amount
        next[tr.To] = to + tr.Amount
        fmt.Printf("Updated next: %v\n", next)
    }
    for key, value := range next {
        balances[key] = value
    }
    return true
}

func main() {
    b := map[string]int{"a": 10, "b": 0, "c": 2}
    fmt.Printf("Initial balances: %v\n", b)
    result := ApplyTransfers(b, []Transfer{{"a", "b", 7}, {"b", "c", 3}})
    fmt.Printf("Result: %v\n", result)
    fmt.Printf("Final balances: %v\n", b)
    
    expected := map[string]int{"a": 3, "b": 4, "c": 5}
    fmt.Printf("Expected balances: %v\n", expected)
    fmt.Printf("Equal? %v\n", reflect.DeepEqual(b, expected))
}