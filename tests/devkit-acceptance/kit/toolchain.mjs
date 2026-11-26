export default () => [
  {
    op: "host",
    required: "empty",
    check: {
      program: "powershell.exe",
      args: ["-NoProfile", "-NonInteractive", "-Command",
        "if (@(Get-ChildItem -Force -ErrorAction Stop).Count) { 'occupied' } else { 'empty' }"]
    }
  },
  {op: "tool", registry: "devkit", name: "w64devkit", version: "2.11.0"},
  {op: "run", tool: "w64devkit", args: ["-y", "-o."]}
];
