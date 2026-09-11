def positions_for(messages):
    visited, voided = set(), set()
    entries, positions = {}, {}
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
        else:
            reference = message['reference']
            if reference in voided:
                continue
            voided.add(reference)
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
            else:
                # Store the retraction for later processing
                if reference not in pending_retractions:
                    pending_retractions[reference] = []
                pending_retractions[reference].append(message)
    
    # Process any pending retractions that might have been applied to posts that came later
    for key, retractions in pending_retractions.items():
        if key in entries:
            bucket, delta = entries[key]
            for retraction in retractions:
                positions[bucket] -= delta
    
    return positions
