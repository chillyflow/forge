package holdout

type Event struct {
	Key      string
	Revision int
	Value    string
	Deleted  bool
}

func Project(events []Event) map[string]string {
	values := map[string]string{}
	revisions := map[string]int{}
	for _, ev := range events {
		previous, seen := revisions[ev.Key]
		if seen && ev.Revision <= previous {
			continue
		}
		revisions[ev.Key] = ev.Revision
		if ev.Deleted {
			delete(values, ev.Key)
			delete(revisions, ev.Key)
		} else {
			values[ev.Key] = ev.Value
		}
	}
	return values
}
