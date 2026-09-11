def positions_for(messages):
    visited, voided = set(), set()
    pending_retractions = {}
    entries, positions = {}, {}
    for message in messages:
        if message['key'] in visited:
            continue
        visited.add(message['key'])
        if message['kind'] == 'post':
            bucket, delta = message['bucket'], message['delta']
            reference = message['key']
            
            # Check if this posting should be voided by pending retractions
            should_void = reference in pending_retractions
            
            # Apply any pending retractions for this posting
            if should_void:
                for retract_key in pending_retractions[reference]:
                    if retract_key not in voided:
                        voided.add(retract_key)
                        positions.setdefault(bucket, 0)
                        positions[bucket] -= delta
                del pending_retractions[reference]
            
            # Only add the posting if it hasn't been voided
            if not should_void:
                entries[reference] = (bucket, delta)
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
                del entries[reference]  # Remove the entry after voiding
            else:
                # Store as pending retraction if the reference hasn't been seen yet
                pending_retractions.setdefault(reference, []).append(message['key'])
    return positions
