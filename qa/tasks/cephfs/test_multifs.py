"""
Test for multiple CephFSs on the Ceph cluster.
"""
import logging

from tasks.cephfs.mount import CephFSMount
from tasks.cephfs.cephfs_test_case import CephFSTestCase
from tasks.cephfs.filesystem import Filesystem

log = logging.getLogger(__name__)


class TestMultiFSClients(CephFSTestCase):
    # one dedicated and one standby for each FS
    MDSS_REQUIRED = 2
    CLIENTS_REQUIRED = 2

    def setUp(self):
        super(TestMultiFSClients, self).setUp()

        self.mount_a.umount_wait()
        self.mount_b.umount_wait()

        self.enable_multifs()
        self.fs1 = self.fs
        self.fs2 = self.mds_cluster.newfs(name='b', create=True)

        self.user1_name = 'user_a'
        self.user2_name = 'user_b'
        self.user1_keyring = self.create_and_auth_user(self.fs1.name,
            self.user1_name, '/', 'rw')
        self.user2_keyring = self.create_and_auth_user(self.fs2.name,
            self.user2_name, '/', 'rw')

class TestClientWithAuth(TestMultiFSClients):
    def setUp(self):
        super(TestClientWithAuth, self).setUp()

        # XXX: change client id and keyring to the user created specifically
        # for the test
        self.mnt_a_orig_id = self.mount_a.client_id
        self.mnt_a_orig_keyring = self.mount_a.client_keypath
        self.mount_a.client_id = self.user1_name
        self.mount_a.client_keypath = self.user1_keyring
        self.mount_a.mount(mount_fs_name=self.fs1.name, createfs=False)

        self.mnt_b_orig_id = self.mount_b.client_id
        self.mnt_b_orig_keyring = self.mount_b.client_keypath
        self.mount_b.client_id = self.user2_name
        self.mount_b.client_keypath = self.user2_keyring
        self.mount_b.mount(mount_fs_name=self.fs2.name, createfs=False)

    def tearDown(self):
        # XXX: reset client id and keyring; important for successful teardown
        self.mount_a.umount_wait()
        self.mount_a.client_id = self.mnt_a_orig_id
        self.mount_a.client_keypath = self.mnt_a_orig_keyring
        self.mount_b.umount_wait()
        self.mount_b.client_id = self.mnt_b_orig_id
        self.mount_b.client_keypath = self.mnt_b_orig_keyring

    def test_rw(self):
        """
        Test read/write for a client with authorization.
        """
        testfile = 'somefile'
        testdata = 'somedata'
        self.mount_a.run_shell(['sudo', 'sh', '-c', 'cat > ' + testfile],
                               stdin=testdata)

        dataread = self.mount_a.run_shell(['cat', testfile]).stdout.\
            getvalue().strip()
        self.assertEqual(testdata, dataread)

    def test_empty_file_creation(self):
        testfile = 'somefile'
        self.mount_a.postestcmd(['touch', testfile])


class TestClientsWithOutAuth(TestMultiFSClients):

    def setUp(self):
        super(TestClientsWithOutAuth, self).setUp()

        self.mnt_a_orig_id = self.mount_a.client_id
        self.mnt_a_orig_keyring = self.mount_a.client_keypath
        self.mount_a.client_id = self.user1_name
        self.mount_a.client_keypath = self.user1_keyring
        # XXX: mounting with user that doesn't have right auth
        self.mount_a.mount(mount_fs_name=self.fs2.name, createfs=False)

        self.mnt_b_orig_id = self.mount_b.client_id
        self.mnt_b_orig_keyring = self.mount_b.client_keypath
        self.mount_b.client_id = self.user2_name
        self.mount_b.client_keypath = self.user2_keyring
        self.mount_b.mount(mount_fs_name=self.fs2.name, createfs=False)

    def tearDown(self):
        self.mount_a.umount_wait()
        self.mount_a.client_id = self.mnt_a_orig_id
        self.mount_a.client_keypath = self.mnt_a_orig_keyring

        self.mount_b.umount_wait()
        self.mount_b.client_id = self.mnt_b_orig_id
        self.mount_b.client_keypath = self.mnt_b_orig_keyring

    def test_read(self):
        """
        Test read for a client without authorization.
        """
        testfile = 'somefile'
        testdata = 'somedata'
        self.mount_b.run_shell(['sudo', 'sh', '-c', 'cat > ' + testfile],
                               stdin=testdata)

        self.mount_a.negtestcmd(['cat', testfile], retval=1,
                                errmsg='operation not permitted')

    def test_write(self):
        """
        Test write for a client without authorization.
        """
        testfile = 'somefile'
        testdata = 'somedata'
        self.mount_a.negtestcmd(['sudo', 'sh', '-c', 'cat >' + testfile],
                                stdin=testdata, retval=1,
                                errmsg='operation not permitted')

    def test_empty_file_creation(self):
        testfile = 'somefile'
        self.mount_a.postestcmd(['touch', testfile])
