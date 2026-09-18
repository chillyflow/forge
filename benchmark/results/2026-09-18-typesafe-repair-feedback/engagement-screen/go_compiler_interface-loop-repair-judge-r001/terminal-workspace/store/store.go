package store

type Record struct {
	ID    string
	Value int
}
type Store interface{ Get(string) (Record, bool) }
type Memory struct{ records map[string]Record }

func New(records []Record) *Memory {
	m := &Memory{records: map[string]Record{}}
	for _, r := range records {
		m.records[r.ID] = r
	}
	return m
}
func (m *Memory) Get(id string) (Record, bool) { r, ok := m.records[id]; return r, ok }

var _ Store = (*Memory)(nil)
