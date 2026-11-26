export const inputs = ["mode"];
export const outputs = ["archive"];

export default ({mode}) => [{
  op: "run",
  tool: "builder",
  args: [{payload: "build.py"}, mode, "argument with spaces", {outputFile: "archive"}]
}];
