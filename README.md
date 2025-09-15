# OrionFS

OrionFS is a distributed collaborative text file store written in C. It models a small network file system with a central Naming Server, multiple Storage Servers, and interactive clients that can create, read, edit, stream, execute, checkpoint, and share text files.

The project is built around three processes:

- **Naming Server (NM):** owns metadata, access control, routing, storage-server registration, logging, and execution requests.
- **Storage Server (SS):** stores the physical files, serves direct client reads/writes/streams, maintains backups, and handles checkpoint data.
- **Client:** connects to the Naming Server, receives routing information, and talks directly to Storage Servers for file operations.

## What It Supports

- Distributed file CRUD: `CREATE`, `READ`, `WRITE`, `DELETE`, `VIEW`, and `INFO`.
- Sentence-level collaborative writes with fine-grained locks.
- Conflict-aware write remapping so stale sentence indexes are not blindly overwritten.
- Word-by-word streaming through `STREAM`.
- Server-side command execution from file contents through `EXEC`.
- Owner-managed access control with direct grants, removals, requests, approvals, and denials.
- Folder operations: `CREATEFOLDER`, `MOVE`, and `VIEWFOLDER`.
- Checkpoints: create, list, view, and revert named file snapshots.
- Single-level undo for the most recent committed file change.
- Best-effort replication and fallback reads from available replicas.
- Continuous backup mirroring that survives file deletion.
- Metadata lookup through a trie plus an LRU cache for hot paths.

## Architecture

```text
Client
  |
  | control plane: commands, metadata, permissions
  v
Naming Server
  |
  | storage routing, create/delete, replication metadata
  v
Storage Servers

For READ, WRITE, STREAM, UNDO, and checkpoint file content operations,
the client receives SS location details from the NM and then connects
directly to the selected Storage Server.
```

The Naming Server keeps global file metadata, user access information, server health, and routing state. Storage Servers own the actual file bytes under `storage_server/data/ss_storage/`, including primary files, replicas, checkpoint folders, and backup copies.

## Repository Layout

```text
.
├── Makefile
├── README.md
└── OrionFS
    ├── Makefile
    ├── client
    │   ├── Makefile
    │   └── client.c
    ├── common
    │   ├── network.c
    │   ├── network.h
    │   ├── protocol.h
    │   ├── utils.c
    │   └── utils.h
    ├── naming_server
    │   ├── Makefile
    │   ├── naming_server.c
    │   ├── nm_storage.c
    │   └── nm_storage.h
    └── storage_server
        ├── Makefile
        └── storage_server.c
```

Runtime metadata, logs, compiled binaries, and sample storage data also live under the component directories.

## Build

From the project implementation directory:

```bash
cd OrionFS
make clean
make
```

This builds the Naming Server, Storage Server, and Client by invoking each component Makefile.

## Run

Open separate terminals from the `OrionFS/` directory.

Start the Naming Server:

```bash
cd naming_server
./naming_server 7071
```

Start one or more Storage Servers:

```bash
cd storage_server
./storage_server SS1 7071 127.0.0.1 7001 10001
```

Arguments:

```text
./storage_server <ss_id> <nm_port> <nm_ip> <ss_nm_port> <ss_client_port>
```

Start a client:

```bash
cd client
./client 127.0.0.1 7071
```

Enter a username when prompted, then use `HELP` inside the client to see the full command list.

## Client Commands

```text
VIEW [-a] [-l]                 List visible files
CREATE <filename>              Create a file
READ <filename>                Read file contents
WRITE <file> <sentence#>       Edit a sentence interactively
DELETE <filename>              Delete a file
INFO <filename>                Show metadata
LIST                           List known users
ADDACCESS -R|-W <file> <user>  Grant access
REMACCESS <file> <user>        Remove access
REQUESTACCESS <file> -R|-W     Request read/write access
VIEWREQUESTS                   View incoming access requests
APPROVEREQUEST <file> <user> -R|-W
DENYREQUEST <file> <user>
UNDO <filename>                Restore the previous version
STREAM <filename>              Stream file contents word by word
EXEC <filename>                Execute file contents on the NM
CREATEFOLDER <name>            Create a folder
MOVE <file> <folder>           Move a file into a folder
VIEWFOLDER <name>              List folder contents
CHECKPOINT <file> <tag>        Create a named checkpoint
VIEWCHECKPOINT <file> <tag>    Read checkpoint contents
REVERT <file> <tag>            Revert to a checkpoint
LISTCHECKPOINTS <file>         List checkpoints
EXIT                           Quit the client
```

## Write Workflow

`WRITE` opens an interactive edit session for a sentence number.

```text
WRITE story.txt 1
1 Once
2 upon
3 a
4 time.
ETIRW
```

During the session, each line is:

```text
<word_index> <content>
```

`ETIRW` commits the edit. OrionFS locks only the target sentence while the edit is active, then remaps the sentence at commit time to avoid overwriting a concurrently changed sentence by accident.

## Data Handling Notes

- Primary files are stored below `storage_server/data/ss_storage/SS<ID>/`.
- Backup files are mirrored below `storage_server/data/ss_storage/backup/`.
- Checkpoints are stored below folder-local `checkpoints/<filename>/<tag>.chk` paths.
- Deleting a file removes the primary file and metadata, but the backup copy is retained.
- Undo is single-level and restores the last committed content snapshot for the file.

## Implementation Highlights

- TCP networking with pthread-based concurrent request handling.
- Shared message protocol in `common/protocol.h`.
- Trie-backed metadata lookup with an LRU cache for recently accessed paths.
- Round-robin Storage Server selection for new primary files.
- Replica selection and fallback when the primary server is unavailable.
- Sentence-level write locks instead of whole-file edit locks.
- Logging for Naming Server and Storage Server activity.
