package holdout

type Booking struct{ Seat, Customer string }

func Reserve(bookings map[string]string, batch []Booking) bool {
	// Check all bookings in the batch first to ensure none conflict
	for _, b := range batch {
		if b.Seat == "" || b.Customer == "" {
			return false
		}
		if _, exists := bookings[b.Seat]; exists {
			return false
		}
	}

	// All bookings are valid, now add them to the bookings map
	for _, b := range batch {
		bookings[b.Seat] = b.Customer
	}
	return true
}
