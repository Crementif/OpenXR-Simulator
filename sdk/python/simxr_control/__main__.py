import argparse
import json
from .client import Client

parser = argparse.ArgumentParser(description='Inspect an OpenXR Simulator local control endpoint')
parser.add_argument('--socket')
args = parser.parse_args()
with Client(args.socket) as client:
    print(json.dumps({'protocol': client.hello, 'sessions': [client.status(session) for session in client.sessions()]}, indent=2))
