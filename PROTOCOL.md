# Protocole de mise à jour firmware — STM32L476RG

**Version du protocole : 1**
**Cible : STM32L476RG (Nucleo-L476RG)**
**Transport : UART 115200 8N1 (CAN prévu ultérieurement)**

---

## 1. Vue d'ensemble

Ce document spécifie le protocole binaire utilisé entre un outil PC et le bootloader
embarqué pour transférer et installer un nouveau firmware.

Le protocole est **indépendant de la couche de transport**. L'implémentation de
référence utilise l'UART ; un portage sur CAN ou tout autre lien orienté octet ne
nécessite aucune modification de la couche protocole.

### Principes de conception

- **Le PC est maître.** Il initie toutes les transactions ; le bootloader ne fait que répondre.
- **Chaque trame est acquittée.** Le PC n'envoie la trame suivante qu'après un ACK.
- **Rien n'est détruit avant validation.** Le bootloader vérifie tout ce qu'il peut
  (taille, slot, version) avant d'effacer la moindre page de flash.
- **Idempotence.** Le retraitement d'une trame déjà reçue ne produit aucun effet de bord.
- **Défense en profondeur.** Trois niveaux de vérification indépendants : CRC par trame
  (transmission), read-back après écriture (stockage), CRC global (cohérence de l'image).

---

## 2. Format de trame

Toutes les trames, dans les deux sens, partagent la même structure.

```
Offset  Taille  Champ    Format          Description
------  ------  -------  --------------  ------------------------------------
0       2       MAGIC    0xAA 0x55       Préambule de synchronisation
2       1       CMD      uint8           Type de message
3       2       LENGTH   uint16 LE       Nombre d'octets dans DATA
5       2       SEQ      uint16 LE       Numéro de séquence
7       N       DATA     N octets        Payload (peut être vide)
7+N     4       CRC32    uint32 LE       Contrôle d'intégrité
```

**En-tête fixe : 7 octets. Surcoût total avec CRC : 11 octets par trame.**

### Ordre des octets

Tous les champs multi-octets sont en **little-endian**, l'ordre natif du Cortex-M4.
Ce choix élimine toute conversion côté embarqué ; la conversion éventuelle est
reportée sur le PC, qui dispose des ressources pour l'absorber.

### Champ MAGIC

Valeur fixe `0xAA 0x55`. Les deux octets sont l'inverse binaire l'un de l'autre, ce qui
produit huit transitions consécutives sur la ligne — le motif le plus éloigné possible
d'une ligne bloquée à un niveau constant, donc le plus facile à distinguer du bruit.

Les valeurs `0x00` et `0xFF` sont volontairement évitées : elles abondent naturellement
dans un binaire (padding, flash effacée, zones de zéros).

### Couverture du CRC

Le CRC32 est calculé sur **CMD, LENGTH, SEQ et DATA** — le MAGIC en est exclu.

Justification : le MAGIC se valide lui-même. Une trame dont le préambule est corrompu
n'est jamais reconnue comme trame, donc son CRC n'est jamais évalué. L'inclure
n'apporterait aucune détection supplémentaire.

En revanche, la couverture de LENGTH est **critique** : un champ de longueur non protégé
constitue un vecteur classique de dépassement de buffer.

### Polynôme CRC

CRC-32 standard (polynôme `0x04C11DB7`), calculé par le périphérique CRC matériel du
STM32L4. Valeur initiale `0xFFFFFFFF`.

### Taille de trame

`LENGTH` maximal accepté : **1024 octets**.
Taille nominale des blocs de données : **256 octets**.

La taille de bloc est contrainte par deux propriétés du contrôleur flash du STM32L4 :

- **Multiple de 8** — l'unité de programmation est le double-mot de 64 bits. Une taille
  non multiple de 8 laisserait des octets orphelins à recoller entre deux trames.
- **Diviseur de 2048** — l'unité d'effacement est la page de 2 Ko. Une taille non
  diviseur ferait chevaucher certaines trames sur deux pages.

256 satisfait les deux et offre un surcoût protocole de 4,3 %, avec un buffer RAM
négligeable.

---

## 3. Codes de commande

Le bit de poids fort distingue les requêtes des réponses, ce qui permet d'identifier
l'émetteur d'un octet à l'œil nu dans une capture d'analyseur logique.

### Requêtes (PC → bootloader)

| Valeur | Nom                | Description                                   |
|--------|--------------------|-----------------------------------------------|
| `0x01` | `CMD_GET_INFO`     | Interroge l'état de la carte                   |
| `0x02` | `CMD_START_UPDATE` | Annonce un transfert : taille, CRC, slot cible |
| `0x03` | `CMD_DATA`         | Transporte un bloc de firmware                 |
| `0x04` | `CMD_END_UPDATE`   | Signale la fin, demande la validation globale  |
| `0x05` | `CMD_ABORT`        | Annule le transfert en cours                   |

### Réponses (bootloader → PC)

| Valeur | Nom         | Description                        |
|--------|-------------|------------------------------------|
| `0x81` | `RSP_INFO`  | Réponse à `CMD_GET_INFO`           |
| `0x82` | `RSP_ACK`   | Trame acceptée                     |
| `0x83` | `RSP_NACK`  | Trame rejetée, avec code d'erreur  |

---

## 4. Codes d'erreur

Transportés dans le champ DATA d'un `RSP_NACK`, sur 1 octet.

| Valeur | Nom               | Signification                                  |
|--------|-------------------|------------------------------------------------|
| `0x01` | `ERR_CRC`         | CRC de trame invalide                          |
| `0x02` | `ERR_SEQ`         | Numéro de séquence inattendu                   |
| `0x03` | `ERR_LENGTH`      | LENGTH hors des bornes acceptées               |
| `0x04` | `ERR_FLASH`       | Échec d'écriture ou de relecture               |
| `0x05` | `ERR_SIZE`        | Firmware trop volumineux pour le slot          |
| `0x06` | `ERR_SLOT`        | Slot cible différent du slot annoncé           |
| `0x07` | `ERR_STATE`       | Commande reçue dans un état incompatible       |
| `0x08` | `ERR_GLOBAL_CRC`  | CRC du firmware complet invalide               |
| `0x09` | `ERR_PROTO_VER`   | Version de protocole non supportée             |

Un NACK sans code d'erreur ne dirait que « ça a échoué ». Le code transforme un
message inutile en diagnostic exploitable, pour le coût d'un octet.

---

## 5. Payloads

### 5.1 `CMD_GET_INFO` — requête

DATA vide, `LENGTH = 0`.

### 5.2 `RSP_INFO` — réponse (12 octets)

```
Offset  Taille  Champ           Description
------  ------  --------------  ------------------------------------------
0       1       proto_version   Version de protocole supportée
1       1       active_slot     Slot en cours d'exécution (0 = A, 1 = B)
2       1       free_slot       Slot où le PC doit écrire (0 = A, 1 = B)
3       1       state           État courant (voir §7)
4       4       fw_version      Version du firmware actif (LE)
8       4       bl_version      Version du bootloader (LE)
```

`active_slot` et `free_slot` sont mutuellement déductibles. Cette redondance est
délibérée : elle place la décision « où écrire » du côté qui détient l'état réel,
plutôt que de la laisser inférer par le PC. Moins le PC déduit, moins il peut se tromper.

### 5.3 `CMD_START_UPDATE` — payload (16 octets)

```
Offset  Taille  Champ           Description
------  ------  --------------  ------------------------------------------
0       4       fw_size         Taille du firmware en octets (LE)
4       4       fw_crc32        CRC32 du firmware complet (LE)
8       4       fw_version      Version du firmware transmis (LE)
12      1       target_slot     Slot de destination (0 = A, 1 = B)
13      1       proto_version   Version du protocole utilisée
14      2       reserved        Réservé, mis à zéro
```

Le nombre de trames n'est **pas** transmis : il se déduit de `fw_size` et de la taille
de bloc. Transmettre une valeur redondante ouvrirait la possibilité d'une incohérence
entre deux champs, qu'il faudrait alors détecter et arbitrer.

Le champ `reserved` porte la structure à 16 octets et laisse de la place pour une
extension future sans changement de taille, donc sans rupture de compatibilité.

### 5.4 `CMD_DATA` — payload

Bloc brut de firmware, `LENGTH` octets, à écrire à l'offset `SEQ × 256` depuis le
début du slot cible.

### 5.5 `CMD_END_UPDATE` — payload

DATA vide. Le bootloader relit l'intégralité de la zone écrite, recalcule le CRC32 et
le compare à `fw_crc32` reçu dans `CMD_START_UPDATE`.

### 5.6 `RSP_ACK` — payload

DATA vide. Le champ SEQ reprend le numéro de la trame acquittée.

---

## 6. Séquencement

### Numérotation

`SEQ` démarre à 0 pour `CMD_START_UPDATE` et s'incrémente à chaque trame.
Le bootloader mémorise le dernier `SEQ` accepté.

### Traitement à la réception

| Condition             | Action                                            |
|-----------------------|---------------------------------------------------|
| `SEQ == attendu`      | Traiter, écrire, acquitter                        |
| `SEQ == attendu - 1`  | **Ne rien réécrire**, renvoyer l'ACK              |
| Autre                 | `RSP_NACK` avec `ERR_SEQ`, abandon du transfert   |

Le second cas correspond à une retransmission consécutive à la perte d'un ACK. Le PC
n'attend pas que la trame soit réécrite — il attend l'accusé qu'il n'a jamais reçu.

Réécrire serait par ailleurs incorrect : la flash ne peut être reprogrammée sans
effacement préalable, et une seconde écriture au même endroit produirait un résultat
indéterminé.

Cette propriété d'**idempotence** rend les retransmissions inoffensives.

### Validation de plausibilité

À la lecture de `LENGTH`, avant toute bufferisation :

```
si LENGTH > 1024  →  rejeter immédiatement, retourner en recherche de MAGIC
```

Attendre le CRC pour rejeter obligerait à bufferiser une quantité de données
potentiellement ingérable. Chaque champ est validé contre les contraintes connues
dès sa lecture.

### Timeouts

Le bootloader arme un timer à chaque trame **entièrement traitée**, et non à la
réception d'un octet quelconque. Le compteur ne court donc pas pendant les opérations
d'écriture flash, qui peuvent durer plusieurs dizaines de millisecondes.

| Phase                     | Timeout |
|---------------------------|---------|
| Attente de `CMD_START`    | 2 s     |
| Entre deux `CMD_DATA`     | 5 s     |
| Côté PC, attente d'un ACK | 1 s     |

Le PC retransmet jusqu'à 3 fois avant d'abandonner.

---

## 7. États et métadonnées

### États du firmware

| Valeur | État          | Signification                                        |
|--------|---------------|------------------------------------------------------|
| `0x00` | `EMPTY`       | Slot vide ou jamais programmé                        |
| `0x01` | `IN_PROGRESS` | Transfert commencé, non terminé                      |
| `0x02` | `TESTING`     | Image installée, en attente de confirmation applicative |
| `0x03` | `VALID`       | Image validée, bootable sans réserve                 |

### Ordonnancement fail-safe

L'ordre des écritures est contraint par une règle : **le marqueur d'invalidation est
toujours écrit avant l'opération destructive**, jamais après.

Au démarrage d'un transfert :

1. Écrire `state = IN_PROGRESS` dans les métadonnées
2. Effacer le slot cible
3. Écrire les blocs

L'ordre inverse laisserait une fenêtre pendant laquelle les métadonnées affirmeraient
qu'un firmware valide existe alors qu'il vient d'être effacé — un état *menteur*.
Avec l'ordre correct, le pire cas est un système qui croit à tort qu'un transfert a
échoué, alors que l'ancienne image est intacte : du pessimisme inutile, mais pas de
perte de données.

À la fin d'un transfert :

1. Vérifier le CRC global par relecture
2. Écrire `state = TESTING`, `active_slot = cible`
3. Redémarrer

### Mécanisme de test et rollback

Un CRC valide prouve l'**intégrité** de l'image, pas son **bon fonctionnement**. Un
firmware transmis sans la moindre corruption peut planter dès sa première seconde.

L'état `TESTING` répond à ce cas :

- Le bootloader saute vers l'image en `TESTING`
- Si l'application démarre correctement, **elle écrit elle-même** `state = VALID`
- Si elle plante, le watchdog provoque un reset ; le bootloader constate `TESTING`
  sans confirmation, incrémente un compteur d'échecs et bascule sur le slot précédent

L'application porte donc une responsabilité : confirmer son propre bon fonctionnement.
Sans cette confirmation, le rollback ne peut pas fonctionner.

---

## 8. Séquence nominale

```
PC                                    Bootloader
│                                              │
│──── CMD_GET_INFO (SEQ=0) ───────────────────▶│
│◀─── RSP_INFO {free_slot=B, fw_ver=1.0} ──────│
│                                              │
│──── CMD_START_UPDATE (SEQ=0) ───────────────▶│  vérifie taille, slot, version
│     {size, crc32, ver, slot=B}               │  écrit IN_PROGRESS
│◀─── RSP_ACK (SEQ=0) ─────────────────────────│  efface le slot B
│                                              │
│──── CMD_DATA (SEQ=1) [256 o] ───────────────▶│  écrit + relit
│◀─── RSP_ACK (SEQ=1) ─────────────────────────│
│                                              │
│──── CMD_DATA (SEQ=2) [256 o] ───────────────▶│
│◀─── RSP_ACK (SEQ=2) ─────────────────────────│
│              ...                             │
│──── CMD_DATA (SEQ=N) ───────────────────────▶│
│◀─── RSP_ACK (SEQ=N) ─────────────────────────│
│                                              │
│──── CMD_END_UPDATE (SEQ=N+1) ───────────────▶│  relit tout, vérifie CRC global
│◀─── RSP_ACK (SEQ=N+1) ───────────────────────│  écrit TESTING, reset
│                                              │
```

---

## 9. Machine à états du bootloader

```
        ┌──────────┐
        │   IDLE   │◀──────────────────────┐
        └────┬─────┘                       │
             │ CMD_START_UPDATE valide     │
             ▼                             │
      ┌─────────────┐                      │
      │  ERASING    │                      │
      └──────┬──────┘                      │
             │ effacement terminé          │
             ▼                             │
      ┌─────────────┐   CMD_DATA           │
      │  RECEIVING  │◀────────┐            │
      └──────┬──────┘─────────┘            │
             │ CMD_END_UPDATE              │
             ▼                             │
      ┌─────────────┐                      │
      │  VERIFYING  │                      │
      └──────┬──────┘                      │
             │                             │
     ┌───────┴────────┐                    │
     │ CRC OK         │ CRC KO             │
     ▼                ▼                    │
┌──────────┐   ┌──────────────┐            │
│  TESTING │   │ NACK + ABORT │────────────┘
│  + reset │   └──────────────┘
└──────────┘
```

Un `CMD_ABORT` ou un timeout depuis n'importe quel état ramène à `IDLE` et remet les
métadonnées sur l'ancien slot valide.

---

## 10. Limites connues

Ce protocole assure l'**intégrité**, pas l'**authenticité**. Un CRC32 détecte la
corruption accidentelle ; il n'offre aucune protection contre un firmware
délibérément malveillant, un CRC étant trivialement recalculable par un attaquant.

Une version durcie remplacerait le CRC global par une signature asymétrique (ECDSA
P-256, par exemple), la clé publique étant stockée dans une zone flash protégée en
écriture. Le mécanisme de transport resterait identique ; seule l'étape de validation
finale changerait.

Cette limite est assumée dans le cadre de ce projet, dont l'objet est la maîtrise du
mécanisme de mise à jour et non la sécurisation cryptographique.

---

## 11. Historique

| Version | Modifications           |
|---------|-------------------------|
| 1       | Spécification initiale  |
