package repair

import "sort"

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
			indegree[requirement]++
			next[task] = append(next[task], requirement)
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
		for _, prerequisite := range next[task] {
			indegree[prerequisite]--
			if indegree[prerequisite] == 0 {
				ready = append(ready, prerequisite)
			}
		}
	}
	if len(out) != len(tasks) {
		return nil, false
	}
	return out, true
}
