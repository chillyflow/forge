package repair

func Unique(xs []int) []int {
	out := []int{}
	seen := map[int]bool{}
	for _, v := range xs {
		if !seen[v] {
			out = append(out, v)
		}
		seen[v] = true
	}
	return out
}
