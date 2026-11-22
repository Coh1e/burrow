// Module test: a second package source adapter can reuse the IR contract, the validator and the
// planner without any Scoop JSON, Windows API or execution backend. The fixture builds IR by hand
// from a source shape that is not a Scoop manifest and checks the published contract directly.
#include "ir.hpp"
#include <algorithm>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

// A hypothetical second adapter: its own source declares a package as a name, version, one
// payload and a list of command words. It normalizes that into the shared IR, exactly like the
// Scoop adapter does, without the validator or planner knowing the source shape.
burrow::ir::Package adapter_from_foreign_source() {
    burrow::ir::Package package;
    package.name = "foreign-tool";
    package.bucket = "vendor";
    package.version = "2.4";
    package.architecture = "64bit";
    package.manifest_digest = std::string(64,'a');
    burrow::ir::Download payload;
    payload.url = "https://vendor.example/foreign-tool.zip";
    payload.filename = "foreign-tool.zip";
    payload.digest.algorithm = burrow::ir::DigestAlgorithm::sha256;
    payload.digest.value = std::string(64,'b');
    payload.digest.declared = true;
    payload.format = burrow::ir::ArchiveFormat::zip;
    payload.extract_directory = "bin";
    package.downloads.push_back(payload);
    package.declared_hash_count = 1;
    package.entries.push_back({"foreign-tool.exe","foreign",{"--vendor"}});
    package.persistence.push_back({"config","config"});
    package.environment.push_back({"FOREIGN_HOME","$dir",false});
    package.hooks.push_back({burrow::ir::HookStage::post_install,
        "Write-Output 'foreign adapter hook'","foreign-source"});
    package.dependencies.push_back({"vendor/runtime","runtime","vendor",{}});
    package.psmodule = burrow::ir::PsModule{"Foreign.Tool"};
    return package;
}

std::string operations(const std::vector<burrow::ir::Step>& steps) {
    std::string result;
    for (const auto& step : steps) {
        if (!result.empty()) result += ',';
        result += burrow::ir::operation_name(step.operation);
    }
    return result;
}
}

int main() {
    auto package = adapter_from_foreign_source();
    burrow::ir::validate(package);
    check(package.diagnostics.empty(),"the validator reported a gap for a valid non-Scoop package");
    bool command_entry = false, lifecycle = false, ps_module = false, native_download = false;
    for (const auto& capability : package.capabilities) {
        command_entry = command_entry || capability.id == "command-entry";
        lifecycle = lifecycle || capability.id == "lifecycle-hooks";
        ps_module = ps_module || capability.id == "ps-module";
        native_download = native_download || capability.id == "native-download";
        check(capability.supported,"capability "+capability.id+" is not supported for a valid package");
    }
    check(command_entry && lifecycle && ps_module && native_download,
        "the validator did not publish the capabilities of the normalized package");

    const auto installation = burrow::ir::plan(burrow::ir::Action::install,package,nullptr,true);
    check(operations(installation.steps) ==
        "download,verify,extract,publish-current,entries,ps-module,environment,persistence,hook,commit-state",
        "the planner did not produce the install sequence for the foreign package: "+operations(installation.steps));
    for (const auto& step : installation.steps)
        check(!step.effect.empty(),"a planned step carries no stated effect");
    const auto verify = std::find_if(installation.steps.begin(),installation.steps.end(),
        [](const burrow::ir::Step& step) { return step.operation == burrow::ir::Operation::verify; });
    check(verify != installation.steps.end() && verify->effect.find("sha256") != std::string::npos,
        "the verify effect does not name the declared digest algorithm");

    const auto removal = burrow::ir::plan(burrow::ir::Action::uninstall,package,nullptr,false);
    check(operations(removal.steps) ==
        "remove-entries,unlink-current,remove-ps-module,remove-environment,unlink-persistence,remove-version,commit-state",
        "the planner did not produce the removal sequence for the foreign package: "+operations(removal.steps));

    // Dependency planning consumes the same normalized identities: the foreign adapter only
    // supplies the declared source string and the installed facts, never a Scoop field.
    std::vector<burrow::ir::InstalledFact> installed(1);
    installed.front().present = true;
    installed.front().name = "runtime";
    installed.front().version = "1.0";
    installed.front().bucket = "vendor";
    burrow::ir::DependencySource source;
    source.buckets = [](const std::string& name) { return name == "runtime" ? std::vector<std::string>{"vendor"} : std::vector<std::string>{}; };
    source.load = [](const std::string& bucket, const std::string& name) {
        burrow::ir::Package dependency;
        dependency.name = name;
        dependency.bucket = bucket;
        dependency.version = "1.0";
        dependency.manifest_digest = std::string(64,'c');
        burrow::ir::Download payload;
        payload.url = "https://vendor.example/runtime.zip";
        payload.filename = "runtime.zip";
        payload.digest.algorithm = burrow::ir::DigestAlgorithm::sha256;
        payload.digest.value = std::string(64,'d');
        payload.digest.declared = true;
        payload.format = burrow::ir::ArchiveFormat::zip;
        dependency.downloads.push_back(payload);
        dependency.declared_hash_count = 1;
        return dependency;
    };
    const auto resolution = burrow::ir::resolve_dependencies(package,installed,true,source);
    check(resolution.diagnostics.empty(),"dependency resolution reported a problem for a satisfied dependency");
    check(resolution.dependencies.size() == 1 && resolution.dependencies.front().installed,
        "the installed dependency was not reused from the normalized installation facts");
    check(resolution.dependencies.front().steps.empty(),
        "a reused dependency carries installation operations it does not need");

    if (failures != 0) {
        std::cerr << failures << " IR reuse checks failed\n";
        return 1;
    }
    std::cout << "IR reuse: a non-Scoop adapter drives the validator, planner and dependency resolver\n";
    return 0;
}
