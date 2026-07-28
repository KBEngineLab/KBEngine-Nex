import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).resolve().parents[1] / "Wait-SdkResourceRelease.py"
MODULE_SPEC = importlib.util.spec_from_file_location("wait_sdk_resource_release", MODULE_PATH)
RESOURCE_CHECKER = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(RESOURCE_CHECKER)


def resource_values(external=0, tcp=0, websocket=0, kcp=0, udp=0, control_blocks=0, timers=0):
    return {
        "external": external,
        "externalTcp": tcp,
        "externalWebSocket": websocket,
        "externalKcp": kcp,
        "externalUdp": udp,
        "externalKcpControlBlocks": control_blocks,
        "externalKcpUpdateTimers": timers,
    }


class ResourceReleaseTests(unittest.TestCase):
    def test_zero_resources_pass(self):
        self.assertTrue(RESOURCE_CHECKER.resources_released(resource_values()))

    def test_active_kcp_resources_do_not_pass(self):
        values = resource_values(external=1, kcp=1, control_blocks=1, timers=1)
        self.assertFalse(RESOURCE_CHECKER.resources_released(values))

    def test_unclassified_external_channel_fails(self):
        with self.assertRaisesRegex(RuntimeError, "classification mismatch"):
            RESOURCE_CHECKER.resources_released(resource_values(external=1))

    def test_output_contract_uses_external_kcp_resources(self):
        values = resource_values(external=1, kcp=1, control_blocks=1, timers=1)
        sample = RESOURCE_CHECKER.format_sample("baseapp", values)
        self.assertIn("client_connections=1", sample)
        self.assertIn("kcp_control_blocks=1", sample)
        self.assertIn("kcp_timers=1", sample)


if __name__ == "__main__":
    unittest.main()
