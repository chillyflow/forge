package repair

import (
	"math"
)

type Event struct {
	Seq           int
	ID, Kind, Key string
	Delta         int
}

func ReplayEvents(events []Event) (map[string]int, bool) {
	state := map[string]int{}
	seen := map[string]int{} // Store the expected sequence number for each ID
	expectedSeq := map[string]int{} // Track expected sequence per ID
	maxInt := math.MaxInt32
	minInt := math.MinInt32

	for _, event := range events {
		if event.ID == "" || event.Key == "" {
			return nil, false
		}
		
		// Check if we've seen this ID before
		if seq, ok := seen[event.ID]; ok {
			// If we've seen this ID, check if the event matches exactly
			// If not, return false as it's invalid
			if seq != event.Seq {
				return nil, false
			}
			// Skip duplicate events with matching sequence and payload
			continue
		}
		
		// Check if this is the expected sequence for this ID
		expected := expectedSeq[event.ID]
		if expected == 0 {
			expected = 1
		}
		if event.Seq != expected {
			return nil, false
		}
		
		// Update expected sequence for this ID
		expectedSeq[event.ID] = event.Seq + 1
		seen[event.ID] = event.Seq
		
		switch event.Kind {
		case "Add":
			current := state[event.Key]
			if event.Delta > 0 && current > maxInt-event.Delta || event.Delta < 0 && current < minInt-event.Delta {
				return nil, false
			}
			state[event.Key] = current + event.Delta
		case "Delete":
			if event.Delta != 0 {
				return nil, false
			}
			delete(state, event.Key)
		default:
			return nil, false
		}
	}
	return state, true
}
