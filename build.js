const fs = require("fs");
const os = require("os");
const { spawnSync } = require("child_process");
const node = __dirname;

// Download the record/replay driver archive, using the latest version unless
//it was overridden via the environment.
let driverArchive = `${currentPlatform()}-recordreplay.tgz`;
let downloadArchive = driverArchive;
if (process.env.DRIVER_REVISION) {
  downloadArchive = `${currentPlatform()}-recordreplay-${process.env.DRIVER_REVISION}.tgz`;
}
const driverFile = `${currentPlatform()}-recordreplay.${driverExtension()}`;
const driverJSON = `${currentPlatform()}-recordreplay.json`;
spawnChecked("curl", [`https://static.replay.io/downloads/${downloadArchive}`, "-o", driverArchive], { stdio: "inherit" });
spawnChecked("tar", ["xf", driverArchive]);
fs.unlinkSync(driverArchive);

// Embed the driver in the source.
const driverContents = fs.readFileSync(driverFile);
const { revision: driverRevision, date: driverDate } = JSON.parse(fs.readFileSync(driverJSON, "utf8"));
fs.unlinkSync(driverFile);
fs.unlinkSync(driverJSON);
let driverString = "";
for (let i = 0; i < driverContents.length; i++) {
  driverString += `\\${driverContents[i].toString(8)}`;
}
fs.writeFileSync(
  `${node}/src/node_record_replay_driver.cc`,
  `
namespace node {
  char gRecordReplayDriver[] = "${driverString}";
  int gRecordReplayDriverSize = ${driverContents.length};
  char gBuildId[] = "${computeBuildId()}";
}
`
);

const numCPUs = os.cpus().length;

if (process.env.CONFIGURE_NODE) {
  spawnChecked(`${node}/configure`, [], { cwd: node, stdio: "inherit" });
}
spawnChecked("make", [`-j${numCPUs}`, "-C", "out", "BUILDTYPE=Release"], {
  cwd: node,
  stdio: "inherit",
  env: {
    ...process.env,
    // Disable recording when node runs as part of its compilation process.
    RECORD_REPLAY_DONT_RECORD: "1",
  },
});

function spawnChecked(cmd, args, options) {
  const prettyCmd = [cmd].concat(args).join(" ");
  console.error(prettyCmd);

  const rv = spawnSync(cmd, args, options);

  if (rv.status != 0 || rv.error) {
    console.error(rv.error);
    throw new Error(`Spawned process failed with exit code ${rv.status}`);
  }

  return rv;
}

function currentPlatform() {
  switch (process.platform) {
    case "darwin":
      return "macOS";
    case "linux":
      return "linux";
    default:
      throw new Error(`Platform ${process.platform} not supported`);
  }
}

function driverExtension() {
  return currentPlatform() == "windows" ? "dll" : "so";
}

/**
 * @returns {string} "YYYYMMDD" format of UTC timestamp of given revision.
 */
function getRevisionDate(
  revision = "HEAD",
  spawnOptions
) {
  const dateString = spawnChecked(
    "git",
    ["show", revision, "--pretty=%cd", "--date=iso-strict", "--no-patch"],
    spawnOptions
  )
    .stdout.toString()
    .trim();

  // convert to UTC -> then get the date only
  // explanations: https://github.com/replayio/backend/pull/7115#issue-1587869475
  return new Date(dateString).toISOString().substring(0, 10).replace(/-/g, "");
}

/**
 * WARNING: We have copy-and-pasted `computeBuildId` into all our runtimes and `backend`.
 * When changing this: always keep all versions of this in sync, or else, builds will break.
 */
function computeBuildId() {
  const runtimeRevision = spawnChecked("git", ["rev-parse", "--short=12", "HEAD"]).stdout.toString().trim();
  const runtimeDate = getRevisionDate();

  // Use the later of the two dates in the build ID.
  const date = +runtimeDate >= +driverDate ? runtimeDate : driverDate;

  return `${currentPlatform()}-node-${date}-${runtimeRevision}-${driverRevision}`;
}
