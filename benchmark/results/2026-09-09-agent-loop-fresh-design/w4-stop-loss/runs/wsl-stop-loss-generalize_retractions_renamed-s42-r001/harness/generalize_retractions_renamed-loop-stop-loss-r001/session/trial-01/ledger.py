def positions_for(messages):
    visited, voided = set(), set()
    entries, positions = {}, {}
    # Track retractions that reference postings that haven't been seen yet
    pending_retractions = {}
    
    for message in messages:
        if message['key'] in visited:
            continue
        visited.add(message['key'])
        if message['kind'] == 'post':
            bucket, delta = message['bucket'], message['delta']
            entries[message['key']] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
            # Check if there are pending retractions for this posting
            if message['key'] in pending_retractions:
                # Apply all pending retractions for this posting
                for retraction_key in pending_retractions[message['key']]:
                    positions[bucket] -= delta
                del pending_retractions[message['key']]
        else:
            reference = message['reference']
            if reference in voided:
                continue
            voided.add(reference)
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
            else:
                # If the referenced posting hasn't been seen yet, store this retraction
                # for later processing
                if reference not in pending_retractions:
                    pending_retractions[reference] = []
                pending_retractions[reference].append(message['key'])
    return positions
