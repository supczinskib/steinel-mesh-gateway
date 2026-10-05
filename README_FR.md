# Passerelle Steinel Mesh pour ESP32-C3

[English](README.md) · [Polski](README_PL.md) · [Deutsch](README_DE.md)

> **Passerelle Bluetooth Mesh autonome sur ESP32-C3 pour le contrôle local des appareils Steinel, le diagnostic, les mises à jour et l’intégration Home Assistant.**

```text
Appareils Steinel <-> Bluetooth Mesh <-> Passerelle ESP32-C3 -> Home Assistant
```

Passerelle Bluetooth Mesh autonome pour les installations Steinel, avec interface web locale et intégration Home Assistant par l’API native ESPHome. La version **2.0.0** prend en charge plusieurs appareils d’un même réseau.

Auteur et responsable : **Bartosz Supcziński** — <bartek@env.pl>

## Fonctionnement

Les appareils Steinel Connect Mesh communiquent par Bluetooth Mesh, tandis que Home Assistant utilise un réseau IP. L’ESP32-C3 rejoint l’installation Mesh existante, échange directement les commandes et les états avec les appareils, puis les expose par ESPHome.

Importez le réseau existant depuis Steinel Cloud ou un fichier JSON exporté par l’application. Sélectionnez les appareils avec les cases à cocher de l’interface web. Au redémarrage, leurs entités prises en charge sont enregistrées dans ESPHome et découvertes par Home Assistant. Aucune extraction manuelle des clés, aucun YAML par lampe, courtier MQTT ou intégration HA personnalisée n’est nécessaire.

Le cloud sert à la configuration. Les commandes et mesures quotidiennes passent localement par Bluetooth Mesh et Wi-Fi.

## Captures d’écran

### ESP32-C3 Super Mini

![ESP32-C3 Super Mini](docs/images/esp32-c3-super-mini.jpg)

### Interface web locale

La page intégrée permet la configuration, le contrôle, le diagnostic et les mises à jour du micrologiciel depuis le navigateur.

![Interface web locale de la passerelle Steinel Mesh](docs/images/steinel-web-interface.png)

### Appareil dans Home Assistant

L’intégration ESPHome standard expose chaque appareil sélectionné séparément dans Home Assistant. La capture ci-dessous montre NightmatIQ Plus.

![Appareil NightmatIQ dans Home Assistant](docs/images/home-assistant-device.png)

### Fenêtre de contrôle Home Assistant facultative

Un module d’interface facultatif regroupe l’état du capteur, l’éclairement, le mode de fonctionnement et le seuil crépusculaire dans une fenêtre compacte pour NightmatIQ Plus.

![Fenêtre de contrôle NightmatIQ dans Home Assistant](docs/images/home-assistant-control.png)

## Fonctions

### Intégration Bluetooth Mesh locale

- Importe un réseau existant depuis Steinel Cloud ou un fichier JSON exporté par l’application.
- Restaure les clés réseau, d’application et d’appareils, l’IV Index et le catalogue sans extraction manuelle des clés.
- Communique directement avec les appareils sélectionnés par Bluetooth Mesh.
- Lit l’état confirmé de la sortie, l’éclairement, le mouvement/la présence et l’identité, selon les fonctions détectées.
- Commande le mode, la sortie, la luminosité, l’automatisme, le seuil de lumière et la durée d’allumage lorsque ces fonctions sont disponibles.

### Gestion des adresses et des sessions

- Choisit une adresse Mesh libre dans la plage du provisionneur.
- Récupère automatiquement lorsque les appareils Mesh rejettent une adresse source réutilisée.
- Mémorise la première adresse source confirmée pour éviter les changements inutiles après un redémarrage ou une interruption temporaire.
- Conserve les paramètres Mesh lors des redémarrages et des mises à jour OTA.
- Utilise des tentatives limitées et des redémarrages contrôlés pour les transitions entre cloud et Bluetooth.

### Interface web de l’appareil

