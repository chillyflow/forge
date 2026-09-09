package repair

type Event struct {
	Seq           int
	ID, Kind, Key string
	Delta         int
}

func ReplayEvents(events []Event) (map[string]int, bool) {
	state := map[string]int{}
	seen := map[string]Event{}
	expected := 1
	maxInt := int(^uint(0) >> 1)
	minInt := -maxInt - 1
	for _, event := range events {
		if event.ID == "" || event.Key == "" {
			return nil, false
		}
		if event.Seq != expected {
			return nil, false
		}
		if prevEvent, ok := seen[event.ID]; ok {
			if prevEvent != event {
				return nil, false
			}
			// Skip duplicate events
			expected++
			continue
		}
		seen[event.ID] = event
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
		expected++
	}
	return state, true
}
