def positions_for(messages):
    visited, voided = set(), set()
    entries, positions = {}, {}
    for message in messages:
        if message['key'] in visited:
            continue
        visited.add(message['key'])
        if message['kind'] == 'post':
            # If this post was previously marked as voided by a retraction, skip it
            if message['key'] in voided:
                # Even if voided, we still need to ensure the bucket exists in positions
                # to maintain the invariant that accounts with canceled entries retain zero entry
                positions.setdefault(message['bucket'], 0)
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
