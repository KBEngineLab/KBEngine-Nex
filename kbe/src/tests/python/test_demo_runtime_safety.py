"""保证实际业务检查不会误用原 assets 的数据库。"""

import unittest
import xml.etree.ElementTree as ET

from test_demo_assets_runtime import INFRASTRUCTURE_COMPONENTS, SHUTDOWN_GROUPS, validate_test_database


def database_config(name, encrypted="false"):
    database = ET.Element("default")
    ET.SubElement(database, "databaseName").text = name
    auth = ET.SubElement(database, "auth")
    ET.SubElement(auth, "encrypt").text = encrypted
    return database


class DemoRuntimeSafetyTests(unittest.TestCase):
    def test_explicit_test_database_is_accepted(self):
        self.assertEqual(
            "kbe_runtime_test",
            validate_test_database(database_config("kbe_runtime_test"), "legacy_game", "kbe_runtime_test"),
        )

    def test_original_database_is_rejected(self):
        with self.assertRaises(RuntimeError):
            validate_test_database(database_config("legacy_game"), "legacy_game", "legacy_game")

    def test_database_case_cannot_bypass_check(self):
        with self.assertRaises(RuntimeError):
            validate_test_database(database_config("LEGACY_GAME"), "legacy_game", "LEGACY_GAME")

    def test_mismatched_database_is_rejected(self):
        with self.assertRaises(RuntimeError):
            validate_test_database(database_config("other_database"), "legacy_game", "kbe_runtime_test")

    def test_empty_database_is_rejected(self):
        with self.assertRaises(RuntimeError):
            validate_test_database(database_config(""), "legacy_game", "")

    def test_reserved_database_is_rejected(self):
        for name in ("kbe", "KBE", "mysql", "sys", "information_schema", "performance_schema"):
            with self.subTest(database=name):
                with self.assertRaises(RuntimeError):
                    validate_test_database(database_config(name), "legacy_game", name)

    def test_encrypted_credentials_are_rejected(self):
        for encrypted in ("true", "1"):
            with self.subTest(encrypted=encrypted):
                with self.assertRaises(RuntimeError):
                    validate_test_database(
                        database_config("kbe_runtime_test", encrypted), "legacy_game", "kbe_runtime_test"
                    )

    def test_base_and_cell_shutdown_requests_share_phase(self):
        group = next(group for group in SHUTDOWN_GROUPS if "baseapp" in group)
        self.assertIn("cellapp", group)
        self.assertLess(group.index("baseapp"), group.index("cellapp"))

    def test_core_components_receive_shutdown_in_the_same_phase(self):
        entity_phase = next(index for index, group in enumerate(SHUTDOWN_GROUPS) if "baseapp" in group)
        database_phase = next(index for index, group in enumerate(SHUTDOWN_GROUPS) if "dbmgr" in group)
        self.assertEqual(entity_phase, database_phase)
        self.assertIn("baseappmgr", SHUTDOWN_GROUPS[entity_phase])
        self.assertIn("cellappmgr", SHUTDOWN_GROUPS[entity_phase])

    def test_infrastructure_lifetime_is_owned_by_test_controller(self):
        for name in INFRASTRUCTURE_COMPONENTS:
            self.assertNotIn(name, [name for group in SHUTDOWN_GROUPS for name in group])


if __name__ == "__main__":
    unittest.main()
