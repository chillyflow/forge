def positions_for(messages):
    entries, positions = {}, {}
    processed = set()
    cancelled = set()
    
    for message in messages:
        # Event IDs are globally idempotent
        if message['key'] in processed:
            continue
        processed.add(message['key'])
        
        if message['kind'] == 'post':
            bucket, delta = message['bucket'], message['delta']
            entries[message['key']] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
        else:  # message['kind'] == 'retract'
            reference = message['reference']
            # A retraction cancels its referenced posting exactly once
            if reference in cancelled:
                continue
            cancelled.add(reference)
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
                del entries[reference]
    
    return positions
