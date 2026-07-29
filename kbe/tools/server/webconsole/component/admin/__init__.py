# cluster/admin/__init__.py
import pkgutil
import importlib
from pathlib import Path

pkg_dir = Path(__file__).resolve().parent / "pages"
for module_info in pkgutil.iter_modules([str(pkg_dir)]):
    importlib.import_module(f"{__package__}.pages.{module_info.name}")