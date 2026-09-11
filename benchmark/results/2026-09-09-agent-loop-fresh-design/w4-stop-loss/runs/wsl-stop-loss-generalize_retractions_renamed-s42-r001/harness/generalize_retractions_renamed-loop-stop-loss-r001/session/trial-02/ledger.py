def positions_for(messages):
    visited, voided = set(), set()
    entries, positions = {}, {}
    for message in messages:
        if message['key'] in visited:
            continue
        visited.add(message['key'])
        if message['kind'] == 'post':
            bucket, delta = message['bucket'], message['delta']
            # If this posting has been voided by a retraction, skip it
            if message['key'] in voided:
                # Ensure the bucket exists even if the posting is voided
                positions.setdefault(bucket, 0)
                continue
            entries[message['key']] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
        else:
            reference = message['reference']
            # Mark the referenced posting as voided
            voided.add(reference)
            # If the referenced posting has already been processed, adjust balance
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
    return positions
