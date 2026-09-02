package holdout

type Booking struct{ Seat, Customer string }

func Reserve(bookings map[string]string, batch []Booking) bool {
	// Empty batches succeed
	if len(batch) == 0 {
		return true
	}

	// Check if any seat in the batch is already booked
	for _, b := range batch {
		if b.Seat == "" || b.Customer == "" {
			return false
		}
		if _, exists := bookings[b.Seat]; exists {
			return false
		}
	}

	// All seats are available, reserve them all
	for _, b := range batch {
		bookings[b.Seat] = b.Customer
	}
	
	return true
}
