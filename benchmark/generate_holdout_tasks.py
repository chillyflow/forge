"""Recreate the September 2 holdout. Never use model outcomes to revise this set."""
import json
from pathlib import Path
import textwrap


DEST = Path(__file__).parent / 'holdout' / '2026-09-02' / 'tasks'


def case(name, language, category, prompt, source, tests, mutations):
    source = textwrap.dedent(source).lstrip()
    broken = source
    for good, bad in mutations:
        assert broken.count(good) == 1, (name, good)
        broken = broken.replace(good, bad, 1)
    if language == 'go':
        path, test_path = 'service.go', 'service_test.go'
        files = {'go.mod': 'module holdout\n\ngo 1.24\n'}
        verify = ['go', 'test', '-json', './...']
    else:
        path, test_path = 'service.py', 'test_service.py'
        files = {}
        verify = ['python', '-m', 'unittest', 'discover', '-v']
    files.update({path: broken, test_path: textwrap.dedent(tests).lstrip()})
    task = {'id': name, 'suite': 'holdout-2026-09-02', 'language': language,
            'category': category, 'prompt': prompt +
            ' Inspect the code, repair the implementation, and run the tests. Do not modify tests.',
            'verify': verify, 'files': files, 'oracle_files': {path: source}}
    DEST.mkdir(parents=True, exist_ok=True)
    (DEST / f'{name}.json').write_text(json.dumps(task, indent=2) + '\n', encoding='utf-8')


