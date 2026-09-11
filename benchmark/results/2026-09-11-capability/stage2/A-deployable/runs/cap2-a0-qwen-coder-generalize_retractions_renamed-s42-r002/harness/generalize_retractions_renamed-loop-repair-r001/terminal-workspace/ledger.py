def positions_for(messages):
    entries, positions = {}, {}
    voided = set()
    
    for message in messages:
        if message['kind'] == 'post':
            bucket, delta = message['bucket'], message['delta']
            # If this post has been voided by a previous retraction, skip it
            if message['key'] in voided:
                continue
            entries[message['key']] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
        else:  # message['kind'] == 'retract'
            reference = message['reference']
            # Track that we've seen this reference (even if post doesn't exist yet)
            # This ensures bucket gets initialized
            if reference not in voided:
                voided.add(reference)
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
                del entries[reference]
    
    return positions
