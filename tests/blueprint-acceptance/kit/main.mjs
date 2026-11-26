const project = (...args) => ({program: "python", args: [{payload: "project.py"}, ...args]});

export default () => [
  {op: "state", check: project("check"), unmet: [3], change: project("prepare")},
  {op: "tool", registry: "sdk", name: "builder", version: "2"},
  ...["windows", "linux", "macos"].map(platform => ({
    op: "env", platform, name: "BRW_ACCEPT_PLATFORM", value: platform
  })),
  ...["Debug", "Release"].map(mode => ({
    op: "include", module: "./build/job.mjs", args: {mode}
  })),
  {op: "run", ...project("test", {from: 7, output: "archive"})}
];
