package repair

func RangeSum(lo, hi int) int {
	if lo > hi {
		return 0
	}
	sum := 0
	for i := lo; i <= hi; i++ {
		sum += i
	}
	return sum
}
