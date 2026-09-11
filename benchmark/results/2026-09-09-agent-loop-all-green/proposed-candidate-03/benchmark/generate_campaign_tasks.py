"""Generate the deterministic multi-file and multi-language campaign fixtures."""
import json
from pathlib import Path


GO_MOD = 'module campaign.test/fixture\n\ngo 1.24\n'
GO_VERIFY = ['go', 'test', '-json', './...']
PY_VERIFY = ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-v']


def task(task_id, language, category, prompt, files, oracle_files, protected, entry=None):
    value = {'id': task_id, 'suite': 'campaign', 'language': language,
             'category': category, 'prompt': prompt,
             'verify': GO_VERIFY if language == 'go' else PY_VERIFY,
             'protected_files': protected, 'files': files, 'oracle_files': oracle_files}
    if entry is not None:
        value['entry_files'] = entry
    return value


TASKS = [
    task('go_multifile_registry', 'go', 'multi-file',
         'Registry names are case-insensitive and ignore surrounding whitespace for both writes and reads. Repair the implementation across the repository and run go test -json ./... without modifying tests.',
         {'go.mod': GO_MOD,
          'names/normalize.go': '''package names

import "strings"

func Normalize(value string) string { return strings.TrimSpace(value) }
''',
          'registry/registry.go': '''package registry

import "campaign.test/fixture/names"

type Registry struct { values map[string]int }
func New() *Registry { return &Registry{values: map[string]int{}} }
func (r *Registry) Put(name string, value int) { r.values[names.Normalize(name)] = value }
func (r *Registry) Get(name string) (int, bool) { value, ok := r.values[name]; return value, ok }
''',
          'registry/registry_test.go': '''package registry

import "testing"
func TestNormalizedNames(t *testing.T) { r:=New(); r.Put("  Alpha ",7); for _,key:=range []string{"alpha"," ALPHA ","Alpha"} { if got,ok:=r.Get(key); !ok || got!=7 { t.Fatalf("%q=(%d,%v)",key,got,ok) } } }
'''},
         {'names/normalize.go': '''package names

import "strings"

func Normalize(value string) string { return strings.ToLower(strings.TrimSpace(value)) }
''',
          'registry/registry.go': '''package registry

import "campaign.test/fixture/names"

type Registry struct { values map[string]int }
func New() *Registry { return &Registry{values: map[string]int{}} }
func (r *Registry) Put(name string, value int) { r.values[names.Normalize(name)] = value }
func (r *Registry) Get(name string) (int, bool) { value, ok := r.values[names.Normalize(name)]; return value, ok }
'''}, ['registry/registry_test.go']),

    task('go_api_pagination', 'go', 'api',
         'The HTTP list endpoint must reject invalid page sizes with 400 and return exactly the requested window using one-based pages. Inspect the API code, repair it, and run go test -json ./... without modifying tests.',
         {'go.mod': GO_MOD,
          'api/handler.go': '''package api

import (
 "encoding/json"
 "net/http"
 "strconv"
)
func ListHandler(values []string) http.HandlerFunc { return func(w http.ResponseWriter,r *http.Request) {
 page,_:=strconv.Atoi(r.URL.Query().Get("page")); size,_:=strconv.Atoi(r.URL.Query().Get("size"))
 if page<1 { page=1 }; if size<1 { size=10 }
 start:=page*size; if start>len(values) { start=len(values) }; end:=start+size; if end>len(values) { end=len(values) }
 json.NewEncoder(w).Encode(values[start:end])
} }
''',
          'api/handler_test.go': '''package api

import ("encoding/json"; "net/http/httptest"; "reflect"; "testing")
func TestPagination(t *testing.T) { h:=ListHandler([]string{"a","b","c","d","e"}); r:=httptest.NewRequest("GET","/?page=2&size=2",nil); w:=httptest.NewRecorder(); h(w,r); var got []string; json.Unmarshal(w.Body.Bytes(),&got); if !reflect.DeepEqual(got,[]string{"c","d"}) { t.Fatalf("got %v",got) }; bad:=httptest.NewRequest("GET","/?page=1&size=nope",nil); bw:=httptest.NewRecorder(); h(bw,bad); if bw.Code!=400 { t.Fatalf("bad status %d",bw.Code) } }
'''},
         {'api/handler.go': '''package api

import (
 "encoding/json"
 "net/http"
 "strconv"
)
func ListHandler(values []string) http.HandlerFunc { return func(w http.ResponseWriter,r *http.Request) {
 page,errPage:=strconv.Atoi(r.URL.Query().Get("page")); size,errSize:=strconv.Atoi(r.URL.Query().Get("size"))
 if errPage!=nil || errSize!=nil || page<1 || size<1 { http.Error(w,"invalid pagination",http.StatusBadRequest); return }
 start:=(page-1)*size; if start>len(values) { start=len(values) }; end:=start+size; if end>len(values) { end=len(values) }
 json.NewEncoder(w).Encode(values[start:end])
} }
'''}, ['api/handler_test.go']),

    task('go_compiler_interface', 'go', 'compiler-failure',
         'The in-memory store must satisfy the Store interface and return records by string ID. The repository currently does not compile. Repair it and run go test -json ./... without modifying tests.',
         {'go.mod': GO_MOD,
          'store/store.go': '''package store

type Record struct { ID string; Value int }
type Store interface { Get(string) (Record,bool) }
type Memory struct { records map[string]Record }
func New(records []Record) *Memory { m:=&Memory{records:map[string]Record{}}; for _,r:=range records { m.records[r.ID]=r }; return m }
func (m *Memory) Get(id int) (Record,bool) { r,ok:=m.records[id]; return r,ok }
var _ Store = (*Memory)(nil)
''',
          'store/store_test.go': '''package store

import "testing"
func TestMemory(t *testing.T) { var s Store=New([]Record{{ID:"x",Value:9}}); got,ok:=s.Get("x"); if !ok || got.Value!=9 { t.Fatalf("got %v %v",got,ok) } }
'''},
         {'store/store.go': '''package store

type Record struct { ID string; Value int }
type Store interface { Get(string) (Record,bool) }
type Memory struct { records map[string]Record }
func New(records []Record) *Memory { m:=&Memory{records:map[string]Record{}}; for _,r:=range records { m.records[r.ID]=r }; return m }
func (m *Memory) Get(id string) (Record,bool) { r,ok:=m.records[id]; return r,ok }
var _ Store = (*Memory)(nil)
'''}, ['store/store_test.go']),

    task('go_explore_precedence', 'go', 'exploration',
         'Configuration resolution is wrong: explicit runtime overrides must win over file values, which must win over defaults. Locate the relevant implementation, repair precedence without changing the public API, and run go test -json ./... without modifying tests.',
         {'go.mod': GO_MOD,
          'config/types.go': '''package config

type Values map[string]string
''',
          'config/merge.go': '''package config

func Resolve(defaults,file,overrides Values) Values { out:=Values{}; for k,v:=range overrides { out[k]=v }; for k,v:=range file { out[k]=v }; for k,v:=range defaults { out[k]=v }; return out }
''',
          'config/merge_test.go': '''package config

import ("reflect"; "testing")
func TestPrecedence(t *testing.T) { got:=Resolve(Values{"host":"default","port":"80"},Values{"host":"file"},Values{"host":"runtime"}); want:=Values{"host":"runtime","port":"80"}; if !reflect.DeepEqual(got,want) { t.Fatalf("got %v",got) } }
'''},
         {'config/merge.go': '''package config

func Resolve(defaults,file,overrides Values) Values { out:=Values{}; for k,v:=range defaults { out[k]=v }; for k,v:=range file { out[k]=v }; for k,v:=range overrides { out[k]=v }; return out }
'''}, ['config/merge_test.go'], entry=[]),

    task('go_refactor_clock', 'go', 'refactor',
         'Refactor expiration checks to use the injected Clock everywhere so tests and production share one time source. Preserve the exported API and run go test -json ./... without modifying tests.',
         {'go.mod': GO_MOD,
          'session/clock.go': '''package session

import "time"
type Clock interface { Now() time.Time }
type SystemClock struct{}
func (SystemClock) Now() time.Time { return time.Now() }
''',
          'session/session.go': '''package session

import "time"
type Session struct { ExpiresAt time.Time; clock Clock }
func New(expires time.Time, clock Clock) Session { return Session{ExpiresAt:expires,clock:clock} }
func (s Session) Expired() bool { return time.Now().After(s.ExpiresAt) }
''',
          'session/session_test.go': '''package session

import ("testing"; "time")
type fixed struct{ at time.Time }; func (f fixed) Now() time.Time{return f.at}
func TestExpiredUsesClock(t *testing.T) { base:=time.Date(2030,1,1,0,0,0,0,time.UTC); if New(base.Add(time.Hour),fixed{base}).Expired() { t.Fatal("future marked expired") }; if !New(base.Add(-time.Hour),fixed{base}).Expired() { t.Fatal("past marked active") } }
'''},
         {'session/session.go': '''package session

import "time"
type Session struct { ExpiresAt time.Time; clock Clock }
func New(expires time.Time, clock Clock) Session { return Session{ExpiresAt:expires,clock:clock} }
func (s Session) Expired() bool { return s.clock.Now().After(s.ExpiresAt) }
'''}, ['session/session_test.go']),

    task('go_multifile_transfer', 'go', 'multi-file',
         'Transfer must atomically debit the source ledger and credit the destination ledger, returning an error without changing either ledger when funds are insufficient. Repair all affected files and run go test -json ./... without modifying tests.',
         {'go.mod': GO_MOD,
          'ledger/ledger.go': '''package ledger

type Ledger struct { Balance int }
func (l *Ledger) debit(amount int) bool { if amount<0 || l.Balance<=amount { return false }; l.Balance-=amount; return true }
''',
          'ledger/transfer.go': '''package ledger

import "errors"
func Transfer(from,to *Ledger, amount int) error { if !from.debit(amount) { return errors.New("insufficient funds") }; from.Balance+=amount; return nil }
''',
          'ledger/transfer_test.go': '''package ledger

import "testing"
func TestTransfer(t *testing.T) { a,b:=&Ledger{10},&Ledger{2}; if err:=Transfer(a,b,10); err!=nil || a.Balance!=0 || b.Balance!=12 { t.Fatalf("exact: %v %d %d",err,a.Balance,b.Balance) }; c,d:=&Ledger{3},&Ledger{4}; if err:=Transfer(c,d,5); err==nil || c.Balance!=3 || d.Balance!=4 { t.Fatalf("atomic: %v %d %d",err,c.Balance,d.Balance) } }
'''},
         {'ledger/ledger.go': '''package ledger

type Ledger struct { Balance int }
func (l *Ledger) debit(amount int) bool { if amount<0 || l.Balance<amount { return false }; l.Balance-=amount; return true }
''',
          'ledger/transfer.go': '''package ledger

import "errors"
func Transfer(from,to *Ledger, amount int) error { if !from.debit(amount) { return errors.New("insufficient funds") }; to.Balance+=amount; return nil }
'''}, ['ledger/transfer_test.go']),

    task('py_multifile_invoice', 'python', 'multi-file',
         'Invoice totals must apply the customer discount before adding the fixed shipping charge, and money must be rounded to two decimal places. Repair the implementation across modules and run the unittest suite without modifying tests.',
         {'billing/__init__.py': '',
          'billing/money.py': '''def money(value):
    return round(value, 1)
''',
          'billing/invoice.py': '''from .money import money

def total(subtotal, discount_rate, shipping):
    return money((subtotal + shipping) * (1 - discount_rate))
''',
          'tests/test_invoice.py': '''import unittest
from billing.invoice import total

class InvoiceTests(unittest.TestCase):
    def test_order_and_rounding(self):
        self.assertEqual(total(10.05, 0.10, 2.00), 11.05)
        self.assertEqual(total(1.26, 0, 0), 1.26)
'''},
         {'billing/money.py': '''def money(value):
    return round(value, 2)
''',
          'billing/invoice.py': '''from .money import money

def total(subtotal, discount_rate, shipping):
    return money(subtotal * (1 - discount_rate) + shipping)
'''}, ['tests/test_invoice.py']),

    task('py_api_query', 'python', 'api',
         'parse_query must accept a URL query string, keep blank values, return the last value for repeated keys, and reject malformed percent escapes with ValueError. Repair it and run the unittest suite without modifying tests.',
         {'web/__init__.py': '',
          'web/query.py': '''from urllib.parse import parse_qs

def parse_query(value):
    parsed = parse_qs(value)
    return {key: values[0] for key, values in parsed.items()}
''',
          'tests/test_query.py': '''import unittest
from web.query import parse_query

class QueryTests(unittest.TestCase):
    def test_contract(self):
        self.assertEqual(parse_query("a=1&a=2&empty="), {"a": "2", "empty": ""})
        with self.assertRaises(ValueError):
            parse_query("bad=%ZZ")
'''},
         {'web/query.py': '''from urllib.parse import parse_qs
import re

def parse_query(value):
    if re.search(r"%(?![0-9A-Fa-f]{2})", value):
        raise ValueError("malformed percent escape")
    parsed = parse_qs(value, keep_blank_values=True, strict_parsing=True)
    return {key: values[-1] for key, values in parsed.items()}
'''}, ['tests/test_query.py']),

    task('py_compiler_syntax', 'python', 'compiler-failure',
         'The expression module fails to import. Repair the syntax while preserving evaluate(expression), which must only support integer addition and reject other forms with ValueError. Run the unittest suite without modifying tests.',
         {'expr/__init__.py': '',
          'expr/evaluate.py': '''import ast

def evaluate(expression)
    node = ast.parse(expression, mode="eval").body
    if not isinstance(node, ast.BinOp) or not isinstance(node.op, ast.Add):
        raise ValueError("unsupported")
    if not isinstance(node.left, ast.Constant) or not isinstance(node.right, ast.Constant):
        raise ValueError("unsupported")
    return node.left.value + node.right.value
''',
          'tests/test_evaluate.py': '''import unittest
from expr.evaluate import evaluate

class EvaluateTests(unittest.TestCase):
    def test_addition_only(self):
        self.assertEqual(evaluate("2 + 5"), 7)
        with self.assertRaises(ValueError): evaluate("2 * 5")
        with self.assertRaises(ValueError): evaluate("'a' + 'b'")
'''},
         {'expr/evaluate.py': '''import ast

def evaluate(expression):
    node = ast.parse(expression, mode="eval").body
    if not isinstance(node, ast.BinOp) or not isinstance(node.op, ast.Add):
        raise ValueError("unsupported")
    if not all(isinstance(value, ast.Constant) and isinstance(value.value, int)
               for value in (node.left, node.right)):
        raise ValueError("unsupported")
    return node.left.value + node.right.value
'''}, ['tests/test_evaluate.py']),

    task('py_explore_slug', 'python', 'exploration',
         'Public slugs must be lowercase ASCII words separated by one hyphen, with punctuation removed, accents transliterated, and no leading or trailing separator. Locate and repair the relevant code, then run the unittest suite without modifying tests.',
         {'app/__init__.py': '', 'app/models.py': 'class Article:\n    pass\n',
          'app/text/__init__.py': '',
          'app/text/slug.py': '''import re

def slugify(value):
    return re.sub(r"\\s+", "-", value.strip().lower())
''',
          'tests/test_slug.py': '''import unittest
from app.text.slug import slugify

class SlugTests(unittest.TestCase):
    def test_public_form(self):
        self.assertEqual(slugify("  Café, déjà vu!  "), "cafe-deja-vu")
        self.assertEqual(slugify("one---two"), "one-two")
'''},
         {'app/text/slug.py': '''import re
import unicodedata

def slugify(value):
    normalized = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode()
    return re.sub(r"[^a-z0-9]+", "-", normalized.lower()).strip("-")
'''}, ['tests/test_slug.py'], entry=[]),

    task('py_refactor_repository', 'python', 'refactor',
         'UserService must depend only on the repository protocol, not MemoryRepository internals, and missing users must return None. Refactor the implementation without changing the public service API and run the unittest suite without modifying tests.',
         {'users/__init__.py': '',
          'users/repository.py': '''class MemoryRepository:
    def __init__(self, users):
        self.users = {user["id"]: user for user in users}

    def find(self, user_id):
        return self.users.get(user_id)
''',
          'users/service.py': '''from .repository import MemoryRepository

class UserService:
    def __init__(self, repository):
        self.repository = repository

    def display_name(self, user_id):
        return self.repository.users[user_id]["name"].strip().title()
''',
          'tests/test_service.py': '''import unittest
from users.service import UserService

class StubRepository:
    def find(self, user_id):
        return {"id": user_id, "name": "  ada lovelace "} if user_id == "a" else None

class ServiceTests(unittest.TestCase):
    def test_protocol_and_missing(self):
        service = UserService(StubRepository())
        self.assertEqual(service.display_name("a"), "Ada Lovelace")
        self.assertIsNone(service.display_name("missing"))
'''},
         {'users/service.py': '''class UserService:
    def __init__(self, repository):
        self.repository = repository

    def display_name(self, user_id):
        user = self.repository.find(user_id)
        return user["name"].strip().title() if user is not None else None
'''}, ['tests/test_service.py']),

    task('py_graph_cycle', 'python', 'algorithm',
         'topological_order must return every node after its dependencies, include dependency-only nodes, and raise ValueError on cycles. Repair it and run the unittest suite without modifying tests.',
         {'graph/__init__.py': '',
          'graph/order.py': '''def topological_order(graph):
    result, visiting = [], set()
    def visit(node):
        if node in visiting:
            return
        visiting.add(node)
        for dependency in graph.get(node, []):
            visit(dependency)
        result.append(node)
    for node in graph:
        visit(node)
    return result
''',
          'tests/test_order.py': '''import unittest
from graph.order import topological_order

class OrderTests(unittest.TestCase):
    def test_dependencies_and_cycles(self):
        got=topological_order({"build":["compile"],"compile":["generate"],"test":["compile"]})
        self.assertEqual(set(got),{"build","compile","generate","test"})
        self.assertLess(got.index("generate"),got.index("compile")); self.assertLess(got.index("compile"),got.index("build"))
        with self.assertRaises(ValueError): topological_order({"a":["b"],"b":["a"]})
'''},
         {'graph/order.py': '''def topological_order(graph):
    result, visiting, visited = [], set(), set()
    def visit(node):
        if node in visiting:
            raise ValueError("cycle")
        if node in visited:
            return
        visiting.add(node)
        for dependency in graph.get(node, []):
            visit(dependency)
        visiting.remove(node)
        visited.add(node)
        result.append(node)
    for node in graph:
        visit(node)
    return result
'''}, ['tests/test_order.py']),

    task('py_atomic_batch', 'python', 'multi-file',
         'apply_batch must validate all inventory adjustments before mutating state: unknown SKUs and negative final quantities reject the whole batch. Repair it and run the unittest suite without modifying tests.',
         {'inventory/__init__.py': '',
          'inventory/model.py': '''class Inventory:
    def __init__(self, quantities):
        self.quantities = dict(quantities)
''',
          'inventory/batch.py': '''def apply_batch(inventory, adjustments):
    for sku, delta in adjustments:
        if sku not in inventory.quantities:
            return False
        inventory.quantities[sku] += delta
        if inventory.quantities[sku] < 0:
            return False
    return True
''',
          'tests/test_batch.py': '''import unittest
from inventory.model import Inventory
from inventory.batch import apply_batch

class BatchTests(unittest.TestCase):
    def test_atomicity(self):
        stock=Inventory({"a":3,"b":1}); self.assertTrue(apply_batch(stock,[("a",-2),("b",2)])); self.assertEqual(stock.quantities,{"a":1,"b":3})
        stock=Inventory({"a":3,"b":1}); self.assertFalse(apply_batch(stock,[("a",-2),("b",-4)])); self.assertEqual(stock.quantities,{"a":3,"b":1})
        self.assertFalse(apply_batch(stock,[("missing",1)])); self.assertEqual(stock.quantities,{"a":3,"b":1})
'''},
         {'inventory/batch.py': '''def apply_batch(inventory, adjustments):
    proposed = dict(inventory.quantities)
    for sku, delta in adjustments:
        if sku not in proposed or proposed[sku] + delta < 0:
            return False
        proposed[sku] += delta
    inventory.quantities.clear()
    inventory.quantities.update(proposed)
    return True
'''}, ['tests/test_batch.py']),
]


def main():
    destination = Path(__file__).parent / 'tasks'
    destination.mkdir(exist_ok=True)
    for value in TASKS:
        (destination / f'{value["id"]}.json').write_text(
            json.dumps(value, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
