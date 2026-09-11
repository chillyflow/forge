def positions_for(messages):
    visited, voided = set(), set()
    entries, positions = {}, {}
    for message in messages:
        if message['key'] in visited:
            continue
        visited.add(message['key'])
        if message['kind'] == 'post':
            # If this posting has been voided, skip it
            if message['key'] in voided:
                continue
            bucket, delta = message['bucket'], message['delta']
            entries[message['key']] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
        else:
            reference = message['reference']
            if reference in voided:
                continue
            voided.add(reference)
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
    return positions
