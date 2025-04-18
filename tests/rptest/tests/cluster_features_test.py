# Copyright 2021 Redpanda Data, Inc.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.md
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0

import os
import time
import datetime

from rptest.utils.rpenv import sample_license
from rptest.services.admin import Admin
from rptest.services.redpanda import RESTART_LOG_ALLOW_LIST
from rptest.tests.redpanda_test import RedpandaTest
from rptest.services.cluster import cluster
from rptest.services.redpanda_installer import RedpandaInstaller, wait_for_num_versions

from ducktape.errors import TimeoutError as DucktapeTimeoutError
from ducktape.utils.util import wait_until

CURRENT_LOGICAL_VERSION = 5

# The upgrade tests defined below rely on having a logical version lower than
# CURRENT_LOGICAL_VERSION. For the sake of these tests, the exact version
# shouldn't matter.
OLD_VERSION = (22, 1, 4)


class FeaturesTestBase(RedpandaTest):
    """
    Test cases defined in this parent class are executed as part
    of subclasses that define node count below.
    """
    def _get_features_map(self, feature_response=None):
        if feature_response is None:
            feature_response = self.admin.get_features()
        return dict((f['name'], f) for f in feature_response['features'])

    def _assert_default_features(self):
        """
        Verify that the config GET endpoint serves valid json with
        the expected features and version.
        """

        features_response = self.admin.get_features()
        self.logger.info(f"Features response: {features_response}")

        # This assertion will break each time we increment the value
        # of `latest_version` in the redpanda source.  Update it when
        # that happens.
        initial_version = features_response["cluster_version"]
        assert initial_version == CURRENT_LOGICAL_VERSION, \
            f"Version mismatch: {initial_version} vs {CURRENT_LOGICAL_VERSION}"

        assert self._get_features_map(
            features_response)['central_config']['state'] == 'active'

        return features_response


class FeaturesMultiNodeTest(FeaturesTestBase):
    """
    Multi-node variant of tests is the 'normal' execution path for feature manager.
    """
    def __init__(self, *args, **kwargs):
        super().__init__(*args, num_brokers=3, **kwargs)

        self.admin = Admin(self.redpanda)

    @cluster(num_nodes=3)
    def test_get_features(self):
        self._assert_default_features()

    @cluster(num_nodes=3, log_allow_list=RESTART_LOG_ALLOW_LIST)
    def test_explicit_activation(self):
        """
        Using a dummy feature, verify its progression through unavailable->available->active
        """

        # Parameters of the compiled-in test feature
        feature_alpha_version = 2001
        feature_alpha_name = "__test_alpha"

        initial_version = self.admin.get_features()['cluster_version']
        assert (initial_version < feature_alpha_version)
        # Initially, before setting the magic environment variable, dummy test features
        # should be hidden
        assert feature_alpha_name not in self._get_features_map().keys()

        self.redpanda.set_environment({'__REDPANDA_TEST_FEATURES': "ON"})
        self.redpanda.restart_nodes(self.redpanda.nodes)
        assert self._get_features_map(
        )[feature_alpha_name]['state'] == 'unavailable'

        # Version is too low, feature should be unavailable
        assert initial_version == self.admin.get_features()['cluster_version']

        self.redpanda.set_environment({
            '__REDPANDA_TEST_FEATURES':
            "ON",
            '__REDPANDA_LOGICAL_VERSION':
            f'{feature_alpha_version}'
        })
        self.redpanda.restart_nodes(self.redpanda.nodes)

        # Wait for version to increment: this is a little slow because we wait
        # for health monitor structures to time out in order to propagate the
        # updated version
        wait_until(lambda: feature_alpha_version == self.admin.get_features()[
            'cluster_version'],
                   timeout_sec=15,
                   backoff_sec=1)

        # Feature should become available now that version increased.  It should NOT
        # become active, because it has an explicit_only policy for activation.
        assert self._get_features_map(
        )[feature_alpha_name]['state'] == 'available'

        # Disable the feature, see that it enters the expected state
        self.admin.put_feature(feature_alpha_name, {"state": "disabled"})
        wait_until(lambda: self._get_features_map()[feature_alpha_name][
            'state'] == 'disabled',
                   timeout_sec=5,
                   backoff_sec=1)
        state = self._get_features_map()[feature_alpha_name]
        assert state['state'] == 'disabled'
        assert state['was_active'] == False

        # Write to admin API to enable the feature
        self.admin.put_feature(feature_alpha_name, {"state": "active"})

        # This is an async check because propagation of feature_table is async
        wait_until(lambda: self._get_features_map()[feature_alpha_name][
            'state'] == 'active',
                   timeout_sec=5,
                   backoff_sec=1)

        # Disable the feature, see that it enters the expected state
        self.admin.put_feature(feature_alpha_name, {"state": "disabled"})
        wait_until(lambda: self._get_features_map()[feature_alpha_name][
            'state'] == 'disabled',
                   timeout_sec=5,
                   backoff_sec=1)
        state = self._get_features_map()[feature_alpha_name]
        assert state['state'] == 'disabled'
        assert state['was_active'] == True

    @cluster(num_nodes=3, log_allow_list=RESTART_LOG_ALLOW_LIST)
    def test_license_upload_and_query(self):
        """
        Test uploading and retrieval of license
        """
        license = sample_license()
        if license is None:
            self.logger.info(
                "Skipping test, REDPANDA_SAMPLE_LICENSE env var not found")
            return
        license_contents = {
            'expires': datetime.date(2122, 6, 6),
            'format_version': 0,
            'org': 'redpanda-testing',
            'type': 'enterprise'
        }

        assert self.admin.put_license(license).status_code == 200
        wait_until(lambda: self.admin.get_license()['loaded'] is True,
                   timeout_sec=5,
                   backoff_sec=1)
        resp = self.admin.get_license()
        assert resp['loaded'] is True
        assert resp['license'] is not None

        def is_equal_to_license_properties(license_contents,
                                           license_properties):
            """Compares the values within first parameters map to a response
            from the redpanda admin server"""
            days_left = (license_contents['expires'] -
                         datetime.date.today()).days
            return license_properties['format_version'] == license_contents['format_version'] and \
                license_properties['org'] == license_contents['org'] and \
                license_properties['type'] == license_contents['type'] and \
                license_properties['expires'] == days_left

        assert is_equal_to_license_properties(license_contents,
                                              resp['license']) is True


