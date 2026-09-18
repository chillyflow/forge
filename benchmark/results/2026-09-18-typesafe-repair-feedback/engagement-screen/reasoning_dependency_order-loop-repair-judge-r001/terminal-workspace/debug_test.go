package main

import (
	"fmt"
	"sort"
)

func debugOrderBuild(tasks []string, deps map[string][]string) ([]string, bool) {
	// Check for duplicate tasks
	known := map[string]bool{}
	for _, task := range tasks {
		if known[task] {
			return nil, false
		}
		known[task] = true
	}
	
	// Build dependency graph
	indegree := map[string]int{}
	next := map[string][]string{}
	
	// Initialize all tasks with 0 indegree
	for _, task := range tasks {
		indegree[task] = 0
	}
	
	// Process dependencies
	for task, requirements := range deps {
		// Check if task exists
		if !known[task] {
			return nil, false
		}
		
		// Check for duplicate dependencies and unknown prerequisites
		seen := map[string]bool{}
		for _, requirement := range requirements {
			// Check if requirement exists
			if !known[requirement] {
				return nil, false
			}
			// Check for duplicate dependencies
			if seen[requirement] {
				return nil, false
			}
			seen[requirement] = true
			
			// Update dependency graph
			indegree[task]++
			next[requirement] = append(next[requirement], task)
		}
	}
	
	fmt.Printf("indegree: %v\n", indegree)
	fmt.Printf("next: %v\n", next)
	
	// Find all tasks with no dependencies (indegree = 0)
	ready := []string{}
	for task, n := range indegree {
		if n == 0 {
			ready = append(ready, task)
		}
	}
	
	fmt.Printf("ready initially: %v\n", ready)
	
	// Sort lexicographically to ensure deterministic order
	sort.Strings(ready)
	
	fmt.Printf("ready after sort: %v\n", ready)
	
	// Topological sort
	out := []string{}
	for len(ready) > 0 {
		// Take the first task (lexicographically smallest)
		task := ready[0]
		ready = ready[1:]
		out = append(out, task)
		
		fmt.Printf("Processing task: %s, ready now: %v\n", task, ready)
		
		// Remove this task and update dependencies
		for _, dependent := range next[task] {
			indegree[dependent]--
			fmt.Printf("  %s now has indegree %d\n", dependent, indegree[dependent])
			if indegree[dependent] == 0 {
				ready = append(ready, dependent)
				fmt.Printf("  Added %s to ready\n", dependent)
			}
		}
	}
	
	fmt.Printf("Final output: %v\n", out)
	
	// Check if all tasks were processed (no cycles)
	if len(out) != len(tasks) {
		return nil, false
	}
	
	return out, true
}

func main() {
	tasks := []string{"package", "compile", "generate", "lint"}
	deps := map[string][]string{"package": {"compile"}, "compile": {"generate"}}
	
	result, ok := debugOrderBuild(tasks, deps)
	fmt.Printf("Result: %v, ok: %v\n", result, ok)
}