def main():
    case('holdout_go_reservations', 'go', 'atomic',
         'Reserve atomically assigns seats for every request or leaves the booking map unchanged. '
         'Existing bookings and duplicate seat requests must reject the entire batch; empty batches succeed.',
         '''
         package holdout
         type Booking struct { Seat, Customer string }
         func Reserve(bookings map[string]string, batch []Booking) bool {
             pending := make(map[string]string)
             for _, b := range batch {
                 if b.Seat == "" || b.Customer == "" { return false }
                 if _, exists := bookings[b.Seat]; exists { return false }
                 if _, exists := pending[b.Seat]; exists { return false }
                 pending[b.Seat] = b.Customer
             }
             for seat, customer := range pending { bookings[seat] = customer }
             return true
         }
         ''', '''
         package holdout
         import ("reflect"; "testing")
         func TestReserve(t *testing.T) {
             for _, batch := range [][]Booking{
                 {{"B", "bob"}, {"B", "bea"}},
                 {{"B", "bob"}, {"A", "bea"}},
                 {{"B", "bob"}, {"C", ""}},
             } {
                 b := map[string]string{"A":"ann"}
                 if Reserve(b,batch) || !reflect.DeepEqual(b,map[string]string{"A":"ann"}) {
                     t.Fatalf("rejected reservation changed bookings: %v",b)
                 }
             }
             b := map[string]string{"A":"ann"}
             if !Reserve(b, []Booking{{"B","bob"},{"C","cam"}}) || len(b)!=3 { t.Fatal(b) }
             if !Reserve(b,nil) || b["B"]!="bob" { t.Fatal(b) }
         }
         ''', [('if _, exists := pending[b.Seat]; exists { return false }',
                '// Reservations in this batch will be committed together.')])

    case('holdout_go_build_waves', 'go', 'dependency',
         'BuildWaves groups jobs into dependency waves. A wave contains every job whose prerequisites '
         'were completed in earlier waves, sorted lexically. Missing prerequisites and cycles return an error.',
         '''
         package holdout
         import ("fmt"; "sort")
         func BuildWaves(deps map[string][]string) ([][]string,error) {
             done := map[string]bool{}
             waves := [][]string{}
             for len(done)<len(deps) {
                 ready:=[]string{}
                 for job, prerequisites := range deps {
                     if done[job] { continue }
                     ok:=true
                     for _, p := range prerequisites {
                         if _, exists:=deps[p]; !exists { return nil,fmt.Errorf("missing %s",p) }
                         if !done[p] { ok=false }
                     }
                     if ok { ready=append(ready,job) }
                 }
                 if len(ready)==0 { return nil,fmt.Errorf("cycle") }
                 sort.Strings(ready)
                 for _,job:=range ready { done[job]=true }
                 waves=append(waves,ready)
             }
             return waves,nil
         }
         ''', '''
         package holdout
         import ("reflect"; "testing")
         func TestWaves(t *testing.T) {
             deps:=map[string][]string{"app":{"lib","assets"},"lib":{"core"},"core":{},"assets":{}}
             want:=[][]string{{"assets","core"},{"lib"},{"app"}}
             for i:=0;i<20;i++ { got,err:=BuildWaves(deps); if err!=nil || !reflect.DeepEqual(got,want) { t.Fatalf("waves %v, error %v",got,err) } }
             for _,bad:=range []map[string][]string{{"a":{"b"},"b":{"a"}},{"a":{"missing"}},{"a":{"a"}}} {
                 if _,err:=BuildWaves(bad);err==nil { t.Fatal("invalid graph accepted") }
             }
             got,err:=BuildWaves(map[string][]string{}); if err!=nil || len(got)!=0 { t.Fatal(got,err) }
             got,err=BuildWaves(map[string][]string{"a":{},"b":{"a","a"}})
             if err!=nil || !reflect.DeepEqual(got,[][]string{{"a"},{"b"}}) { t.Fatal(got,err) }
         }
         ''', [('if !done[p] { ok=false }',
                'if !done[p] && len(deps[p])>0 { ok=false }')])

    case('holdout_go_projection', 'go', 'replay',
         'Project consumes versioned key updates in arrival order. Only a strictly newer revision '
         'may change a key; deletion must prevent older updates from resurrecting that key. Revisions are per key.',
         '''
         package holdout
         type Event struct { Key string; Revision int; Value string; Deleted bool }
         func Project(events []Event) map[string]string {
             values:=map[string]string{}
             revisions:=map[string]int{}
             for _,ev:=range events {
                 previous,seen:=revisions[ev.Key]
                 if seen && ev.Revision<=previous { continue }
                 revisions[ev.Key]=ev.Revision
                 if ev.Deleted { delete(values,ev.Key) } else { values[ev.Key]=ev.Value }
             }
             return values
         }
         ''', '''
         package holdout
         import ("reflect"; "testing")
         func TestProject(t *testing.T) {
             events:=[]Event{{"x",5,"new",false},{"x",6,"",true},{"x",4,"stale",false},
                 {"y",1,"first",false},{"y",1,"duplicate",false},{"z",0,"zero",false}}
             got:=Project(events)
             if !reflect.DeepEqual(got,map[string]string{"y":"first","z":"zero"}) { t.Fatalf("projection %v",got) }
             events=append(events,Event{"x",7,"reborn",false})
             if Project(events)["x"]!="reborn" { t.Fatal("new revision rejected") }
             if len(Project(nil))!=0 { t.Fatal("empty input") }
         }
         ''', [('delete(values,ev.Key)', 'delete(values,ev.Key); delete(revisions,ev.Key)')])

    case('holdout_go_mounts', 'go', 'api',
         'ResolveMount selects the longest matching mount by path components and returns the remaining '
         'relative path. Clean paths first. The root mount is a fallback; textual prefixes inside a component do not match.',
         '''
         package holdout
         import ("path"; "strings")
         func ResolveMount(request string, mounts []string) (string,string,bool) {
             request=path.Clean("/"+request)
             best:=""
             for _,raw:=range mounts {
                 mount:=path.Clean("/"+raw)
                 if (mount=="/" || request==mount || strings.HasPrefix(request,mount+"/")) && len(mount)>len(best) { best=mount }
             }
             if best=="" { return "","",false }
             return best,strings.TrimPrefix(strings.TrimPrefix(request,best),"/"),true
         }
         ''', '''
         package holdout
         import "testing"
         func TestMounts(t *testing.T) {
             for _,tc:=range []struct{request,mount,rest string}{
                 {"/apple/x","/","apple/x"},{"/app/a","/app","a"},{"/app","/app",""},
                 {"/app/v2/x","/app/v2","x"},{"/app/../apple","/","apple"},
             } {
                 mount,rest,ok:=ResolveMount(tc.request,[]string{"/app","/","/app/v2"})
                 if !ok || mount!=tc.mount || rest!=tc.rest { t.Fatalf("%s: %q %q %v",tc.request,mount,rest,ok) }
             }
             if _,_,ok:=ResolveMount("/apple",[]string{"/app"});ok { t.Fatal("partial component matched") }
         }
         ''', [('request==mount || strings.HasPrefix(request,mount+"/")',
                'strings.HasPrefix(request,mount)')])

    case('holdout_go_window', 'go', 'algorithm',
         'Window.Add accepts nondecreasing integer timestamps and sums values in (now-width, now]. '
         'Rejected out-of-order timestamps must leave the window unchanged. Width is positive.',
         '''
         package holdout
         type sample struct { at,value int }
         type Window struct { Width int; samples []sample; last int; started bool }
         func (w *Window) Add(now,value int) (int,bool) {
             if w.started && now<w.last { return 0,false }
             w.last=now; w.started=true
             w.samples=append(w.samples,sample{now,value})
             first:=0
             for first<len(w.samples) && w.samples[first].at<=now-w.Width { first++ }
             w.samples=w.samples[first:]
             sum:=0; for _,s:=range w.samples { sum+=s.value }
             return sum,true
         }
         ''', '''
         package holdout
         import "testing"
         func TestWindow(t *testing.T) {
             w:=Window{Width:5}
             for _,tc:=range []struct{at,value,want int}{{0,3,3},{4,2,5},{5,7,9},{5,-2,7},{10,1,1}} {
                 got,ok:=w.Add(tc.at,tc.value); if !ok || got!=tc.want { t.Fatalf("at %d sum %d",tc.at,got) }
             }
             if _,ok:=w.Add(9,100);ok { t.Fatal("backdated sample accepted") }
             if got,ok:=w.Add(10,2);!ok || got!=3 { t.Fatal(got,ok) }
         }
         ''', [('w.samples[first].at<=now-w.Width','w.samples[first].at<now-w.Width')])

    case('holdout_go_lru', 'go', 'stateful',
         'Cache is a bounded LRU cache. Successful Get and both new and existing Put operations make '
         'the key most recently used. Updating a key must preserve other entries until capacity is exceeded.',
         '''
         package holdout
         type Cache struct { Capacity int; values map[string]int; order []string }
         func (c *Cache) touch(key string) {
             for i,k:=range c.order { if k==key { c.order=append(c.order[:i],c.order[i+1:]...);break } }
             c.order=append(c.order,key)
         }
         func (c *Cache) Get(key string) (int,bool) {
             value,ok:=c.values[key];if ok { c.touch(key) };return value,ok
         }
         func (c *Cache) Put(key string,value int) {
             if c.Capacity<=0 { return }
             if c.values==nil { c.values=map[string]int{} }
             c.values[key]=value
             c.touch(key)
             if len(c.values)>c.Capacity { delete(c.values,c.order[0]);c.order=c.order[1:] }
         }
         ''', '''
         package holdout
         import "testing"
         func TestCache(t *testing.T) {
             c:=Cache{Capacity:2};c.Put("a",1);c.Put("b",2);c.Put("a",3);c.Put("c",4)
             if _,ok:=c.Get("b");ok { t.Fatal("least recent key retained") }
             if v,ok:=c.Get("a");!ok || v!=3 { t.Fatal("updated key evicted",v,ok) }
             c.Put("d",5);if _,ok:=c.Get("c");ok { t.Fatal("Get did not refresh recency") }
             z:=Cache{Capacity:0};z.Put("x",1);if _,ok:=z.Get("x");ok { t.Fatal("zero capacity") }
             one:=Cache{Capacity:1};one.Put("x",1);one.Put("x",2);if v,ok:=one.Get("x");!ok || v!=2 { t.Fatal(v,ok) }
         }
         ''', [('c.values[key]=value\n    c.touch(key)',
                '_,exists:=c.values[key]\n    c.values[key]=value\n    if !exists { c.touch(key) }')])

    case('holdout_py_versions', 'python', 'atomic',
         'commit applies a batch of optimistic document updates atomically. Each update supplies the '
         'expected current version. Repeated updates to one document are allowed when their versions '
         'form a valid chain. Missing documents and version conflicts leave all documents unchanged.',
         '''
         def commit(documents, updates):
             staged = dict(documents)
             for key, expected, value in updates:
                 if key not in staged or staged[key][0] != expected:
                     return False
                 staged[key] = (expected + 1, value)
             documents.clear()
             documents.update(staged)
             return True
         ''', '''
         import unittest
         from service import commit
         class Versions(unittest.TestCase):
             def test_chain(self):
                 docs = {'x': (0, 'old'), 'y': (4, 'stay')}
                 self.assertTrue(commit(docs, [('x', 0, 'mid'), ('x', 1, 'new')]))
                 self.assertEqual(docs, {'x': (2, 'new'), 'y': (4, 'stay')})
             def test_conflicts_roll_back(self):
                 for updates in [[('x', 0, 'new'), ('x', 0, 'bad')],
                                 [('x', 0, 'new'), ('absent', 0, 'bad')]]:
                     docs = {'x': (0, 'old')}
                     self.assertFalse(commit(docs, updates))
                     self.assertEqual(docs, {'x': (0, 'old')})
             def test_empty(self):
                 self.assertTrue(commit({}, []))
         ''', [('staged = dict(documents)', 'staged = documents')])

    case('holdout_py_closure', 'python', 'dependency',
         'requirements returns the transitive prerequisites of the requested targets, excluding the '
         'targets themselves. Include only reachable jobs, visit shared dependencies once, and reject '
         'reachable cycles or unknown jobs with ValueError.',
         '''
         def requirements(graph, targets):
             active, visited = set(), set()
             def visit(node):
                 if node in active:
                     raise ValueError('cycle')
                 if node in visited:
                     return
                 if node not in graph:
                     raise ValueError('missing job')
                 active.add(node)
                 for dependency in graph[node]:
                     visit(dependency)
                 active.remove(node)
                 visited.add(node)
             targets = set(targets)
             for target in targets:
                 visit(target)
             return visited - targets
         ''', '''
         import unittest
         from service import requirements
         class Closure(unittest.TestCase):
             def test_diamond(self):
                 graph = {'a': ['b', 'c'], 'b': ['d'], 'c': ['d'], 'd': [], 'unused': ['missing']}
                 self.assertEqual(requirements(graph, ['a']), {'b', 'c', 'd'})
                 self.assertEqual(requirements(graph, ['a', 'b']), {'c', 'd'})
                 self.assertEqual(requirements(graph, []), set())
             def test_rejections(self):
                 for graph in [{'a': ['b'], 'b': ['a']}, {'a': ['missing']}, {'a': ['a']}]:
                     with self.assertRaises(ValueError):
                         requirements(graph, ['a'])
         ''', [('active.remove(node)', '# Keep the traversal history for later visits.')])

    case('holdout_py_retractions', 'python', 'replay',
         'balances replays postings and retractions. Event IDs are globally idempotent. A retraction '
         'cancels its referenced posting exactly once, even if it arrives before that posting. '
         'Unmatched retractions have no balance effect; accounts whose postings cancel retain a zero entry.',
         '''
         def balances(events):
             seen, cancelled = set(), set()
             postings, totals = {}, {}
             for event in events:
                 if event['id'] in seen:
                     continue
                 seen.add(event['id'])
                 if event['kind'] == 'post':
                     account, amount = event['account'], event['amount']
                     postings[event['id']] = (account, amount)
                     totals.setdefault(account, 0)
                     if event['id'] not in cancelled:
                         totals[account] += amount
                 else:
                     target = event['target']
                     if target in cancelled:
                         continue
                     cancelled.add(target)
                     if target in postings:
                         account, amount = postings[target]
                         totals[account] -= amount
             return totals
         ''', '''
         import unittest
         from service import balances
         def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
         def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}
         class Retractions(unittest.TestCase):
             def test_order_and_duplicates(self):
                 p = post('p', 7)
                 for events in [[p, retract('r', 'p')], [retract('r', 'p'), p],
                                [p, p, retract('r', 'p'), retract('s', 'p')]]:
                     self.assertEqual(balances(events), {'cash': 0})
             def test_independent_postings(self):
                 self.assertEqual(balances([post('a', 7), post('b', -2), retract('r', 'a')]), {'cash': -2})
                 self.assertEqual(balances([retract('r', 'missing')]), {})
                 self.assertEqual(balances([]), {})
         ''', [("if event['id'] not in cancelled:", 'if True:')])

    case('holdout_py_overlay', 'python', 'api',
         'overlay recursively combines configuration dictionaries. Override values replace defaults; '
         'nested dictionaries merge, None deletes a key, and lists replace in full. The returned data '
         'must share no mutable containers with either input.',
         '''
         from copy import deepcopy
         def overlay(base, changes):
             result = deepcopy(base)
             for key, value in changes.items():
                 if value is None:
                     result.pop(key, None)
                 elif isinstance(value, dict) and isinstance(result.get(key), dict):
                     result[key] = overlay(result[key], value)
                 else:
                     result[key] = deepcopy(value)
             return result
         ''', '''
         import unittest
         from service import overlay
         class Overlay(unittest.TestCase):
             def test_merge_and_delete(self):
                 base = {'db': {'port': 1, 'host': 'local'}, 'tags': ['base'], 'remove': 1}
                 changes = {'db': {'port': 2}, 'tags': ['new'], 'remove': None}
                 result = overlay(base, changes)
                 self.assertEqual(result, {'db': {'port': 2, 'host': 'local'}, 'tags': ['new']})
                 result['tags'].append('later')
                 result['db']['host'] = 'elsewhere'
                 self.assertEqual(changes['tags'], ['new'])
                 self.assertEqual(base['db']['host'], 'local')
             def test_container_replacement(self):
                 changes = {'x': {'nested': []}}
                 result = overlay({'x': 1}, changes)
                 result['x']['nested'].append(3)
                 self.assertEqual(changes['x']['nested'], [])
                 self.assertEqual(overlay({'a': 2}, {'absent': None}), {'a': 2})
         ''', [('result[key] = deepcopy(value)', 'result[key] = value')])

    case('holdout_py_records', 'python', 'parser',
         'read_records parses comma-separated records with the standard csv module. Quoted cells '
         'may contain commas, line breaks, or doubled quotes. Preserve blank cells and use all physical '
         'lines belonging to a quoted record. Return a list of rows.',
         '''
         import csv
         import io
         def read_records(text):
             return list(csv.reader(io.StringIO(text, newline='')))
         ''', '''
         import unittest
         from service import read_records
         class Records(unittest.TestCase):
             def test_embedded_lines(self):
                 self.assertEqual(read_records('id,note\\r\\n1,"hello\\r\\nworld"\\r\\n'),
                                  [['id', 'note'], ['1', 'hello\\r\\nworld']])
             def test_quotes_and_blanks(self):
                 self.assertEqual(read_records('a,"b,c","d""e",\\n'), [['a', 'b,c', 'd"e', '']])
                 self.assertEqual(read_records(''), [])
                 self.assertEqual(read_records('a,b'), [['a', 'b']])
         ''', [("csv.reader(io.StringIO(text, newline=''))", 'csv.reader(text.splitlines())')])

    case('holdout_py_queue', 'python', 'stateful',
         'Queue uses integer priority (smaller first), with FIFO order for ties. Scheduling an existing '
         'key replaces its pending item; cancellation removes it. Stale heap entries must never run '
         'and must not consume a later replacement. pop returns (key, value), or None when empty.',
         '''
         import heapq
         class Queue:
             def __init__(self):
                 self.heap, self.current, self.serial = [], {}, 0
             def schedule(self, key, priority, value):
                 self.serial += 1
                 self.current[key] = (self.serial, value)
                 heapq.heappush(self.heap, (priority, self.serial, key))
             def cancel(self, key):
                 self.current.pop(key, None)
             def pop(self):
                 while self.heap:
                     _, serial, key = heapq.heappop(self.heap)
                     entry = self.current.get(key)
                     if entry is None or entry[0] != serial:
                         continue
                     del self.current[key]
                     return key, entry[1]
                 return None
         ''', '''
         import unittest
         from service import Queue
         class Queues(unittest.TestCase):
             def test_reschedule_does_not_consume_replacement(self):
                 q = Queue()
                 q.schedule('x', 0, 'old')
                 q.schedule('y', 1, 'other')
                 q.schedule('x', 2, 'new')
                 self.assertEqual(q.pop(), ('y', 'other'))
                 self.assertEqual(q.pop(), ('x', 'new'))
                 self.assertIsNone(q.pop())
             def test_cancel_readd_and_fifo(self):
                 q = Queue()
                 q.schedule('b', 1, 1); q.cancel('b'); q.schedule('a', 1, 2); q.schedule('b', 1, 3)
                 self.assertEqual(q.pop(), ('a', 2)); self.assertEqual(q.pop(), ('b', 3))
                 q.cancel('missing'); self.assertIsNone(q.pop())
         ''', [('entry = self.current.get(key)', 'entry = self.current.pop(key, None)'),
               ('del self.current[key]', '# Entry was removed while reading it.')])


if __name__ == '__main__':
    main()
