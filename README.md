# afpfs-ng - Apple Filing Protocol Client Library

**Version 0.8.2**

afpfs-ng is a client implementation of the Apple Filing Protocol (AFP) written in C. It provides a library (`libafpclient`) and command-line tools for accessing AFP shares from Linux, FreeBSD, and other Unix-like systems.

## Compatible Servers

- Mac OS X / macOS computers
- Netatalk (Linux AFP server)
- Apple Time Capsule and AirPort Express
- Various NAS devices with AFP support

## Components

| Component | Description |
|-----------|-------------|
| `libafpclient` | Core AFP client library (shared and static) |
| `afpcmd` | Interactive command-line AFP client (FTP-like) |
| `afpgetstatus` | Query server status without authenticating |
| `fuse/` | FUSE-based filesystem mount daemon (`afpfsd`, `mount_afp`) |

## Build Dependencies

| Dependency | Required for |
|------------|-------------|
| libgcrypt | Encrypted login methods (DHX2, etc.) |
| libgmp | Cryptographic operations |
| libreadline + libncurses | Command-line client (`afpcmd`) |
| libfuse (>= 2.7.0) | FUSE filesystem mounting (optional) |

## Building

```bash
./configure && make && sudo make install
```

### Configure options

| Flag | Description |
|------|-------------|
| `--disable-fuse` | Build without FUSE support |
| `--disable-gcrypt` | Build without libgcrypt (limits UAM support) |

On FreeBSD or systems where dependencies live in `/usr/local`:

```bash
CFLAGS="-I/usr/local/include -L/usr/local/lib" ./configure && make && sudo make install
```

## Usage

### Command-line client (`afpcmd`)

Interactive AFP shell with command history and filename completion:

```bash
# Connect to a volume
afpcmd afp://username:password@hostname.local/volumename

# Anonymous connection, list volumes
afpcmd afp://hostname.local/
```

Supported commands: `cd`, `ls`, `get`, `put`, `mkdir`, `rmdir`, `delete`, `rename`, `touch`, `chmod`, `pwd`, `lpwd`, `lcd`, `status`, `statvfs`, `passwd`, `user`, `disconnect`, `help`, `exit`.

Batch mode for file transfers:

```bash
afpcmd afp://user:pass@server/Path/to/file.tar.bz2
```

### FUSE mounting

Start the management daemon:

```bash
afpfsd -d   # -d for debug output
```

Mount a volume (Mac OS X style syntax):

```bash
mount_afp afp://username:password@hostname.local/volumename /mnt/mountpoint
mount_afp afp://username;AUTH=DHX2:password@hostname.local/volumename /mnt/mountpoint
```

Mount read-only:

```bash
mount_afp -o ro afp://user:pass@hostname/volume /mnt/mountpoint
```

Unmount:

```bash
fusermount -u /mnt/mountpoint
```

### Server status

```bash
afpgetstatus afp://hostname.local
```

## Protocol Support

- AFP 3.x (including 3.2/3.3 partial support)
- AFP 2.x (partial)
- IPv6 via `getaddrinfo()`
- Multiple UAMs (User Authentication Methods) including DHX2

## License

GPL (same as the original afpfs-ng)

## Credits

This is a fork of the original afpfs-ng project (now unmaintained). It includes patches from the XBMC project and upstream contributions.

Original project: https://sites.google.com/site/alexthepuffin/home

Contact: simon.vetter@gmx.com

See `AUTHORS` for a list of contributors.
