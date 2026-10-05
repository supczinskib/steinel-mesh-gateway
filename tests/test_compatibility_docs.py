"""Check device lists, README structure and public installation instructions."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
FILES = ["README.md", "README_PL.md", "README_DE.md", "README_FR.md",
         "CHANGELOG.md", "docs/DEVICE_COMPATIBILITY.md", "docs/releases/v2.0.0.md"]


def device_rows(text):
    headings = ("Supported devices", "Obsługiwane urządzenia",
                "Unterstützte Geräte", "Appareils pris en charge",
                "Devices and compatibility", "Urządzenia i zgodność",
                "Geräte und Kompatibilität", "Appareils et compatibilité")
    section = re.search(r"^#{2,3} (?:" + "|".join(headings) +
                        r")\n(.*?)(?=^#{1,3} |\Z)", text, re.M | re.S)
    assert section, "Supported-device list is missing"
    lines = section.group(1).splitlines()
    rows = [line[2:] for line in lines if line.startswith("- ")]
    if rows:
        return rows
    table = [line for line in lines if line.startswith("| ") or line.startswith("|---")]
    return [line.split("|")[1].strip() for line in table[2:]]


def run():
    documents = {name: (ROOT / name).read_text(encoding="utf-8") for name in FILES}
    expected = device_rows(documents["README.md"])
    assert len(expected) == 6
    for name, text in documents.items():
        actual = device_rows(text)
        # IS 180's qualifier is translated; the model identities must match.
        actual = [re.sub(r"IS 180 \([^)]*\)", "IS 180 (variant)", row) for row in actual]
        reference = [re.sub(r"IS 180 \([^)]*\)", "IS 180 (variant)", row) for row in expected]
        assert actual == reference, f"Compatibility model list differs in {name}"
    for name in ["README.md", "README_PL.md", "README_DE.md", "README_FR.md"]:
        assert "docs/DEVICE_COMPATIBILITY.md" in documents[name]
        assert "docs/releases/v2.0.0.md" in documents[name]
        assert "esphome compile esphome/steinel-c3.yaml" in documents[name], name
        assert "esphome/components/steinel_mesh/" in documents[name], name
        assert "esphome/nightmatiq-c3.yaml" not in documents[name], name
        assert "docs/BUILDING.md" not in documents[name], name
    readme_headings = {
        "README.md": (
            "## Why this project exists", "## Screenshots", "## What this project provides",
            "## Hardware and compatibility", "### Devices and compatibility",
            "## Device identity and discovery", "## Limits and memory", "## Security",
            "## Repository layout", "## Ready-made installation", "## Building from source",
            "## Requirements", "## 1. Download and prepare the project", "## 2. Validate",
            "## 3. First installation or USB recovery", "## 4. Connect Wi-Fi",
            "## 5. Connect Steinel Mesh", "## 6. Updating over Wi-Fi",
            "## 7. Home Assistant integration", "## 8. Optional compact Home Assistant dialog",
            "## Multiple gateways", "## Fallback access point", "## Troubleshooting",
            "## Related project", "## License", "## Credits and support"),
        "README_PL.md": (
            "## Dlaczego powstał ten projekt", "## Zrzuty ekranu", "## Co zapewnia projekt",
            "## Sprzęt i kompatybilność", "### Urządzenia i zgodność",
            "## Identyfikacja i wykrywanie funkcji", "## Limity i RAM", "## Bezpieczeństwo",
            "## Struktura repozytorium", "## Instalacja gotowego firmware",
            "## Budowanie ze źródeł", "## Wymagania", "## 1. Pobranie i przygotowanie projektu",
            "## 2. Walidacja", "## 3. Pierwsza instalacja lub odzyskiwanie przez USB",
            "## 4. Połączenie z Wi-Fi", "## 5. Połączenie z siecią Steinel Mesh",
            "## 6. Aktualizacja przez Wi-Fi", "## 7. Integracja z Home Assistant",
            "## 8. Opcjonalne zwarte okno Home Assistant", "## Wiele bramek",
            "## Awaryjny punkt dostępowy", "## Rozwiązywanie problemów",
            "## Powiązany projekt", "## Licencja", "## Autor i wsparcie"),
        "README_DE.md": (
            "## Funktionsweise", "## Abbildungen", "## Funktionen",
            "## Hardware und Kompatibilität", "### Geräte und Kompatibilität",
            "## Identität und Funktionserkennung", "## Grenzen und Speicher", "## Sicherheit",
            "## Repository-Struktur", "## Installation eines fertigen Firmware-Abbilds",
            "## Kompilieren aus dem Quellcode", "## WLAN verbinden", "## Steinel Mesh verbinden",
            "## Firmware über WLAN aktualisieren", "## Zurücksetzen auf Werkseinstellungen",
            "## Home Assistant", "## Optionaler Home-Assistant-Steuerdialog",
            "## Mehrere Gateways", "## Fallback-Zugangspunkt", "## Fehlerbehebung",
            "## Verwandtes Projekt", "## Lizenz", "## Support"),
        "README_FR.md": (
            "## Fonctionnement", "## Captures d’écran", "## Fonctions",
            "## Matériel et compatibilité", "### Appareils et compatibilité",
            "## Identité et découverte des fonctions", "## Limites et mémoire", "## Sécurité",
            "## Structure du dépôt", "## Installer le micrologiciel prêt à l’emploi",
            "## Compilation depuis les sources", "## Connexion au Wi-Fi",
            "## Connexion à Steinel Mesh", "## Mise à jour du micrologiciel par Wi-Fi",
            "## Réinitialisation d’usine", "## Home Assistant",
            "## Fenêtre de contrôle Home Assistant facultative", "## Plusieurs passerelles",
            "## Point d’accès de secours", "## Dépannage", "## Projet associé",
            "## Licence", "## Assistance"),
    }
    repository_headings = {
        "README.md": "Repository layout", "README_PL.md": "Struktura repozytorium",
        "README_DE.md": "Repository-Struktur", "README_FR.md": "Structure du dépôt",
    }
    repository_paths = {
        "esphome/steinel-c3.yaml", "esphome/components/steinel_mesh/",
        "esphome/components/steinel_mesh/mesh_protocol.h",
        "esphome/components/steinel_mesh/steinel_nodes.cpp", "scripts/", "tests/",
        "home-assistant/", "docs/images/", "docs/DEVICE_COMPATIBILITY.md",
        "docs/releases/", "demo/",
    }
    screenshot_paths = (
        "docs/images/esp32-c3-super-mini.jpg", "docs/images/steinel-web-interface.png",
        "docs/images/home-assistant-device.png", "docs/images/home-assistant-control.png",
    )
    for name, headings in readme_headings.items():
        text = documents[name]
        matches = [re.search(r"^" + re.escape(heading) + r"$", text, re.M)
                   for heading in headings]
        assert all(matches), f"README section is missing in {name}"
        positions = [match.start() for match in matches]
        assert positions == sorted(positions), f"README section order changed in {name}"
        assert "|---|---|---|" in text, name
        repository = re.search(r"^## " + re.escape(repository_headings[name]) +
                               r"\n(.*?)(?=^## |\Z)", text, re.M | re.S).group(1)
        assert "|---|---|" in repository, f"Repository table is missing in {name}"
        paths = set(re.findall(r"^\| `([^`]+)` \|", repository, re.M))
        assert paths == repository_paths, f"Repository table differs in {name}"
        for path in paths:
            assert (ROOT / path).exists(), f"Missing repository path {path} in {name}"
        image_blocks = re.findall(r"^### [^\n]+\n\n(?:(?!^#{1,3} ).)*?"
                                  r"!\[[^\]]*\]\(([^)]+)\)", text, re.M | re.S)
        assert tuple(image_blocks) == screenshot_paths, f"Screenshot sections differ in {name}"
        for target in re.findall(r"!?\[[^\]]*\]\(([^)]+)\)", text):
            if re.match(r"[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("#"):
                continue
            assert (ROOT / target.split("#", 1)[0]).exists(), f"Broken link {target} in {name}"
        for script in ("01_install_esphome.sh", "03_validate_all.sh", "09_upload_usb.sh",
                       "05_upload_ota.sh", "00_self_test.sh", "11_test_mesh.sh",
                       "10_prepare_release.sh"):
            assert f"scripts/{script}" in text, f"Missing {script} instructions in {name}"
        for detail in ("2026.7.3", "303a:1001", "/dev/ttyACM0", "12345678",
                       "60", "2025.7", "SHA-256", "npm test", "target_entities",
                       "window.steinelNightmatiqEntities", "?v=200"):
            assert detail in text, f"Missing setup detail {detail} in {name}"
    for name, text in documents.items():
        assert "built-in product names" not in text.lower(), name
        assert "lighting families were selected" not in text.lower(), name
        assert "0x1DCE" not in text and "0x1E79" not in text, name
    print("Documentation: device lists, README sections, tables, screenshots, local links and setup instructions passed")


if __name__ == "__main__":
    run()