- Sélection des appareils par cases à cocher, détails et commandes pour chaque appareil.
- Anglais, polonais, allemand et français, avec choix mémorisé dans le navigateur.
- Contrôle, actualisation des états et diagnostic de la configuration.
- RSSI Mesh, compteurs de réponses/délais dépassés, RAM libre et plus grand bloc disponible.
- Rapports en lecture seule téléchargeables en JSON, sans clés ni identifiants privés du réseau.
- Mises à jour automatiques ou manuelles protégées par mot de passe.
- Administration du mot de passe, du Wi-Fi et de la réinitialisation d’usine.

### Intégration Home Assistant

- Un appareil Home Assistant distinct pour chaque nœud Mesh sélectionné.
- Sélecteurs, nombres, capteurs et capteurs binaires standards selon les fonctions découvertes.
- Disponibilité, identité et versions logicielle/matérielle par appareil lorsque ces informations sont fournies.
- Adresse IP de la passerelle, disponibilité du Mesh, état et signal Bluetooth.
- Actualisation manuelle et diagnostic brut des propriétés Sensor facultatif.

L’API native ESPHome fournit les commandes sans intégration personnalisée, courtier MQTT ou fichier YAML par appareil.

## Matériel et compatibilité

### Matériel requis

- ESP32-C3 Super Mini avec 4 Mo de flash ;
- connexion USB/JTAG native pour la première installation ou la récupération ;
- réseau Wi-Fi 2,4 GHz ;
- installation Steinel Connect Mesh avec accès à son compte cloud ou à une sauvegarde exportée par l’application.

L’interface USB apparaît normalement comme un périphérique Espressif USB JTAG/serial (`303a:1001`) et comme `/dev/ttyACM*` sous Linux.

### Plateforme prise en charge

Le micrologiciel utilise ESP-IDF sur ESP32-C3. Les fonctions Bluetooth 5 étendues sont désactivées car Bluetooth Mesh utilise les annonces BLE 4.2. La configuration tient compte de la RAM limitée de l’ESP32-C3.

### Fonctions prises en charge

| Profil / modèle | Fonctions exposées |
|---|---|
| Mode de l’appareil | Auto / Always On / Always Off |
| Generic OnOff Server `1000` | Sortie confirmée, sélecteur On/Off |
| Light Lightness Server `1300` | Luminosité sous forme d’entité number, 0–100 % |
| Light LC Server `130F` | Commande automatique, seuil de lumière et durée d’allumage |
| Sensor Server `1100` | Éclairement et mouvement/présence, si ces propriétés sont transmises |
| Scene / Scheduler | Reconnus dans les métadonnées ; pas d’éditeur générique de scènes ou de programmation horaire |

La passerelle vérifie les **modèles SIG associés à une clé** et les Composition Data authentifiées : nombre d’éléments, Company/Product ID et capacités importées. Une absence de réponse ne bloque pas les échanges valides utilisant l’AppKey importée ; une incompatibilité confirmée bloque les commandes.

Les groupes, groupes voisins et relations Master/Slave se configurent dans Steinel Connect. La passerelle utilise la configuration existante du réseau sans la modifier.

Les sorties utilisent le premier modèle compatible ; une sortie Lightness est prioritaire sur un élément OnOff séparé. Tous les éléments Sensor sont interrogés. Les sorties indépendantes d’un appareil ne sont pas exposées comme des canaux distincts. Les capteurs restent inconnus jusqu’à réception d’une propriété pertinente. L’envoi d’une commande ne constitue pas une confirmation de l’état réel.

### Appareils et compatibilité

| Appareils | Prise en charge | Fonctions / condition |
|---|---|---|
| NightmatIQ Plus, IS 180 (variante Mesh) | Modèles SIG | Commandes et mesures selon l’équipement et le micrologiciel de l’appareil |
| L 800 SC, L 810 SC / C, L 820 SC, L 830 SC / C, L 835 SC / C, L 840 SC / C | Modèles SIG | Commandes et mesures selon l’équipement et le micrologiciel de l’appareil |
| L 40 SC / C, L 42 SC / C | Modèles SIG | Commandes et mesures selon l’équipement et le micrologiciel de l’appareil |
| L 270 digi SC, L 271 digi SC / C | Modèles SIG | Commandes et mesures selon l’équipement et le micrologiciel de l’appareil |
| RS 200 SC / C, GL 80 SC / C | Modèles SIG | Commandes et mesures selon l’équipement et le micrologiciel de l’appareil |
| Spot One SC, Spot Duo SC, Spot Way SC, Spot Garden SC, XLED home 2 SC | Modèles SIG | Commandes et mesures selon l’équipement et le micrologiciel de l’appareil |

