package repair

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
	
	// Create a list of tasks in the order they appear in the original list
	// This will be used to determine tie-breaking
	taskOrder := make(map[string]int)
	for i, task := range tasks {
		taskOrder[task] = i
	}
	
	ready := []string{}
	for task, n := range indegree {
		if n == 0 {
			ready = append(ready, task)
		}
	}
	
	// Sort ready tasks by their position in the original tasks list to break ties
	sort.Slice(ready, func(i, j int) bool {
		return taskOrder[ready[i]] < taskOrder[ready[j]]
	})
	
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
		// Re-sort ready tasks to maintain order
		sort.Slice(ready, func(i, j int) bool {
			return taskOrder[ready[i]] < taskOrder[ready[j]]
		})
	}
	
	if len(out) != len(tasks) {
		return nil, false
	}
	return out, true
}
