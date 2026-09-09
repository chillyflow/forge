def requirements(graph, targets):
    visited = set()
    
    def visit(node):
        if node not in graph:
            raise ValueError('missing job')
        
        if node in visited:
            return
            
        # Mark as being visited (for cycle detection)
        visited.add(node)
        
        # Visit all dependencies
        for dependency in graph[node]:
            visit(dependency)
    
    targets = set(targets)
    for target in targets:
        visit(target)
    
    return visited - targets
