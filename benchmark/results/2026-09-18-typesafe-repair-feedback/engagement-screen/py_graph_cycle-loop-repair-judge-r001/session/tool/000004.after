def topological_order(graph):
    result, visiting, visited = [], set(), set()
    
    def visit(node):
        if node in visiting:
            raise ValueError("Cycle detected")
        if node in visited:
            return
        visiting.add(node)
        for dependency in graph.get(node, []):
            visit(dependency)
        visiting.remove(node)
        visited.add(node)
        result.append(node)
    
    # Get all nodes that appear as dependencies (even if they don't have dependencies themselves)
    all_nodes = set(graph.keys())
    for dependencies in graph.values():
        all_nodes.update(dependencies)
    
    for node in all_nodes:
        if node not in visited:
            visit(node)
    
    return result
