# Running a server

From an empty Windows machine to a running shard. A **shard** is one game world: a front machine (gateway and login),
one or more game servers that split the world between them, a PostgreSQL database, and optionally the website's API.
Everything can run on one machine; this guide does that first, then says what changes when you spread it out.

## 1. What you need

- **The programs**: a release of Silkroad Reimagined (`SroSetup.exe`, `SroControl.exe`, `SroGame.exe`, `SroWeb.exe` and
  their DLLs), or your own build (`cmake --build --preset server-release`).
- **PostgreSQL 14 or newer.**
- **Your own original files** (we provide none; see the [README](../README.md#what-you-need-your-own-original-files)):
  - a **vSRO 1.188 client** folder: the game servers read its maps, navigation and tables;
  - **`SRO_VT_SHARD.bak`**: the vSRO 1.188 server's shard database backup;
  - **`Quest.sct`**: the vSRO 1.188 server's compiled quest script (`Script\VIETNAM\Quest.sct` in the server files).

Only the shard backup is read: the original's other databases (`SRO_VT_ACCOUNT`, `SRO_VT_LOG`, `SRO_CERTIFICATION`) are
not needed. From it `SroSetup` takes only the game's reference data (items, monsters, skills, drops, spawns,
schedules, ...): your server starts with no accounts and no characters. `SQL Server` is **not** needed: the backup
file is read directly.

## 2. The folder

Unpack the programs into a folder of their own, for example `C:\SroServer`, and give it this shape:

```
C:\SroServer\
    SroSetup.exe, SroControl.exe, SroGame.exe, SroWeb.exe, *.dll
    shard.cfg                 <- a copy of deploy\shard.example.cfg, edited (step 4)
    data\
        SRO_VT_SHARD.bak      <- yours
        Quest.sct             <- yours
```

The client folder stays where it is; `shard.cfg` says where (`game = ...`).

## 3. The database

Install PostgreSQL (its installer asks for a password for the `postgres` superuser). Then make a user and a database
for the shard, in **SQL Shell (psql)** as `postgres`:

```sql
CREATE ROLE sro LOGIN PASSWORD 'choose-a-long-password';
CREATE DATABASE sro_live OWNER sro;
```

Use a password of your own; nobody but the servers needs to know it. Keep PostgreSQL closed to the internet: only your
game machines may reach it (on one machine it listens on `127.0.0.1` only, the default).

## 4. The settings: `shard.cfg`

Copy `deploy\shard.example.cfg` to `shard.cfg` and set at least these:

```ini
shard-name = MyShard

# The database (step 3).
db-host = 127.0.0.1
db-port = 5432
db-name = sro_live
db-user = sro
# db-password = ...        # better: the environment variable SR_DB_PASSWORD (see below)

# Your files (step 2): the client folder, and the folder with SRO_VT_SHARD.bak and Quest.sct.
game = C:\Games\vSRO_1188
data = data

# Where players connect: the machine's public name or address.
login-host = play.example.com
tls-name = play.example.com

# One machine, the whole world (the example has two game servers; use this instead):
server.1.name = World
server.1.public-host = play.example.com
server.1.port = 15781
server.1.regions = *
```

**The database password.** It may be written in `shard.cfg` as `db-password`, but then anyone who can read the file has
it. Better: set it for the user that runs the servers, once, in PowerShell:

```powershell
[Environment]::SetEnvironmentVariable("SR_DB_PASSWORD", "choose-a-long-password", "User")
```

(`SR_DB_URL = postgresql://sro:PASSWORD@host:5432/sro_live` still works too, and wins over the `db-*` keys.)

The rest of `shard.cfg` (rates, the level cap, events, PK) is explained in its own comments.

## 5. Setup: `SroSetup`

```
SroSetup.exe --config shard.cfg
```

It does, in order, and says what it did or what is wrong at each step:

1. **The files**: finds `data\SRO_VT_SHARD.bak`, `data\Quest.sct` and the client folder; checks they are vSRO 1.188
   (the database's items and monsters must be the client's).
2. **The database**: connects with the `db-*` settings and makes our tables (an empty layout: accounts, characters,
   items, guilds, ...).
3. **The game's data**: reads the reference tables out of the backup into the database.
4. **The quests**: reads `Quest.sct` into the quest table.
5. **The certificate**: makes the server's TLS certificate for `tls-name` if there is none (`dev\server.pem` and
   `dev\server.key`). The client is given `server.pem`: it trusts that certificate alone.

A first run takes a few minutes. Running it again changes nothing that is already there; after you update the
programs, run it again: it brings the database's layout up to the new version. `SroSetup --reimport` reads the game's
data and the quests again (after you replace a file); your players' accounts and characters stay.

The servers refuse to start on a database `SroSetup` has not prepared, and say so.

## 6. Start

Each in its own window (or as a service), from the folder:

```
SroControl.exe --config shard.cfg             gateway :15779, login :15780
SroGame.exe    --config shard.cfg --id 1      game server 1 :15781
SroWeb.exe     --config shard.cfg             the website's API :15790 (optional)
```

`SroControl.exe --config shard.cfg --status` prints which game servers are up.

**Firewall:** open the gateway (15779), the login (15780) and each game server's port (15781, ...) to players. The
website's API stays local; put a reverse proxy (Caddy, nginx) with HTTPS in front of it.

**Players** need `server.pem` from step 5 (the launcher brings it) and the gateway's address.

## 7. Accounts and game masters

Players register on the website (`SroWeb`), or you make an account by hand:

```
SroLogin.exe --config shard.cfg --create-account NAME --password PASSWORD
```

A game master is an account with the original's GM flag. In psql, connected to `sro_live`:

```sql
UPDATE sro_vt_account.tb_user SET sec_primary = 1 WHERE struserid = 'NAME';
```

## 8. More than one machine

The same `shard.cfg` on every machine. The front machine runs `SroControl`; each game machine runs `SroGame --id N`
for its `server.N.*` lines; every machine needs the client folder (`game`), and only the machine running `SroSetup`
needs `data\`. Point `db-host` at the database's machine and let PostgreSQL accept those machines alone
(`pg_hba.conf`). The game servers of a shard talk only through the database: they never connect to each other.

## 9. When something is wrong

| It says | What to do |
|---|---|
| `SroSetup: SRO_VT_SHARD.bak not found` | put it in the `data` folder named in `shard.cfg` |
| `... is not vSRO 1.188` / `the client's items do not match` | the backup and the client are of different versions; use both from 1.188 |
| `cannot connect to the database` | check `db-host`, `db-port`, `db-name`, `db-user` and the password; is PostgreSQL running? |
| `permission denied for schema` | the database's owner must be `db-user` (`CREATE DATABASE ... OWNER sro`) |
| a game server: `run SroSetup first` | step 5 |
| players: `the server's certificate is not trusted` | they need the `server.pem` of this server (made for its `tls-name`) |