class FeaturesMultiNodeUpgradeTest(FeaturesTestBase):
    """
    Multi-node variant of tests that exercise upgrades from older versions.
    """
    def __init__(self, *args, **kwargs):
        super().__init__(*args, num_brokers=3, **kwargs)
        self.admin = Admin(self.redpanda)
        self.installer = self.redpanda._installer

    def setUp(self):
        self.installer.install(self.redpanda.nodes, OLD_VERSION)
        super().setUp()

    @cluster(num_nodes=3, log_allow_list=RESTART_LOG_ALLOW_LIST)
    def test_upgrade(self):
        """
        Verify that on updating to a new logical version, the cluster
        version does not increment until all nodes are up to date.
        """
        initial_version = self.admin.get_features()['cluster_version']
        assert initial_version < CURRENT_LOGICAL_VERSION, \
            f"downgraded logical version {initial_version}"

        self.installer.install(self.redpanda.nodes, RedpandaInstaller.HEAD)

        # Restart nodes one by one.  Version shouldn't increment until all three are done.
        self.redpanda.restart_nodes([self.redpanda.nodes[0]])
        _ = wait_for_num_versions(self.redpanda, 2)
        assert initial_version == self.admin.get_features()['cluster_version']

        self.redpanda.restart_nodes([self.redpanda.nodes[1]])
        # Even after waiting a bit, the logical version shouldn't change.
        time.sleep(5)
        assert initial_version == self.admin.get_features()['cluster_version']

        self.redpanda.restart_nodes([self.redpanda.nodes[2]])

        # Node logical versions are transmitted as part of health messages, so we may
        # have to wait for the next health tick (health_monitor_tick_interval=10s) before
        # the controller leader fetches health from the last restarted peer.
        wait_until(lambda: CURRENT_LOGICAL_VERSION == self.admin.get_features(
        )['cluster_version'],
                   timeout_sec=15,
                   backoff_sec=1)

    @cluster(num_nodes=3, log_allow_list=RESTART_LOG_ALLOW_LIST)
    def test_rollback(self):
        """
        Verify that on a rollback before updating all nodes, the cluster
        version does not increment.
        """
        initial_version = self.admin.get_features()['cluster_version']
        assert initial_version < CURRENT_LOGICAL_VERSION, \
            f"downgraded logical version {initial_version}"

        self.installer.install(self.redpanda.nodes, RedpandaInstaller.HEAD)
        # Restart nodes one by one.  Version shouldn't increment until all three are done.
        self.redpanda.restart_nodes([self.redpanda.nodes[0]])
        _ = wait_for_num_versions(self.redpanda, 2)
        # Even after waiting a bit, the logical version shouldn't change.
        time.sleep(5)
        assert initial_version == self.admin.get_features()['cluster_version']

        self.redpanda.restart_nodes([self.redpanda.nodes[1]])
        time.sleep(5)
        assert initial_version == self.admin.get_features()['cluster_version']

        self.installer.install(self.redpanda.nodes, OLD_VERSION)
        self.redpanda.restart_nodes([self.redpanda.nodes[0]])
        self.redpanda.restart_nodes([self.redpanda.nodes[1]])
        _ = wait_for_num_versions(self.redpanda, 1)
        assert initial_version == self.admin.get_features()['cluster_version']


