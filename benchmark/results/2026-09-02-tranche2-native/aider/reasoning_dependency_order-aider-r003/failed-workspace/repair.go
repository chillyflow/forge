package repair

import (
	"sort"
)

func OrderBuild(tasks []string, deps map[string][]string) ([]string, bool) {
	// Check for duplicate tasks
	known := map[string]bool{}
	for _, task := range tasks {
		if known[task] {
			return nil, false
		}
		known[task] = true
	}

	// Initialize data structures
	indegree := map[string]int{}
	next := map[string][]string{}
	
	// Set initial indegree to 0 for all tasks
	for _, task := range tasks {
		indegree[task] = 0
	}
	
	// Process dependencies
	for task, requirements := range deps {
		// Check if task exists
		if !known[task] {
			return nil, false
		}
		
		// Check for duplicate dependencies and unknown dependencies
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
	
	// Find all tasks with no dependencies (indegree = 0)
	ready := []string{}
	for task, n := range indegree {
		if n == 0 {
			ready = append(ready, task)
		}
	}
	
	// Sort ready tasks lexicographically for deterministic output
	sort.Strings(ready)
	
	// Process tasks in topological order
	out := []string{}
	for len(ready) > 0 {
		// Take the first task (lexicographically smallest)
		task := ready[0]
		ready = ready[1:]
		out = append(out, task)
		
		// Update dependencies of dependent tasks
		for _, dependent := range next[task] {
			indegree[dependent]--
			if indegree[dependent] == 0 {
				ready = append(ready, dependent)
			}
		}
	}
	
	// Check if all tasks were processed (no cycles)
	if len(out) != len(tasks) {
		return nil, false
	}
	
	return out, true
}
