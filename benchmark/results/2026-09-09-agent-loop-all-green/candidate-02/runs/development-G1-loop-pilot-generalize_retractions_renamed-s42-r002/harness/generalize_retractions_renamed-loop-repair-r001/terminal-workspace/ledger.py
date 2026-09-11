def positions_for(messages):
    entries, positions = {}, {}
    voided = set()
    
    for message in messages:
        if message['kind'] == 'post':
            # Only process the first post with this key (idempotency)
            if message['key'] in entries:
                continue
            # If this post was marked for retraction, don't process it
            if message['key'] in voided:
                continue
            bucket, delta = message['bucket'], message['delta']
            entries[message['key']] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
        else:  # message['kind'] == 'retract'
            reference = message['reference']
            # Only process the first retraction of a reference (idempotency)
            if reference in voided:
                continue
            voided.add(reference)
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
                del entries[reference]
    
    return positions
