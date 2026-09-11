def positions_for(messages):
    visited, cancelled = set(), set()
    entries, positions = {}, {}
    for message in messages:
        if message['key'] in visited:
            continue
        visited.add(message['key'])
        if message['kind'] == 'post':
            bucket, delta = message['bucket'], message['delta']
            entries[message['key']] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
        else:
            reference = message['reference']
            if reference in cancelled:
                continue
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
                cancelled.add(reference)
            else:
                # If the referenced post hasn't arrived yet, mark it as cancelled
                # to prevent multiple cancellations when it arrives
                cancelled.add(reference)
    return positions
