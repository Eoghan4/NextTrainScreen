import apiRequest
import trainInfo
from apiRequest import apiRequest
from trainInfo import getStationDirections, getNextTrainForGivenDirection

TARGET_STATION = 'Malahide'

api_response = apiRequest(TARGET_STATION)

directions = getStationDirections(TARGET_STATION, api_response)

print(f"Next trains from {TARGET_STATION}")

for direction in directions:
    next_train_info = getNextTrainForGivenDirection(TARGET_STATION, api_response, direction)
    
    if next_train_info is None:
        print(f"No {direction.lower()} trains")
    else:
        destination, actual_due = next_train_info
        print(f"{direction}: {destination}\t\t{actual_due} minutes")