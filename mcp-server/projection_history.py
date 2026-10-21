"""Compatibility projection logs from bounded submitted CPU frame history."""
from debug_control import _json_result
from legacy_control import SessionBinding


class ProjectionHistory:
    def __init__(self, directory):
        self._binding = SessionBinding(directory, require_commands=False)

    def close(self):
        self._binding.close()

    def read(self, count):
        try:
            if not self._binding.connect():
                return None
            from simxr_control import Capability
            client, session = self._binding.client, self._binding.session
            if client.sessions() != [session]:
                self._binding.close()
                raise RuntimeError('Projection history session changed; reconnect explicitly')
            if not client.status(session)['capabilities'] & Capability.PROJECTION_HISTORY:
                return None
            document = client.projection_history(session, count=count)['document']
            entries = []
            for record in document['entries']:
                views = record['projection']['views']
                entry = {'frame': str(record['frame_id']), 'displayTime': str(record['display_time']),
                         'layer': record['layer'], 'ipd': record['ipd'], 'view_count': len(views),
                         'projection': _json_result(record['projection'])}
                if views:
                    pose = views[0]['pose_in_space']
                    entry['pose'] = dict(pose['position'], **{'q' + key: value for key, value in pose['orientation'].items()})
                for name, view in zip(('left', 'right'), views):
                    fov, rectangle = view['fov'], view['color']['rectangle']
                    entry[name + '_fov'] = {short: fov[full] for short, full in
                                           (('aL', 'left'), ('aR', 'right'), ('aU', 'up'), ('aD', 'down'))}
                    entry[name + '_rect'] = [rectangle[key] for key in ('x', 'y', 'width', 'height')]
                entries.append(entry)
            return dict(_json_result(document), entries=entries, transport='frame_history', session=str(session.id))
        except BaseException as error:
            if self._binding.is_uncertain(error):
                self._binding.invalidate(error)
            raise
