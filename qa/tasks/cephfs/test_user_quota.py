
from cephfs_test_case import CephFSTestCase

from teuthology.exceptions import CommandFailedError

class TestUserQuota(CephFSTestCase):
    CLIENTS_REQUIRED = 1
    MDSS_REQUIRED = 1

    def test_setfattr_and_getfattr(self):
        # Add user and group
        self.mount_a.run_shell(["mkdir", "-m", "777", "subdir"])
        self.mount_a.run_shell(["groupadd", "group1"])
        self.mount_a.run_shell(["useradd", "-g", "group1", "user1"])

        # Have no user or group quota
        self.assertEqual(
            self.mount_a.getfattr("./subdir", "ceph.user_quota.max_bytes@user1"),
            None)
        self.assertEqual(
            self.mount_a.getfattr("./subdir", "ceph.group_quota.max_bytes@group1"),
            None)

        # Set user quota and group quota for the first time
        self.mount_a.setfattr("./subdir", "ceph.user_quota.max_bytes@user1", "536870912")
        self.mount_a.setfattr("./subdir", "ceph.group_quota.max_bytes@group1", "268435456")
        self.assertEqual(
            self.mount_a.getfattr("./subdir", "ceph.user_quota.max_bytes@user1").split()[0],
            "536870912")
        self.assertEqual(
            self.mount_a.getfattr("./subdir", "ceph.group_quota.max_bytes@group1").split()[0],
            "268435456")

        # Change user quota and group quota
        self.mount_a.setfattr("./subdir", "ceph.user_quota.max_bytes@user1", "134217728")
        self.mount_a.setfattr("./subdir", "ceph.group_quota.max_bytes@group1", "67108864")
        self.assertEqual(
            self.mount_a.getfattr("./subdir", "ceph.user_quota.max_bytes@user1").split()[0],
            "134217728")
        self.assertEqual(
            self.mount_a.getfattr("./subdir", "ceph.group_quota.max_bytes@group1").split()[0],
            "67108864")

        # Cancle user quota and group quota
        self.mount_a.setfattr("./subdir", "ceph.user_quota.max_bytes@user1", "0")
        self.mount_a.setfattr("./subdir", "ceph.group_quota.max_bytes@group1", "0")
        self.assertEqual(
            self.mount_a.getfattr("./subdir", "ceph.user_quota.max_bytes@user1"),
            None)
        self.assertEqual(
            self.mount_a.getfattr("./subdir", "ceph.group_quota.max_bytes@group1"),
            None)

    def test_write_exceed_user_quota(self):
        # Add user and group
        self.mount_a.run_shell(["mkdir", "-m", "777", "subdir"])
        self.mount_a.run_shell(["groupadd", "group1"])
        self.mount_a.run_shell(["useradd", "-g", "group1", "user1"])

        # Set some nice high user quota
        self.mount_a.setfattr("./subdir", "ceph.user_quota.max_bytes@user1", "536870912")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir"])

        # Switch to user1
        self.mount_a.run_shell(["su", "user1"])

        # Do some writes within my quota
        self.mount_a.write_n_mb("subdir/file", 100)

        # Set quotas lower than what user already wrote, it should
        # refuse to write more once it's seen them
        self.mount_a.setfattr("./subdir", "ceph.user_quota.max_bytes@user1", "52428800")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir"])

        # Do some writes are forbidden under the new user quota
        with self.assertRaises(CommandFailedError):
            self.mount_a.write_n_mb("subdir_data/file", 40)

    def test_write_exceed_group_quota(self):
        # Add user and group
        self.mount_a.run_shell(["mkdir", "-m", "777", "subdir"])
        self.mount_a.run_shell(["groupadd", "group1"])
        self.mount_a.run_shell(["useradd", "-g", "group1", "user1"])

        # Set some nice high group quota
        self.mount_a.setfattr("./subdir", "ceph.group_quota.max_bytes@group1", "536870912")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir"])

        # Switch to group1
        self.mount_a.run_shell(["su", "user1"])

        # Do some writes within my quota
        self.mount_a.write_n_mb("subdir/file", 100)

        # Set quotas lower than what group already wrote, it should
        # refuse to write more once it's seen them
        self.mount_a.setfattr("./subdir", "ceph.group_quota.max_bytes@group1", "52428800")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir"])

        # Do some writes are forbidden under the new group quota
        with self.assertRaises(CommandFailedError):
            self.mount_a.write_n_mb("subdir/file", 40)

    def test_write_exceed_user_or_group_quota(self):
        # Add user and group
        self.mount_a.run_shell(["mkdir", "-m", "777", "subdir"])
        self.mount_a.run_shell(["groupadd", "group1"])
        self.mount_a.run_shell(["useradd", "-g", "group1", "user1"])

        # Set user quota under group quota
        self.mount_a.setfattr("./subdir", "ceph.user_quota.max_bytes@user1", "4194304")
        self.mount_a.setfattr("./subdir", "ceph.group_quota.max_bytes@group1", "536870912")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir"])

        # Switch to user1
        self.mount_a.run_shell(["su", "user1"])

        # write exceed user quota is forbidden
        with self.assertRaises(CommandFailedError):
            self.mount_a.write_n_mb("subdir/file", 200)

        # Change set group quota under user quota
        self.mount_a.setfattr("./subdir", "ceph.group_quota.max_bytes@group1", "4194304")
        self.mount_a.setfattr("./subdir", "ceph.user_quota.max_bytes@user1", "536870912")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir"])

        # write exceed new group quota is forbidden
        with self.assertRaises(CommandFailedError):
            self.mount_a.write_n_mb("subdir/file", 200)

    def test_user_write_with_multiple_parent(self):
        # Add user and group
        self.mount_a.run_shell(["mkdir", "-m", "777", "subdir"])
        self.mount_a.run_shell(["mkdir", "-m", "777", "subdir/subdir1"])
        self.mount_a.run_shell(["mkdir", "-m", "777", "subdir/subdir1/subdir2"])
        self.mount_a.run_shell(["groupadd", "group1"])
        self.mount_a.run_shell(["useradd", "-g", "group1", "user1"])

        # Set level1 parent has lowest user qutoa
        self.mount_a.setfattr("./subdir", "ceph.user_quota.max_bytes@user1", "4194304")
        self.mount_a.setfattr("./subdir/subdir1", "ceph.user_quota.max_bytes@user1", "268435456")
        self.mount_a.setfattr("./subdir/subdir1/subdir2", "ceph.user_quota.max_bytes@user1", "536870912")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir/subdir1"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir/subdir1/subdir2"])

        # Switch to user1
        self.mount_a.run_shell(["su", "user1"])

        # write exceed level1 parent's user quota is forbidden
        with self.assertRaises(CommandFailedError):
            self.mount_a.write_n_mb("subdir/subdir1/subdir2/file", 200)

        # Set level2 parent has lowest user quota
        self.mount_a.setfattr("./subdir", "ceph.user_quota.max_bytes@user1", "268435456")
        self.mount_a.setfattr("./subdir/subdir1", "ceph.user_quota.max_bytes@user1", "4194304")
        self.mount_a.setfattr("./subdir/subdir1/subdir2", "ceph.user_quota.max_bytes@user1", "536870912")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir/subdir1"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir/subdir1/subdir2"])

        # write exceed level2 parent's user quota is forbidden
        with self.assertRaises(CommandFailedError):
            self.mount_a.write_n_mb("subdir/subdir1/subdir2/file", 200)

        # Set level3 parent has lowest user quota
        self.mount_a.setfattr("./subdir", "ceph.user_quota.max_bytes@user1", " 536870912")
        self.mount_a.setfattr("./subdir/subdir1", "ceph.user_quota.max_bytes@user1", "268435456")
        self.mount_a.setfattr("./subdir/subdir1/subdir2", "ceph.user_quota.max_bytes@user1", "4194304")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir/subdir1"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.user_quota.max_bytes@user1", "./subdir/subdir1/subdir2"])

        # write exceed level3 parent's user quota is forbidden
        with self.assertRaises(CommandFailedError):
            self.mount_a.write_n_mb("subdir/subdir1/subdir2/file", 200)

    def test_group_write_with_multiple_parent(self):
        # Add user and group
        self.mount_a.run_shell(["mkdir", "-m", "777", "subdir"])
        self.mount_a.run_shell(["mkdir", "-m", "777", "subdir/subdir1"])
        self.mount_a.run_shell(["mkdir", "-m", "777", "subdir/subdir1/subdir2"])
        self.mount_a.run_shell(["groupadd", "group1"])
        self.mount_a.run_shell(["useradd", "-g", "group1", "user1"])

        # Set level1 parent has lowest group quota
        self.mount_a.setfattr("./subdir", "ceph.group_quota.max_bytes@group1", "4194304")
        self.mount_a.setfattr("./subdir/subdir1", "ceph.group_quota.max_bytes@group1", "268435456")
        self.mount_a.setfattr("./subdir/subdir1/subdir2", "ceph.group_quota.max_bytes@group1", "536870912")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir/subdir1"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir/subdir1/subdir2"])

        self.mount_a.run_shell(["su", "user1"])

        # write exceed level1 parent's group quota is forbidden
        with self.assertRaises(CommandFailedError):
            self.mount_a.write_n_mb("subdir/subdir1/subdir2/file", 200)

        # Set level2 parent has lowest group quota
        self.mount_a.setfattr("./subdir", "ceph.group_quota.max_bytes@group1", "268435456")
        self.mount_a.setfattr("./subdir/subdir1", "ceph.group_quota.max_bytes@group1", "4194304")
        self.mount_a.setfattr("./subdir/subdir1/subdir2", "ceph.group_quota.max_bytes@group1", "536870912")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir/subdir1"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir/subdir1/subdir2"])

        # write exceed level2 parent's group quota is forbidden
        with self.assertRaises(CommandFailedError):
            self.mount_a.write_n_mb("subdir/subdir1/subdir2/file", 200)

        # Set level3 parent has lowest group quota
        self.mount_a.setfattr("./subdir", "ceph.group_quota.max_bytes@group1", "536870912")
        self.mount_a.setfattr("./subdir/subdir1", "ceph.group_quota.max_bytes@group1", "268435456")
        self.mount_a.setfattr("./subdir/subdir1/subdir2", "ceph.group_quota.max_bytes@group1", "4194304")

        #Debug
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir/subdir1"])
        self.mount_a.run_shell(["getfattr", "-n", "ceph.group_quota.max_bytes@group1", "./subdir/subdir1/subdir2"])

        # write exceed level3 parent's group quota is forbidden
        with self.assertRaises(CommandFailedError):
            self.mount_a.write_n_mb("subdir/subdir1/subdir2/file", 200)
