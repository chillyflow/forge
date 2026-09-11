package repair

import (
	"sort"
)

type Request struct {
	ID               string
	Min, Max, Weight int
}

type quotaShare struct {
	index     int
	whole     int
	remainder int64
	id        string
}

func AllocateQuota(capacity int, requests []Request) (map[string]int, bool) {
	if capacity < 0 {
		return nil, false
	}
	out := map[string]int{}
	minTotal, maxTotal := 0, 0
	for _, request := range requests {
		if request.ID == "" || request.Min < 0 || request.Max < request.Min || request.Weight <= 0 {
			return nil, false
		}
		if _, exists := out[request.ID]; exists {
			return nil, false
		}
		if minTotal > int(^uint(0)>>1)-request.Min || maxTotal > int(^uint(0)>>1)-request.Max {
			return nil, false
		}
		minTotal += request.Min
		maxTotal += request.Max
		out[request.ID] = request.Min
	}
	if capacity < minTotal || capacity > maxTotal {
		return nil, false
	}
	left := capacity - minTotal
	if left == 0 {
		return out, true
	}

	// Distribute remainder proportionally by weight
	weightTotal := int64(0)
	active := []int{}
	for i, request := range requests {
		if out[request.ID] < request.Max {
			weightTotal += int64(request.Weight)
			active = append(active, i)
		}
	}

	if weightTotal <= 0 {
		return out, true
	}

	// Calculate initial shares
	shares := make([]quotaShare, 0, len(active))
	for _, i := range active {
		request := requests[i]
		numerator := int64(left) * int64(request.Weight)
		whole := int(numerator / weightTotal)
		remainder := numerator % weightTotal
		room := request.Max - out[request.ID]
		if whole > room {
			whole = room
		}
		shares = append(shares, quotaShare{i, whole, remainder, request.ID})
	}

	// Distribute whole portions
	used := 0
	for _, share := range shares {
		out[share.id] += share.whole
		used += share.whole
	}
	left -= used

	// Handle remaining with fractional remainder tie-breaking
	if left > 0 {
		// Sort by remainder descending, then by ID ascending for tie-breaking
		sort.SliceStable(shares, func(i, j int) bool {
			if shares[i].remainder == shares[j].remainder {
				// For tie-breaking, sort by ID lexicographically
				return shares[i].id < shares[j].id
			}
			return shares[i].remainder > shares[j].remainder
		})

		// Allocate remaining units to highest fractional remainders
		for i := 0; i < left && i < len(shares); i++ {
			share := shares[i]
			out[share.id]++
		}
	}

	return out, true
}
