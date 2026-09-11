def positions_for(messages):
    visited, voided = set(), set()
    entries, positions = {}, {}
    for message in messages:
        if message['key'] in visited:
            continue
        visited.add(message['key'])
        if message['kind'] == 'post':
            bucket, delta = message['bucket'], message['delta']
            # If this post was already voided by a retraction, skip it
            if message['key'] in voided:
                continue
            entries[message['key']] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
        else:
            reference = message['reference']
            # If we've already processed this retraction, skip it
            if reference in voided:
                continue
            # Mark that this reference has been retracted
            voided.add(reference)
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
                # Remove the entry to prevent double application
                del entries[reference]
    return positions
