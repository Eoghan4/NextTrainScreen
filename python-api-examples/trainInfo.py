import apiRequest

def getStationDirections(target_station, stationInfo):
    trains = stationInfo['station']['trains']

    directions = []
    for train in trains:
        if train['destination'] != f'{target_station}' and train['direction'] not in directions:
            directions.append(train['direction'])
    return directions

def getNextTrainForGivenDirection(target_station, stationInfo, direction):
    trains = stationInfo['station']['trains']

    next_train = None

    for train in trains:
        if train['destination'] == target_station:
            continue
        
        if train['direction'] == direction and next_train is None:
            next_train = train

    if next_train is None:
        return None
    else:
        actual_due = int(next_train['dueIn']) + int(next_train['late'])
        return (next_train['destination'], actual_due)
