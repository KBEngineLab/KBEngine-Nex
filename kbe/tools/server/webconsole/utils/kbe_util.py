from KBESettings import settings


def get_machines_address(hosts = None):
    if isinstance(settings.MACHINES_ADDRESS, (tuple, list)) and settings.MACHINES_ADDRESS:
        hosts = settings.MACHINES_ADDRESS

    if hosts is None:
        return "<broadcast>"
    return hosts