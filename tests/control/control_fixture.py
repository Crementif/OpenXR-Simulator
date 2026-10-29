from copy import deepcopy
import math

from simxr_control import Capability, Session


def configuration():
    return {'head': {'position': [2, 1.7, 0], 'orientation': [0, math.sqrt(.5), 0, math.sqrt(.5)]},
            'controllers': [{'controls': {'trigger': .5, 'grip': .25, 'primary': False,
                'stick': [.2, -.3], 'trackpad': [.4, .6], 'connected': True}} for _ in range(2)],
            'views': {'ipd': .067, 'outer_fov': [-1, 1, 1, -1] * 2, 'inset_fov': [-.2, .2, .2, -.2] * 2}}


class Endpoint:
    def __init__(self):
        self.hello = {'capabilities': Capability.SIMULATION_COMMANDS}
        self.current_session = Session(17, 23)
        self.frame = 2**53 + 9
        self.state = configuration()
        self.receipts = {}
        self.scheduled = []
        self.observed = []
        self.closed = 0
        self.schedule_error = None
        self.status_error = None

    def sessions(self):
        return [self.current_session]

    def simulation_status(self, session, transaction=None):
        if self.status_error:
            raise self.status_error
        self.observed.append((session, transaction))
        if transaction is not None:
            return deepcopy(self.receipts[transaction])
        return {'document': {'frame_id': self.frame, 'configuration': deepcopy(self.state)}}

    def schedule(self, session, commands, **options):
        self.scheduled.append((session, deepcopy(commands), options))
        if self.schedule_error:
            raise self.schedule_error
        identity = 2**63 + len(self.scheduled)
        result = {'transaction_id': identity, 'phase': 'queued',
                  'document': {'target_frame': options['frame']}}
        self.receipts[identity] = result
        return deepcopy(result)

    def close(self):
        self.closed += 1
