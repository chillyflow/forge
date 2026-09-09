package repair

import (
	"sort"
)

type Request struct {
	ID               string
	Min, Max, Weight int
}

type quotaShare struct {
	index, whole int
	remainder    int64
	id           string
}

func AllocateQuota(capacity int, requests []Request) (map[string]int, bool) {
	if capacity < 0 {
		return nil, false
	}
	out := map[string]int{}
	minTotal, maxTotal := 0, 0
	idSet := map[string]bool{}
	
	for _, request := range requests {
		if request.ID == "" || request.Min < 0 || request.Max < request.Min || request.Weight <= 0 {
			return nil, false
		}
		if idSet[request.ID] {
			return nil, false
		}
		idSet[request.ID] = true
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
	
	// Calculate proportional shares
	weightTotal := int64(0)
	activeRequests := []Request{}
	
	for _, request := range requests {
		if out[request.ID] < request.Max {
			weightTotal += int64(request.Weight)
			activeRequests = append(activeRequests, request)
		}
	}
	
	if weightTotal == 0 {
		return nil, false
	}
	
	// Calculate initial shares and remainders
	shares := make([]quotaShare, len(activeRequests))
	totalUsed := 0
	
	for i, request := range activeRequests {
		numerator := int64(left) * int64(request.Weight)
		whole := int(numerator / weightTotal)
		remainder := numerator % weightTotal
		
		// Ensure we don't exceed max
		room := request.Max - out[request.ID]
		if whole > room {
			whole = room
		}
		
		shares[i] = quotaShare{
			index:     i,
			whole:     whole,
			remainder: remainder,
			id:        request.ID,
		}
		totalUsed += whole
	}
	
	// Distribute the initial shares
	for _, share := range shares {
		out[share.id] += share.whole
	}
	
	// Handle leftover units using largest fractional remainder with ID tie-break
	leftover := left - totalUsed
	if leftover > 0 {
		// Sort by remainder descending, then by index ascending for tie-breaking
		sort.SliceStable(shares, func(i, j int) bool {
			if shares[i].remainder == shares[j].remainder {
				return shares[i].index < shares[j].index
			}
			return shares[i].remainder > shares[j].remainder
		})
		
		// Assign leftover units
		for i := 0; i < int(leftover) && i < len(shares); i++ {
			out[shares[i].id]++
		}
	}
	
	return out, true
}