class FeaturesSingleNodeTest(FeaturesTestBase):
    """
    A single node variant to make sure feature_manager does its job in the absence
    of any health reports.
    """
    def __init__(self, *args, **kwargs):
        # Skip immediate parent constructor
        super().__init__(*args, num_brokers=1, **kwargs)

        self.admin = Admin(self.redpanda)

    @cluster(num_nodes=1)
    def test_get_features(self):
        self._assert_default_features()


class FeaturesSingleNodeUpgradeTest(FeaturesTestBase):
    """
    Single-node variant of tests that exercise upgrades from older versions.
    """
    def __init__(self, *args, **kwargs):
        super().__init__(*args, num_brokers=1, **kwargs)
        self.admin = Admin(self.redpanda)
        self.installer = self.redpanda._installer

    def setUp(self):
        self.installer.install(self.redpanda.nodes, OLD_VERSION)
        super().setUp()

    @cluster(num_nodes=1, log_allow_list=RESTART_LOG_ALLOW_LIST)
    def test_upgrade(self):
        """
        Verify that on updating to a new logical version, the cluster
        version does not increment until all nodes are up to date.
        """
        initial_version = self.admin.get_features()['cluster_version']
        assert initial_version < CURRENT_LOGICAL_VERSION, \
            f"downgraded logical version {initial_version}"

        # Restart nodes one by one.  Version shouldn't increment until all three are done.
        self.installer.install([self.redpanda.nodes[0]],
                               RedpandaInstaller.HEAD)
        self.redpanda.restart_nodes([self.redpanda.nodes[0]])
        wait_until(lambda: CURRENT_LOGICAL_VERSION == self.admin.get_features(
        )['cluster_version'],
                   timeout_sec=5,
                   backoff_sec=1)


class FeaturesNodeJoinTest(RedpandaTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, num_brokers=4, **kwargs)

        self.admin = Admin(self.redpanda)
        self.installer = self.redpanda._installer

    def setUp(self):
        # We will start nodes by hand during test.
        pass

    @cluster(num_nodes=4)
    def test_old_node_join(self):
        """
        Verify that when an old-versioned node tries to join a newer-versioned cluster,
        it is rejected.
        """

        # Pick a node to roleplay an old version of redpanda
        old_node = self.redpanda.nodes[-1]
        self.installer.install([old_node], OLD_VERSION)

        # Start first three nodes
        self.redpanda.start(self.redpanda.nodes[0:-1])

        # Explicit clean because it's not included in the default
        # one during start()
        self.redpanda.clean_node(old_node, preserve_current_install=True)

        initial_version = self.admin.get_features()['cluster_version']
        assert initial_version == CURRENT_LOGICAL_VERSION, \
            f"Version mismatch: {initial_version} vs {CURRENT_LOGICAL_VERSION}"

        try:
            self.redpanda.start_node(old_node)
        except DucktapeTimeoutError:
            pass
        else:
            raise RuntimeError(
                f"Node {old_node} joined cluster, but should have been rejected"
            )

        # Restart it with a sufficiently recent version and join should succeed
        self.installer.install([old_node], RedpandaInstaller.HEAD)
        self.redpanda.restart_nodes([old_node])
        wait_until(lambda: self.redpanda.registered(old_node),
                   timeout_sec=10,
                   backoff_sec=1)
