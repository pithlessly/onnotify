#!/usr/bin/env python3

import os, sys, fcntl, contextlib, re, time, threading, subprocess

@contextlib.contextmanager
def exclusive_lock(f):
    fcntl.flock(f, fcntl.LOCK_EX)
    try:
        yield
    finally:
        fcntl.flock(f, fcntl.LOCK_UN)

color  = not os.environ.get("NO_COLOR")
red    = "\x1b[91m" if color else ""
yellow = "\x1b[93m" if color else ""
reset  = "\x1b[m"   if color else ""

# number of seconds between updating the metadata DB
DB_UPDATE_INTERVAL = 15

progname = sys.argv[0]
cwd = os.getcwd().encode("utf8") # expands symlinks automatically

def error(e): print(f"{progname} {red}error{reset}: {e}", file=sys.stderr)
def warn(e): print(f"{progname} {yellow}warning{reset}: {e}", file=sys.stderr)

class Failure(RuntimeError):
    pass

def main(argv):
    my_id = os.getpid()
    whoami = os.environ.get("LOGNAME")
    if not whoami:
        raise Failure("LOGNAME is unset")
    if '/' in whoami:
        raise Failure("LOGNAME contains a slash")
    db_dir = f"/tmp/notifydb.{whoami}"
    os.makedirs(db_dir, exist_ok=True)
    db_file = f"{db_dir}/db"
    cmd = argv[1:]
    do_clear = True
    if cmd and cmd[0] in {"--no-clear", "-c"}:
        do_clear = False
        cmd = cmd[1:]
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if cmd:
        runner = Runner(cmd, do_clear)
        runner.early_clear()
    else:
        runner = NotificationRunner()
    with open(db_file, "a+b") as f:
        with exclusive_lock(f):
            allowed_fifo_files = db_update(db_dir, my_id, f, True)
            db_prune_old_fifo_files(db_dir, allowed_fifo_files)
        shutdown = threading.Event()
        thread = threading.Thread(target=db_update_thread, args=(db_dir, my_id, f, shutdown))
        thread.start()
        try:
            db_fifo_thread(db_dir, my_id, runner)
        finally:
            shutdown.set()
            thread.join()

def parse_record(s, regex=re.compile(rb"^(\d+) (\d+) (.+)\n$")):
    m = regex.match(s)
    if not m:
        warn(f"ignoring malformed record:\n{s}")
        return None
    return (int(m.group(1)),
            int(m.group(2)),
            m.group(3))

def unparse_record(parts):
    return b" ".join(str(p).encode() if isinstance(p, int) else p for p in parts) + b"\n"

def db_update(db_dir, my_id, f, create_record):
    f.seek(0)
    now = int(time.time())
    new_records = []
    n_outdated = 0
    for record in f:
        record = parse_record(record)
        if not record: continue
        (time_, id_, path) = record
        if now - time_ >= DB_UPDATE_INTERVAL * 2: n_outdated += 1; continue
        if id_ == my_id: continue
        new_records.append(record)
    if n_outdated > 0:
        warn(f"erased {n_outdated} outdated record{'s' * (n_outdated != 1)}")
    fifo_path = f"{db_dir}/fifo.{my_id}"
    if create_record:
        new_records.append((now, my_id, cwd))
        try:
            os.mkfifo(fifo_path)
        except FileExistsError:
            pass
    else:
        os.remove(fifo_path)
    allowed_fifo_files = {f"fifo.{id_}" for _, id_, _ in new_records}
    content = b"".join(map(unparse_record, new_records))
    f.truncate(0)
    f.write(content)
    f.flush()
    return allowed_fifo_files

def db_prune_old_fifo_files(db_dir, allowed_fifo_files):
    for f in os.listdir(db_dir):
        if f.startswith("fifo.") and f not in allowed_fifo_files:
            os.remove(f"{db_dir}/{f}")

def db_update_thread(db_dir, my_id, db_f, shutdown):
    try:
        while shutdown.wait(DB_UPDATE_INTERVAL) == False:
            with exclusive_lock(db_f):
                db_update(db_dir, my_id, db_f, True)
    finally:
        with exclusive_lock(db_f):
            db_update(db_dir, my_id, db_f, False)

class Runner():
    def clear(self):
        if not self.do_clear: return
        ANSI = "\x1b[3J\x1b[H\x1b[2J"
        print(end=ANSI, flush=True)
    def __init__(self, cmd, do_clear):
        self.cmd = cmd
        self.child_process = None
        self.do_clear = do_clear
        self.cleared = False
    def early_clear(self):
        self.clear()
        self.cleared = True
    def start_child(self):
        if self.child_process is not None:
            self.child_process.terminate()
        self.child_process = subprocess.Popen(self.cmd)
    def run(self):
        if not self.cleared:
            self.clear();
        self.start_child()
        self.cleared = False
    startup = run

class NotificationRunner():
    def run(self):
        import datetime
        time = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{time}] received notification")
    def startup(self):
        print(f"[{progname}] waiting for notifications")

def db_fifo_thread(db_dir, my_id, runner):
    runner.startup()
    fifo_path = f"{db_dir}/fifo.{my_id}"
    with open(fifo_path, "rb") as fifo:
        while True:
            # block until some data is available
            data = fifo.read(16)
            if not data:
                time.sleep(0.05)
                continue
            for _ in data:
                runner.run()

if __name__ == "__main__":
    try:
        main(sys.argv)
    except Failure as e:
        error(e)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(0)
