#include "PluginHooks.hpp"

#include "PluginManager.hpp"
#include "PluginResolver.hpp"
#include "PythonInterpreter.hpp"
#include "PythonPluginInterface.hpp"
#include "pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/LifecycleEvents.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>
#include <boost/nowide/cstdlib.hpp>
#include "libslic3r_version.h"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/log/trivial.hpp>

#include <exception>
#include <cstddef>
#include <memory>
#include "slic3r/plugin/PluginDescriptor.hpp"
#include <string>
#include <stdexcept>
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::plugin_hooks {
namespace {

// Manifest resolver: turns the bare capability name a preset stores into the full
// "name;uuid;capability" reference the dispatchers consume (see
// ConfigBase::collect_plugin_manifest / update_plugin_manifest).
void install_capability_resolver()
{
    ConfigBase::set_resolve_capability_fn([](const std::string& cap_name, const std::string& cap_type) {
        const auto ref = PluginManager::instance().resolve_capability_ref(cap_name, plugin_capability_type_from_string(cap_type));
        return ref ? create_full_ref(*ref) : std::string();
    });
}

// Global libslic3r-side seam (Slic3r::fire_lifecycle_event, in libslic3r/LifecycleEvents.hpp):
// broadcasts to every loaded, enabled capability regardless of type, unlike the SlicingPipeline
// hook below which only targets picker-selected SlicingPipeline capabilities.
void install_lifecycle_event_hook()
{
    set_lifecycle_hook_fn([](LifecycleEvent event, const LifecycleEventContext& ctx) {
        PluginManager::instance().dispatch_lifecycle_event(event, ctx);
    });
}

// Print::process() fires this hook at each pipeline seam on the slicing worker
// thread; here we run the picker-selected SlicingPipeline capabilities. Per
// capability we acquire the GIL, honor cancellation, and convert a plugin
// failure into a (non-critical) SlicingError so it surfaces as a slicing-error
// notification rather than the fatal-crash dialog.
void install_slicing_pipeline_hook()
{
    Print::set_slicing_pipeline_hook_fn(
        [](Print& print, const PrintObject* object, SlicingPipelineStepPlugin step) {
            const auto* caps  = print.config().option<ConfigOptionStrings>("slicing_pipeline_plugin");
            // `plugins` is a dynamic-only manifest key (not a static PrintConfig member), so it
            // must be read from the full/dynamic config -- reading it off print.config() (the
            // static PrintConfig) always yields nullptr and skips every capability. Mirrors the
            // post-process path (PostProcessor.cpp, via BackgroundSlicingProcess::full_print_config()).
            const auto* plugs = print.full_print_config().option<ConfigOptionStrings>("plugins");
            if (caps == nullptr || caps->values.empty())
                return;

            execute_capabilities_from_refs<SlicingPipelinePluginCapability>(
                *caps, plugs, PluginCapabilityType::SlicingPipeline,
                [&](std::shared_ptr<SlicingPipelinePluginCapability> cap, const PluginCapabilityRef& ref) {
                    const std::string plugin_key = ref.uuid.empty() ? ref.name : ref.uuid;
                    ExecutionResult r;
                    try {
                        // GIL is acquired per capability (not once for the whole dispatch) so it
                        // is released between capabilities.
                        PythonGILState gil;
                        if (!gil)
                            throw std::runtime_error("Python interpreter is shutting down");
                        // throw_if_canceled() is protected on PrintBase; canceled() is the public
                        // equivalent check (same cancel flag), so honor cancellation via it.
                        if (print.canceled())
                            throw CanceledException();
                        SlicingPipelineContext ctx;
                        ctx.orca_version = SoftFever_VERSION;
                        ctx.step   = step;
                        ctx.print  = &print;
                        ctx.object = object;
                        // Slicing hooks are synchronous by design and never await: the context
                        // borrows Print/PrintObject that are only valid during this call, and the
                        // UI thread may itself be blocked waiting on this slicing worker. An async
                        // override is rejected here rather than scheduled -- cap->execute() casts
                        // its Python result to ExecutionResult and a coroutine object cannot be
                        // cast, so it surfaces as the SlicingError below.
                        r = cap->execute(ctx);
                    } catch (const CanceledException&) {
                        throw; // cancellation must reach process(), never become a slicing error
                    } catch (const std::exception& ex) {
                        // A Python raise reaches here as pybind11::error_already_set; surface it as a
                        // (non-critical) slicing error instead of a crash.
                        throw SlicingError(GUI::format(_u8L("Slicing pipeline plugin '%1%' error: %2%"),
                                                       ref.capability_name, ex.what()));
                    }
                    if (r.status == PluginResult::FatalError)
                        throw SlicingError(GUI::format(_u8L("Slicing pipeline plugin '%1%' error: %2%"),
                                                       ref.capability_name, r.message));
                    // log a non-empty success/skipped message instead of dropping it. This is
                    // log-only by design: every pipeline hook fires AFTER set_done() (see Print.cpp),
                    // so the Print-level m_step_active is -1 here. Calling active_step_add_warning()
                    // would then index m_state[-1] (out-of-bounds; the guarding assert is compiled
                    // out in Release), so it must NOT be called from a pipeline hook.
                    if (!r.message.empty()) {
                        static const char* const kStepNames[] = {
                            "posSlice", "posPerimeters", "posEstimateCurledExtrusions", "posPrepareInfill", "posInfill",
                            "posIroning", "posContouring", "posSupportMaterial", "posDetectOverhangsForLift",
                            "posSimplifyPath", "psWipeTower", "psSkirtBrim", "psGCodePostProcess"
                        }; // order must match SlicingPipelineStepPlugin
                        const char* step_name = static_cast<size_t>(step) < sizeof(kStepNames) / sizeof(kStepNames[0])
                                                    ? kStepNames[static_cast<int>(step)] : "Unknown";
                        BOOST_LOG_TRIVIAL(info) << "Slicing pipeline plugin '" << ref.capability_name
                                                << "' [" << step_name << "]: " << r.message;
                    }
                });
        });
}

// Flash Studio runs post-processing in libslic3r (GCode/PostProcessor.cpp); this installs the
// slicing-pipeline plugins' G-code step there (Step.psGCodePostProcess), as OrcaSlicer 2.5.2's
// GUI PostProcessor does: each plugin edits the working copy of the exported G-code in place.
void install_post_process_hook()
{
    set_post_process_plugins_fn([](const ConfigOptionStrings& capabilities, const ConfigOptionStrings* plugins,
                                   const std::string& gcode_path, const std::string& host, const std::string& output_name,
                                   const DynamicPrintConfig& config) {
        boost::nowide::setenv("SLIC3R_PP_OUTPUT_NAME", output_name.c_str(), 1);
        const boost::filesystem::path gcode_file(gcode_path);
        execute_capabilities_from_refs<SlicingPipelinePluginCapability>(
            capabilities, plugins, PluginCapabilityType::SlicingPipeline,
            [&](std::shared_ptr<SlicingPipelinePluginCapability> cap, const PluginCapabilityRef& ref) {
                SlicingPipelineContext ctx;
                ctx.orca_version = SoftFever_VERSION;
                ctx.step         = SlicingPipelineStepPlugin::psGCodePostProcess;
                ctx.gcode_path   = gcode_path;
                ctx.host         = host;
                ctx.output_name  = output_name;
                ctx.full_config  = &config;

                ExecutionResult r;
                try {
                    PythonGILState gil;
                    if (!gil)
                        throw std::runtime_error("Python interpreter is shutting down");
                    r = cap->execute(ctx);
                } catch (const std::exception& ex) {
                    const std::string msg = GUI::format(_u8L("Post-processing plugin %1% raised an exception.\nError: %2%"),
                                                        ref.capability_name, ex.what());
                    BOOST_LOG_TRIVIAL(error) << msg;
                    throw Slic3r::RuntimeError(msg);
                }
                if (r.status == PluginResult::RecoverableError || r.status == PluginResult::FatalError) {
                    const std::string msg = GUI::format(_u8L("Post-processing plugin %1% failed.\nError: %2%"),
                                                        ref.capability_name, r.message);
                    BOOST_LOG_TRIVIAL(error) << msg;
                    throw Slic3r::RuntimeError(msg);
                }
                if (!r.message.empty())
                    BOOST_LOG_TRIVIAL(info) << "Post-processing plugin " << ref.capability_name << ": " << r.message;
                if (!boost::filesystem::exists(gcode_file))
                    throw Slic3r::RuntimeError(GUI::format(
                        _u8L("Post-processing plugin %1% failed.\n\nThe post-processing plugin is expected to modify the G-code file %2% in place, but the G-code file was deleted.\nPlease check the plugin implementation.\n"),
                        ref.capability_name, gcode_path));
                BOOST_LOG_TRIVIAL(info) << "Post-processing plugin " << ref.capability_name << " completed successfully";
            });
    });
}

} // namespace

void install()
{
    install_capability_resolver();
    install_slicing_pipeline_hook();
    install_lifecycle_event_hook();
    install_post_process_hook();
}

void uninstall()
{
    ConfigBase::set_resolve_capability_fn(nullptr);
    Print::set_slicing_pipeline_hook_fn(nullptr);
    set_post_process_plugins_fn(nullptr);
    set_lifecycle_hook_fn(nullptr);
}

} // namespace Slic3r::plugin_hooks
