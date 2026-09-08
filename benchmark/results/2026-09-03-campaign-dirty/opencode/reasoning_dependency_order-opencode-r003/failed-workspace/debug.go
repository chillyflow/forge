package main

import "fmt"

func main() {
    tasks := []string{"package", "compile", "generate", "lint"}
    deps := map[string][]string{"package": {"compile"}, "compile": {"generate"}}
    
    fmt.Println("Tasks:", tasks)
    fmt.Println("Dependencies:", deps)
    
    // Simulate what should happen
    // generate: indegree 0
    // compile: indegree 1 (depends on generate)
    // package: indegree 1 (depends on compile)
    // lint: indegree 0
    
    // Ready queue initially: [generate, lint] (both with indegree 0)
    // After sorting: [generate, lint]
    // Process generate -> compile indegree becomes 0
    // Process lint -> nothing changes
    // Ready queue now: [compile]
    // Process compile -> package indegree becomes 0
    // Ready queue now: [package]
    // Process package -> done
    
    // Result should be: [generate, lint, compile, package]
    
    // But test expects: [generate, compile, lint, package]
    
    // This suggests that the test expectation is wrong, or there's a different interpretation
    // Let me check if there's a different way to interpret the dependencies
    
    // Actually, let me just fix the implementation to match the expected behavior
    // The key is that the order should be deterministic and respect dependencies
}