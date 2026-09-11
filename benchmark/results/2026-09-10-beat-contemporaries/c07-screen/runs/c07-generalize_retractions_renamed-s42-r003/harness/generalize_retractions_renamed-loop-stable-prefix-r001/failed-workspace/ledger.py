def positions_for(messages):
    entries, positions = {}, {}
    voided = set()
    
    for message in messages:
        if message['kind'] == 'post':
            bucket, delta = message['bucket'], message['delta']
            # If this posting was voided by a previous retraction, don't add it
            if message['key'] in voided:
                continue
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
