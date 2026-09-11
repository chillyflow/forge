def positions_for(messages):
    visited, voided = set(), set()
    entries, positions = {}, {}
    for message in messages:
        if message['key'] in visited:
            continue
        visited.add(message['key'])
        if message['kind'] == 'post':
            # If this post has been voided by a retraction, don't process it
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
            # If reference is not in entries, we still mark it as voided to prevent double cancellation
            # But we don't modify positions in this case - this is a "unmatched retraction"
    return positions
