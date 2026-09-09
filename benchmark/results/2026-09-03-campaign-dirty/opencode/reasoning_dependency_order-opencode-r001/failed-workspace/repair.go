package repair

import "sort"

func OrderBuild(tasks []string, deps map[string][]string) ([]string, bool) {
	// Check for duplicate tasks
	seen := make(map[string]bool)
	for _, task := range tasks {
		if seen[task] {
			return nil, false
		}
		seen[task] = true
	}

	// Build dependency graph
	indegree := make(map[string]int)
	next := make(map[string][]string)
	
	// Initialize all tasks with 0 indegree
	for _, task := range tasks {
		indegree[task] = 0
	}
	
	// Process dependencies
	for task, requirements := range deps {
		// Check if task exists
		if _, exists := indegree[task]; !exists {
			return nil, false
		}
		
		// Check dependencies
		depSeen := make(map[string]bool)
		for _, requirement := range requirements {
			// Check if dependency exists
			if _, exists := indegree[requirement]; !exists {
				return nil, false
			}
			// Check for duplicate dependencies
			if depSeen[requirement] {
				return nil, false
			}
			depSeen[requirement] = true
			
			// Update dependency counts
			indegree[task]++
			next[requirement] = append(next[requirement], task)
		}
	}
	
	// Find all tasks with no dependencies (indegree = 0)
	ready := []string{}
	for task, count := range indegree {
		if count == 0 {
			ready = append(ready, task)
		}
	}
	
	// Sort ready tasks lexicographically
	sort.Strings(ready)
	
	// Process tasks in topological order
	out := []string{}
	for len(ready) > 0 {
		// Take the first task (already sorted lexicographically)
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