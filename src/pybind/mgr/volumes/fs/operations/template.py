from ..exception import VolumeException

class GroupTemplate(object):
    def list_subvolumes(self):
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")

    def create_snapshot(self, snapname):
        """
        create a subvolume group snapshot.

        :param: group snapshot name
        :return: None
        """
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")

    def remove_snapshot(self, snapname):
        """
        remove a subvolume group snapshot.

        :param: group snapshot name
        :return: None
        """
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")

    def list_snapshots(self):
        """
        list all subvolume group snapshots.

        :param: None
        :return: None
        """
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")

class SubvolumeTemplate(object):
    VERSION = None

    @staticmethod
    def version():
        return SubvolumeTemplate.VERSION

    def open(self):
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")

    def create(self, size, isolate_nspace, pool, mode, uid, gid):
        """
        set up metadata, pools and auth for a subvolume.

        This function is idempotent.  It is safe to call this again
        for an already-created subvolume, even if it is in use.

        :param size: In bytes, or None for no size limit
        :param isolate_nspace: If true, use separate RADOS namespace for this subvolume
        :param pool: the RADOS pool where the data objects of the subvolumes will be stored
        :param mode: the user permissions
        :param uid: the user identifier
        :param gid: the group identifier
        :return: None
        """
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")

    def remove(self):
        """
        make a subvolume inaccessible to guests.

        This function is idempotent.  It is safe to call this again

        :param: None
        :return: None
        """
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")

    def resize(self, newsize, nshrink):
        """
        resize a subvolume

        :param newsize: new size In bytes (or inf/infinite)
        :return: new quota size and used bytes as a tuple
        """
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")

    def create_snapshot(self, snapname):
        """
        snapshot a subvolume.

        :param: subvolume snapshot name
        :return: None
        """
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")

    def remove_snapshot(self, snapname):
        """
        remove a subvolume snapshot.

        :param: subvolume snapshot name
        :return: None
        """
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")

    def list_snapshots(self):
        """
        list all subvolume snapshots.

        :param: None
        :return: None
        """
        raise VolumeException(-errno.ENOTSUP, "operation not supported.")
