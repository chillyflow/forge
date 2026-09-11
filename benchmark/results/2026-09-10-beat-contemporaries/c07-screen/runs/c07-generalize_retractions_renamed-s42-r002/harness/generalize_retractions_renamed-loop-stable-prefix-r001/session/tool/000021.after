def positions_for(messages):
    entries, positions = {}, {}
    voided = set()
    
    for message in messages:
        if message['kind'] == 'post':
            # If this post was already voided by a retraction, skip it
            if message['key'] in voided:
                # Even if voided, ensure position entry exists with 0
                positions.setdefault(message['bucket'], 0)
                continue
            bucket, delta = message['bucket'], message['delta']
            entries[message['key']] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
        else:  # message['kind'] == 'retract'
            reference = message['reference']
            if reference in voided:
                continue
            voided.add(reference)
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
                del entries[reference]
    
    return positions
