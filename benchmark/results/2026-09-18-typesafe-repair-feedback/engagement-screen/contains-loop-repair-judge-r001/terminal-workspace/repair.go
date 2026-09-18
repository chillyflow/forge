package repair

func Contains(xs []int, target int) bool {
	for i := 0; i < len(xs); i++ {
		if xs[i] == target {
			return true
		}
	}
	return false
}
