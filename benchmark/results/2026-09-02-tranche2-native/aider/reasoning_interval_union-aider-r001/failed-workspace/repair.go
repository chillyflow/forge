package repair

import "sort"

type Interval struct{ Lo, Hi int }

func UnionIntervals(in []Interval) []Interval {
	// Create a copy to avoid mutating the input
	out := make([]Interval, len(in))
	for i, interval := range in {
		if interval.Lo > interval.Hi {
			out[i] = Interval{Lo: interval.Hi, Hi: interval.Lo}
		} else {
			out[i] = interval
		}
	}
	
	// Sort the intervals
	sort.Slice(out, func(i, j int) bool {
		if out[i].Lo == out[j].Lo {
			return out[i].Hi < out[j].Hi
		}
		return out[i].Lo < out[j].Lo
	})
	
	// Merge overlapping and adjacent intervals
	merged := make([]Interval, 0, len(out))
	for _, current := range out {
		if len(merged) == 0 {
			merged = append(merged, current)
			continue
		}
		
		// Check if current interval overlaps or is adjacent to the last merged interval
		last := &merged[len(merged)-1]
		if current.Lo <= last.Hi {
			// Overlapping or adjacent - merge them
			if current.Hi > last.Hi {
				last.Hi = current.Hi
			}
		} else {
			// Non-overlapping - add as new interval
			merged = append(merged, current)
		}
	}
	
	return merged
}
