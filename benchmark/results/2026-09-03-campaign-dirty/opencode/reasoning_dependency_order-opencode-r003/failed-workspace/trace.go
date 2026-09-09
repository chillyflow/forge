package main

import (
	"fmt"
	"sort"
)

func OrderBuild(tasks []string, deps map[string][]string) ([]string, bool) {
	known := map[string]bool{}
	for _, task := range tasks {
		if known[task] {
			return nil, false
		}
		known[task] = true
	}
	indegree := map[string]int{}
	next := map[string][]string{}
	for _, task := range tasks {
		indegree[task] = 0
	}
	for task, requirements := range deps {
		if !known[task] {
			return nil, false
		}
		seen := map[string]bool{}
		for _, requirement := range requirements {
			if !known[requirement] || seen[requirement] {
				return nil, false
			}
			seen[requirement] = true
			indegree[task]++
			next[requirement] = append(next[requirement], task)
		}
	}
	ready := []string{}
	for task, n := range indegree {
		if n == 0 {
			ready = append(ready, task)
		}
	}
	sort.Strings(ready)
	out := []string{}
	for len(ready) > 0 {
		task := ready[0]
		ready = ready[1:]
		out = append(out, task)
		for _, dependent := range next[task] {
			indegree[dependent]--
			if indegree[dependent] == 0 {
				ready = append(ready, dependent)
			}
		}
	}
	if len(out) != len(tasks) {
		return nil, false
	}
	return out, true
}

func main() {
	tasks := []string{"package", "compile", "generate", "lint"}
	deps := map[string][]string{"package": {"compile"}, "compile": {"generate"}}
	
	result, ok := OrderBuild(tasks, deps)
	fmt.Printf("Result: %v, ok: %v\n", result, ok)
	
	// Expected: [generate compile lint package]
	// But we get: [generate lint compile package]
	
	// Let's trace what happens:
	fmt.Println("Tracing:")
	fmt.Println("Tasks:", tasks)
	fmt.Println("Dependencies:", deps)
	
	// Build indegree map
	indegree := map[string]int{}
	for _, task := range tasks {
		indegree[task] = 0
	}
	for task, requirements := range deps {
		for _, requirement := range requirements {
			indegree[task]++
		}
	}
	fmt.Println("Indegree:", indegree)
	
	// Ready queue
	ready := []string{}
	for task, n := range indegree {
		if n == 0 {
			ready = append(ready, task)
		}
	}
	fmt.Println("Ready queue before sort:", ready)
	sort.Strings(ready)
	fmt.Println("Ready queue after sort:", ready)
}