La compatibilité dépend des modèles réels et du micrologiciel, pas uniquement du nom commercial. La passerelle les vérifie automatiquement. Les variantes C sans capteur ne gagnent pas leurs propres capteurs grâce à la passerelle.

Chaque entrée exige une sauvegarde valide du réseau Connect, Company ID `0x0563`, des modèles pris en charge associés à l’AppKey importée et le respect des limites ci-dessous. Les anciennes versions Bluetooth propriétaires, variantes non connectées, produits uniquement Z-Wave/KNX/DALI et boutons uniquement clients ne sont pas inclus. La conversion Mesh peut être irréversible et modifier la compatibilité avec les anciens appareils ; consulter la [FAQ Steinel Connect](https://www.steinel-shop.de/service/produktinfos-beratung/faq/faq-connect-app/) avant une mise à jour.

Voir les [détails de compatibilité](docs/DEVICE_COMPATIBILITY.md) et la [description de la version 2.0.0](docs/releases/v2.0.0.md).

## Identité et découverte des fonctions

Chaque appareil sélectionné dispose de la même carte web, de détails dépliables et de son propre appareil HA. Les fonctions sont détectées à partir des réponses authentifiées ou des descripteurs des capteurs. Les commandes nécessitent une réponse récente de l’appareil. Un modèle LC/Sensor seul ne crée pas toutes les entités possibles. Les nouvelles fonctions sont enregistrées puis synchronisées par un redémarrage contrôlé : l’API native ESPHome exige leur enregistrement au démarrage. Des appareils lents ou inaccessibles peuvent nécessiter plusieurs cycles.

Les détails indiquent le modèle, le fabricant, les Company/Product ID et les modèles Mesh, ainsi que les révisions logicielle et matérielle lorsque l’appareil les fournit. Les adresses Mesh et identifiants hexadécimaux utilisent des minuscules. L’UUID et le Composition VID ne sont pas affichés dans les détails web ordinaires.

La liste principale contient les appareils Steinel. Les entrées sans identifiant de fabricant apparaissent séparément sous **Appareils non identifiés** ; les appareils identifiés comme appartenant à un autre fabricant sont exclus. Après l’import, la connexion au cloud et l’import local sont repliés sous **Modifier ou réimporter le réseau** et restent disponibles pour les changements de configuration ultérieurs.

Les réglages demandés apparaissent immédiatement dans l’interface web et Home Assistant, puis sont confirmés en arrière-plan. L’état réel de la sortie et les mesures proviennent des réponses de l’appareil, sans être déduits. Après une commande, la lecture de la sortie est prioritaire et répétée sans attendre le prochain cycle normal. NightmatIQ Plus expose le mode, le seuil crépusculaire, l’éclairement et l’état réel de la sortie, sans commande de luminosité ni durée d’allumage après détection de mouvement.

## Limites et mémoire

- Un réseau et une paire NetKey/AppKey, jusqu’à **16 appareils importés**, **8 éléments par appareil** et **16 groupes de publication des capteurs**.
- Les modèles utilisant une autre AppKey ne sont pas exposés pour les commandes.
- Une file de 16 entrées regroupe les modifications non envoyées ; une entrée est réservée à la poursuite ou au réessai d’une transaction.
- Une transaction avec réponse à la fois ; vérification de l’adresse, de l’opcode et de la propriété. Réessai d’écriture limité avec le même TID.
- Les interrogations sont produites progressivement, sans stocker une file complète par appareil.
- Un tampon de lecture flash de 512 octets évite de conserver tout le JSON en RAM.
- La sauvegarde est placée temporairement dans la **partition OTA inactive**. L’importation remplace son ancienne image ; la copie de travail est ensuite effacée.
- HTTPS et Mesh actif utilisent des modes séparés. Désactivez Mesh avant un nouvel import.
- Modifier la sélection exige un redémarrage contrôlé. Une sélection vide désactive Mesh sans supprimer les clés.
- Les appareils et les valeurs expirent après trois minutes, avec une prolongation limitée selon le nombre de fonctions confirmées dans les réseaux importants. Un réseau important ou partiellement hors ligne peut nécessiter un cycle de plus de 30 secondes.

Dans les automatisations, vérifiez l’entité **Available** de chaque appareil. La sortie et l’automatisme utilisent des sélecteurs On/Off dont l’état devient inconnu après expiration de la mesure. Les commandes sans réponse récente ou avec des Composition Data incompatibles sont refusées. Le diagnostic conserve jusqu’à huit propriétés séparées par élément, sans écriture flash ; elles sont visibles sur le web et dans l’entité texte **Sensor diagnostics**, désactivée par défaut.

Le diagnostic web affiche la RAM interne libre et le plus grand bloc pendant le fonctionnement. Le rapport de compilation indique séparément l’utilisation statique de RAM et de flash par le micrologiciel.

Pour signaler un autre appareil, ouvrez ses détails et choisissez **Préparer le rapport de l’appareil**. Saisissez le modèle indiqué sur l’étiquette, démarrez la collecte et marquez éventuellement des observations, comme un capteur couvert ou un mouvement. **Télécharger le rapport** arrête la collecte et enregistre un fichier JSON à joindre au signalement. La session en lecture seule dure jusqu’à trois minutes et fonctionne aussi pour les appareils importés non sélectionnés dans HA. Les clés, identifiants de compte, mots de passe, noms privés, UUID et adresses réseau sont exclus. Le rapport contient les Composition Data, modèles SIG/fabricant, réponses brutes, erreurs et indicateurs de données tronquées ou manquantes. La passerelle garde un tampon RAM limité ; le navigateur conserve l’historique. Laissez donc la page ouverte. Si la connexion est perdue, les données collectées restent téléchargeables ; le rapport signale un arrêt non confirmé ou un dernier relevé échoué.

## Sécurité

- Le compte administrateur initial est `admin`, avec le mot de passe `12345678`. Changez-le sur la page de l’appareil dès la connexion au Wi-Fi.
- Le même mot de passe protège la page locale et les mises à jour ; il est conservé dans la NVS de l’ESP32.
- Le point d’accès de configuration utilise le mot de passe initial `12345678`.
- Configuration, sauvegardes et mises à jour uniquement sur un LAN de confiance. HTTP Digest authentifie mais ne chiffre pas HTTP.
- Les identifiants Steinel servent aux requêtes HTTPS et ne sont pas enregistrés.
- Les clés réseau/appareils et la configuration sont conservées dans la NVS de l’ESP32. La carte contient des données d’accès à votre réseau.
- L’API native n’est pas chiffrée par défaut ; vous pouvez activer le chiffrement dans votre propre compilation.
- Ne publiez jamais vos sauvegardes, mots de passe, clés ou captures Bluetooth réels.
- La réinitialisation d’usine efface Wi-Fi, administrateur et Mesh, pas la version installée.

## Structure du dépôt

| Chemin | Rôle |
|---|---|
| `esphome/steinel-c3.yaml` | Configuration principale du micrologiciel ESPHome |
| `esphome/components/steinel_mesh/` | Bluetooth Mesh, catalogue, import Steinel Cloud et interface web |
| `esphome/components/steinel_mesh/mesh_protocol.h` | Fonctions portables de protocole et de validation |
| `esphome/components/steinel_mesh/steinel_nodes.cpp` | Entités des appareils et ordonnanceur des transactions |
| `scripts/` | Installation, validation, USB, OTA et préparation des versions |
| `tests/` | Tests de régression du protocole, de l’analyseur, du transport et du navigateur |
| `home-assistant/` | Package Home Assistant et fenêtre de contrôle facultatifs |
| `docs/images/` | Images publiques des README |
| `docs/DEVICE_COMPATIBILITY.md` | Appareils pris en charge et détails de compatibilité |
| `docs/releases/` | Descriptions des versions |
| `demo/` | Démonstration locale de l’interface web |

## Installer le micrologiciel prêt à l’emploi

La première installation recommandée ne nécessite pas de compiler ESPHome :

1. Téléchargez le dernier fichier `steinel-mesh-esp32-c3-gateway-vX.Y.Z-factory.bin` depuis [GitHub Releases](https://github.com/supczinskib/steinel-mesh-gateway/releases/latest).
2. Ouvrez [ESPHome Web](https://web.esphome.io/) dans un navigateur compatible WebSerial et connectez l’ESP32-C3 par USB.
3. Sélectionnez la carte, choisissez **Install** et sélectionnez le fichier `-factory.bin`.
4. Continuez avec **Connexion au Wi-Fi** et **Connexion à Steinel Mesh** ci-dessous.

ESPHome Web traite le fichier localement. L’image `-factory.bin` sert à la première installation ou à la récupération par USB ; les mises à jour suivantes depuis le navigateur utilisent l’image `-ota.bin`.

## Compilation depuis les sources

Linux ou macOS, Python 3.12–3.14, accès USB pour la première installation et accès réseau à l’ESP32-C3 sont nécessaires. Internet est requis pour l’import cloud ; Home Assistant est facultatif.

L’installateur crée un environnement isolé et reproductible avec ESPHome `2026.7.3`, sans modification du package ESPHome installé. Il utilise apt sous Debian/Ubuntu. Sous macOS, utilisez un environnement virtuel Python 3.12–3.14 avec cette version d’ESPHome. Les scripts acceptent aussi la variable `ESPHOME` indiquant le chemin de son exécutable.

Clonez ou téléchargez ce dépôt, ouvrez son répertoire et préparez l’environnement :

```bash
sudo bash scripts/01_install_esphome.sh
bash scripts/03_validate_all.sh
```

La validation exécute les vérifications du dépôt et contrôle la configuration ESPHome.

Pour la première installation ou une récupération, utilisez le chemin USB stable `/dev/serial/by-id/` s’il existe :

```bash
sudo bash scripts/09_upload_usb.sh /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00
```

Sans lien `by-id`, utilisez le port ACM détecté :

```bash
sudo bash scripts/09_upload_usb.sh /dev/ttyACM0
```

La même image convient aux cartes ESP32-C3 prises en charge. La première installation USB prépare également les mises à jour depuis le navigateur ; le bouton de démarrage n’est normalement plus nécessaire ensuite.

```sh
bash scripts/00_self_test.sh
bash scripts/11_test_mesh.sh
esphome compile esphome/steinel-c3.yaml
bash scripts/10_prepare_release.sh
```

Les tests du protocole et de l’analyseur de sauvegarde utilisent AddressSanitizer/UndefinedBehaviorSanitizer et des données synthétiques. Ils couvrent troncatures, composition, chevauchements d’adresses, limites de groupes, associations de clés et corrélation des réponses.

Si votre shell exporte un autre ESP-IDF, utilisez un shell propre ou `env -u IDF_PATH -u IDF_TOOLS_PATH esphome compile esphome/steinel-c3.yaml`.

Tests web : `cd tests && npm install && npm test`. jsdom est réservé aux tests et n’est pas inclus dans le micrologiciel. Les tests vérifient les quatre langues, la mémorisation du choix, les cartes dynamiques, la conservation des cases cochées et les valeurs des commandes API.

## Connexion au Wi-Fi

1. Connectez-vous au point d’accès `nightmatiq-gateway-XXXXXX` avec le mot de passe `12345678`.
2. Choisissez votre réseau Wi-Fi 2,4 GHz dans le portail captif et entrez son mot de passe.
3. Attendez le redémarrage et la connexion au réseau.
4. Ouvrez l’adresse attribuée par le routeur ou le nom d’hôte de l’appareil se terminant par `.local`.

La configuration Wi-Fi est enregistrée et conservée lors des mises à jour du micrologiciel.

## Connexion à Steinel Mesh

1. Ouvrez l’adresse de la passerelle dans un navigateur.
2. Connectez-vous avec `admin` et le mot de passe initial `12345678`.
3. Dans **Gateway administration**, changez le mot de passe dans **Administrator access**. Le même mot de passe autorise les mises à jour.
4. Reconnectez-vous après le redémarrage automatique.
5. Entrez vos identifiants Steinel Cloud et chargez la liste des réseaux.
6. Choisissez l’installation, importez sa configuration et laissez la passerelle redémarrer. Vous pouvez aussi utiliser **Import a local backup** pour téléverser le JSON exporté par l’application ; désactivez Mesh avant l’import.
7. Sélectionnez les appareils à exposer dans Home Assistant et choisissez **Save selection and restart**.

L’adresse initiale de l’appareil est facultative : laissez le choix automatique sauf besoin particulier. L’IV Index se synchronise normalement avec le réseau. Les identifiants Steinel restent uniquement dans le formulaire du navigateur pour les requêtes de configuration.

La sélection peut être modifiée dans le même panneau. Après un changement d’appareils ou de clés dans l’application Steinel, désactivez Mesh et importez une sauvegarde récente. Les sélections précédentes sont conservées selon l’identité des appareils lorsque c’est possible.

L’interface propose **anglais, polonais, allemand et français**. L’anglais est la valeur par défaut ; le choix est mémorisé dans ce navigateur. Les noms des appareils, les données du compte et les valeurs des commandes API ne sont pas traduits.

## Mise à jour du micrologiciel par Wi-Fi

La passerelle vérifie la dernière version stable publiée sur GitHub à l’ouverture de sa page. **CHECK FOR UPDATES** relance la vérification. Si une version plus récente existe, **DOWNLOAD AND INSTALL** la télécharge par HTTPS, vérifie sa taille et son empreinte SHA-256, l’installe puis redémarre. En cas d’échec, le micrologiciel actuel reste actif.

L’installation manuelle est disponible dans **Manual firmware file**. Utilisez seulement un fichier `-ota.bin` ; `-factory.bin` est réservé à la première installation USB. Aucun mot de passe OTA distinct n’est nécessaire.

Depuis la ligne de commande, entrez le mot de passe administrateur à la demande :

```bash
bash scripts/05_upload_ota.sh DEVICE_IP_OR_HOSTNAME
```

Installez une image **OTA** dans l’interface web, jamais une image factory comme OTA. Le Wi-Fi, les identifiants administrateur et la configuration Mesh enregistrée sont conservés.

## Réinitialisation d’usine

**Factory reset** efface le Wi-Fi, le compte administrateur et la configuration Bluetooth Mesh. Le compte initial et le point d’accès de configuration sont rétablis, sans changer la version du micrologiciel installé.

## Home Assistant

Les appareils distincts nécessitent **Home Assistant 2025.7 ou plus récent**, voir la [prise en charge des sous-appareils ESPHome](https://www.home-assistant.io/blog/2025/07/02/release-20257/#noteworthy-improvements-to-existing-integrations).

Home Assistant découvre normalement la passerelle automatiquement par ESPHome. Sinon :

1. Ouvrez **Paramètres → Appareils et services**.
2. Ajoutez l’intégration **ESPHome**.
3. Entrez l’adresse IP ou le nom d’hôte de la passerelle.
4. Affectez les appareils aux zones souhaitées.

Chaque appareil sélectionné dispose de ses propres commandes et diagnostics. La passerelle expose aussi son adresse IP comme entité de diagnostic.

La désactivation de Mesh ou le passage en mode de mise à jour conserve les appareils sélectionnés et leurs entités dans Home Assistant, ainsi que leur affectation aux zones. Les mesures deviennent indisponibles lorsque la communication s’arrête ; les appareils ne sont pas supprimés.

HA peut garder des entrées d’appareils désélectionnés ; supprimez les entrées inutilisées si nécessaire.

## Fenêtre de contrôle Home Assistant facultative

L’intégration ESPHome standard fournit toutes les entités et commandes. Les fichiers de `home-assistant/` ajoutent la tuile de zone et la fenêtre compacte présentées ci-dessus.

1. Copiez `steinel-nightmatiq-package.yaml` dans le répertoire des packages Home Assistant.
2. Copiez `steinel-nightmatiq-popup.js` dans `/config/www/`.
3. Ajoutez `/local/steinel-nightmatiq-popup.js?v=200` comme module JavaScript dans les ressources du tableau de bord.
4. Rechargez la configuration des packages et actualisez le cache du navigateur.

Le module reconnaît automatiquement sa cible lorsqu’il existe exactement un appareil compatible. S’il y en a plusieurs, définissez `window.steinelNightmatiqEntities` dans le JavaScript et le mapping `target_entities` dans le package YAML avec les identifiants de l’appareil choisi. Vérifiez aussi les suffixes tels que `_2`. Cette fenêtre facultative est destinée à NightmatIQ Plus ; les autres appareils utilisent les commandes standards de Home Assistant.

Le module personnalise la tuile de zone générée et la fenêtre « more-info ». La stratégie de zone appartient au frontend Home Assistant ; une mise à jour future peut nécessiter une adaptation du module facultatif.

La zone affiche une seule tuile d’état du capteur, qui ouvre la fenêtre de contrôle. Les autres tuiles du même capteur sont omises de cette vue sans désactiver les entités ni les automatisations. La détection automatique vérifie l’identité de la passerelle plutôt que les seuls noms similaires d’entités d’autres intégrations.

## Plusieurs passerelles

Lors de l’import, chaque passerelle détermine sa politique d’adresses Mesh à partir de l’installation choisie et de sa propre identité matérielle. Le même micrologiciel peut donc être configuré sur plusieurs ESP32-C3 et pour différentes installations Steinel.

Le suffixe MAC du nom est activé par défaut : les noms d’hôte et de point d’accès restent distincts. Choisissez un mot de passe administrateur différent sur chaque passerelle.

## Point d’accès de secours

Si le réseau Wi-Fi configuré reste inaccessible pendant 60 secondes, la passerelle réactive son point d’accès protégé. Connectez-vous avec le mot de passe initial `12345678` et modifiez le Wi-Fi dans le portail captif. La page locale conserve le mot de passe administrateur choisi sur l’appareil.

## Dépannage

### La passerelle n’apparaît pas dans Home Assistant

- Vérifiez que Home Assistant peut joindre la passerelle sur le réseau IoT.
- Ajoutez manuellement l’intégration ESPHome par IP si la découverte est filtrée entre VLAN.
- Vérifiez que la passerelle est en ligne et redémarrez l’intégration ESPHome si la connexion reste indisponible.

### Le Mesh est prêt, mais les valeurs sont indisponibles

- Rapprochez l’ESP32-C3 des appareils sélectionnés et vérifiez **Last Mesh RSSI** dans les diagnostics.
- Attendez la synchronisation de l’IV Index après l’import d’une sauvegarde.
- Utilisez **Refresh devices** pour demander les états actuels.

### Le téléchargement du réseau Steinel échoue

- Vérifiez que le compte accède à l’installation dans l’application officielle Steinel.
- Contrôlez l’accès Internet, le DNS et l’heure système du réseau de la passerelle.
- Après un échec de configuration, attendez le redémarrage avant de réessayer.

### La mise à jour OTA échoue

- Vérifiez l’adresse cible et le mot de passe administrateur.
- Utilisez la mise à jour depuis le navigateur sur un LAN de confiance.
- Récupérez l’appareil par USB natif s’il ne se connecte plus au Wi-Fi.

## Projet associé

Les fonctions Steinel NightmatIQ Plus sont également disponibles comme intégration facultative dans la [passerelle AR01V3 RF/IR, ESP-RC01 et Steinel NightmatIQ Plus](https://github.com/supczinskib/athom-ar01v3-esp-rc01-gateway). Choisissez ce projet pour ajouter NightmatIQ à une passerelle multifonction AR01V3 existante, ou ce dépôt pour une petite passerelle ESP32-C3 dédiée.

## Licence

Copyright (C) 2026 Bartosz Supcziński.

Ce projet est distribué sous la GNU General Public License version 3 uniquement (`GPL-3.0-only`). Voir [LICENSE](LICENSE).

## Assistance

- Auteur et responsable : **Bartosz Supcziński**, <bartek@env.pl>.
- Identifiant du projet ESPHome : `envpl.steinel_mesh_gateway`.

Pour signaler un problème, indiquez la version du micrologiciel, la version d’ESPHome, la cause du redémarrage et les journaux pertinents. Retirez les mots de passe, clés, en-têtes d’autorisation, sauvegardes privées et identifiants réseau avant de partager les diagnostics.

Ce projet communautaire indépendant n’est pas un produit officiel Steinel, ESPHome ou Home Assistant.
