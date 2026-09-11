"""Recreate the small, reviewable Go correctness fixtures (no model calls)."""
import json
from pathlib import Path

CASES = [
    ('add', 'Add must add two integers, including negative operands.',
     'func Add(a, b int) int { return a - b }',
     'if got := Add(2,3); got != 5 { t.Fatalf("expected 5, got %d", got) }; if Add(-2,3) != 1 { t.Fatal("negative operands") }'),
    ('clamp', 'Clamp must restrict x to the inclusive [lo, hi] interval.',
     'func Clamp(x, lo, hi int) int { if x < lo { return hi }; if x > hi { return lo }; return x }',
     'for _, c := range [][4]int{{-1,0,10,0},{11,0,10,10},{5,0,10,5},{0,0,10,0}} { if got:=Clamp(c[0],c[1],c[2]); got!=c[3] { t.Fatalf("%v: got %d",c,got) } }'),
    ('contains', 'Contains returns whether an integer is present, including the last element.',
     'func Contains(xs []int, target int) bool { for i:=0; i<len(xs)-1; i++ { if xs[i]==target { return true } }; return false }',
     'if !Contains([]int{1,2,3},3) || !Contains([]int{7},7) || Contains(nil,1) || Contains([]int{1},2) { t.Fatal("membership failed") }'),
    ('last_index', 'LastIndex returns the last matching index, or -1 when absent.',
     'func LastIndex(xs []int, target int) int { for i,v:=range xs { if v==target { return i } }; return -1 }',
     'if LastIndex([]int{2,1,2},2)!=2 || LastIndex(nil,0)!=-1 || LastIndex([]int{3},4)!=-1 { t.Fatal("last occurrence failed") }'),
    ('ceil_div', 'CeilDiv rounds a nonnegative integer divided by a positive integer upward.',
     'func CeilDiv(a, b int) int { return a / b }',
     'for _,c:=range [][3]int{{0,3,0},{1,3,1},{6,3,2},{7,3,3}} { if CeilDiv(c[0],c[1])!=c[2] { t.Fatalf("%v",c) } }'),
    ('reverse', 'Reverse reverses a slice in place without dropping the middle element.',
     'func Reverse(xs []int) { for i,j:=0,len(xs)-1; i<j; i,j=i+1,j-1 { xs[i]=xs[j]; xs[j]=xs[i] } }',
     'xs:=[]int{1,2,3,4,5}; Reverse(xs); for i,v:=range xs { if v!=5-i { t.Fatalf("got %v",xs) } }; Reverse(nil)'),
    ('unique', 'Unique removes duplicates while preserving the order of first appearances.',
     'func Unique(xs []int) []int { out:=[]int{}; seen:=map[int]bool{}; for _,v:=range xs { if seen[v] { out=append(out,v) }; seen[v]=true }; return out }',
     'got:=Unique([]int{3,1,3,2,1}); want:=[]int{3,1,2}; if len(got)!=len(want) { t.Fatalf("got %v",got) }; for i:=range want { if got[i]!=want[i] { t.Fatalf("got %v",got) } }'),
    ('range_sum', 'RangeSum returns the sum from lo through hi inclusive; empty ranges return zero.',
     'func RangeSum(lo,hi int) int { sum:=0; for i:=lo;i<hi;i++ { sum+=i }; return sum }',
     'if RangeSum(1,3)!=6 || RangeSum(5,5)!=5 || RangeSum(3,1)!=0 || RangeSum(-2,2)!=0 { t.Fatal("inclusive sum failed") }'),
    ('average', 'Average returns a floating point mean, and returns zero for an empty slice.',
     'func Average(xs []int) float64 { sum:=0; for _,v:=range xs { sum+=v }; return float64(sum/len(xs)) }',
     'if Average([]int{1,2})!=1.5 { t.Fatal("fraction truncated") }; if Average(nil)!=0 { t.Fatal("empty input") }'),
    ('prefix', 'TrimPrefix removes exactly one leading prefix; otherwise leave the string unchanged.',
     'import "strings"\nfunc TrimPrefix(s,prefix string) string { return strings.ReplaceAll(s,prefix,"") }',
     'if TrimPrefix("abab","ab")!="ab" || TrimPrefix("zab","ab")!="zab" || TrimPrefix("abc","")!="abc" { t.Fatal("prefix semantics failed") }'),
]

def main():
    destination = Path(__file__).parent / 'tasks'
    destination.mkdir(exist_ok=True)
    for name, prompt, source, assertion in CASES:
        manifest = {
            'id': name,
            'prompt': prompt + ' Inspect the source, fix the bug, and run go test -json ./... to verify. Do not modify tests.',
            'verify': ['go', 'test', '-json', './...'],
            'files': {'go.mod': 'module forgebench\n\ngo 1.24\n', 'repair.go': 'package repair\n\n' + source + '\n',
                      'repair_test.go': 'package repair\n\nimport "testing"\nfunc TestRepair(t *testing.T) { ' + assertion + ' }\n'},
        }
        (destination / f'{name}.json').write_text(json.dumps(manifest, indent=2) + '\n')

if __name__ == '__main__':
    main()
