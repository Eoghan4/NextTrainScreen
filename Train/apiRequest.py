import requests

def apiRequest(target_station):
    return requests.post(f'https://api.irishtnt.com/stations/name/{target_station.lower()}/90').json()