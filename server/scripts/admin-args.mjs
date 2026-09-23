// The command line of scripts/admin.mjs: its usage text and the parsing of
// its arguments.

export function usage(msg) {
    if (msg) {
        process.stderr.write(`error: ${msg}\n`);
    }
    process.stderr.write(
        "usage: node scripts/admin.mjs --server URL alerts [--all]\n" +
            "       node scripts/admin.mjs --server URL resolve --alert ID --note TEXT\n" +
            "       node scripts/admin.mjs --server URL decide --unit ID (--digest HEX | --requeue | --withdraw) --note TEXT\n" +
            "ADMIN_SECRET (at least 32 characters) must be set in the environment.\n",
    );
    process.exit(2);
}

const count = (s, what) => {
    if (!/^[0-9]{1,15}$/.test(s) || Number(s) < 1) {
        usage(`${what} needs an integer of at least 1`);
    }
    return Number(s);
};

export function parseArgs(argv) {
    const o = { all: false };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const value = () => {
            if (argv[i + 1] === undefined) {
                usage(`${a} needs a value`);
            }
            return argv[++i];
        };
        if (a === "--server") {
            o.server = value().replace(/\/+$/, "");
        } else if (a === "--note") {
            o.note = value();
        } else if (a === "--alert") {
            o.alert = count(value(), a);
        } else if (a === "--unit") {
            o.unit = count(value(), a);
        } else if (a === "--digest") {
            o.digest = value();
            if (!/^[0-9a-f]{64}$/.test(o.digest)) {
                usage("--digest needs a result digest (64 lowercase hex digits)");
            }
        } else if (a === "--requeue") {
            o.requeue = true;
        } else if (a === "--withdraw") {
            o.withdraw = true;
        } else if (a === "--all") {
            o.all = true;
        } else if (["alerts", "resolve", "decide"].includes(a) && o.command === undefined) {
            o.command = a;
        } else {
            usage(`unknown argument ${a}`);
        }
    }
    return o;
}
