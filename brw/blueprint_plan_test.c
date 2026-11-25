#include "blueprint.h"
#include <stdio.h>
#include <string.h>

static int check(const char *root, const char *expected)
{
    const char *child = "export const inputs=['v'];"
        "export default a=>{const value=a.v; a.v='changed'; return [{op:'env',name:'MODE',value}]}";
    struct bp_bundle bundle = {0};
    struct bp_plan plan = {0};
    char detail[BP_DETAIL_CAP] = "";
    bundle.entry = "root.mjs";
    bundle.required = 1u << BP_ENV;
    bundle.count = 2;
    bundle.files[0] = (struct bp_file){"root.mjs", "", (unsigned char *)root, strlen(root), true};
    bundle.files[1] = (struct bp_file){"child.mjs", "", (unsigned char *)child, strlen(child), true};
    const char *error = bp_build(&bundle, &plan, detail);
    int failed = expected ? (!error || strcmp(error, expected)) :
        (error || plan.count != 2 || strcmp(plan.steps[0].source, "root.mjs[1] > child.mjs[1]") ||
         strcmp(plan.steps[0].value, "debug") || strcmp(plan.steps[1].value, "release") ||
         strcmp(plan.steps[1].source, "root.mjs[2] > child.mjs[1]"));
    if (failed) fprintf(stderr, "expected %s, got %s (%s)\n",
                        expected ? expected : "plan", error ? error : "plan", detail);
    bp_plan_free(&plan);
    return failed;
}

int main(void)
{
    return check("export default ()=>['debug','release'].map(v=>"
                 "({op:'include',module:'./child.mjs',args:{v}}))", NULL) ||
        check("export default ()=>{const args={v:'debug'}, module='./child.mjs', op='include';"
              "return [{op,module,args},{op,module,args:{get v(){return args.v==='debug'?'release':'changed'}}}]}", NULL) ||
        check("import '../child.mjs'; export default ()=>[]", "invalid-import") ||
        check("export default ()=>[{op:'include',module:'./child.mjs',args:{}}]", "missing-input") ||
        check("export default ()=>[{op:'include',module:'./root.mjs',args:{}}]", "blueprint-cycle") ||
        check("export default ()=>{function again(){return again()} return again()}", "blueprint-limit") ||
        check("export default ()=>['x'.repeat(32*1024*1024)]", "blueprint-limit") ||
        check("export default ()=>{for(;;){}}", "blueprint-limit");
}
