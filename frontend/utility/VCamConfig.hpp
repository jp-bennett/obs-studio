#pragma once

#include <obs.h>

#include <string>
#include <vector>

constexpr const char *VIRTUAL_CAM_ID = "virtualcam_output";
constexpr const char *PIPEWIRE_VIRTUAL_CAM_ID = "pipewire_virtualcam_output";

// Outputs that can back the virtual camera, in order of preference
constexpr const char *VIRTUAL_CAM_OUTPUT_IDS[] = {VIRTUAL_CAM_ID, PIPEWIRE_VIRTUAL_CAM_ID};

enum VCamOutputType {
	Invalid,
	SceneOutput,
	SourceOutput,
	ProgramView,
	PreviewOutput,
};

// Kept for config upgrade
enum VCamInternalType {
	Default,
	Preview,
};

struct VCamConfig {
	VCamOutputType type = VCamOutputType::ProgramView;
	std::string scene;
	std::string source;

	// Output implementation used for the virtual camera. Stored in the
	// user config rather than the scene collection, as it depends on
	// what is installed on the machine.
	std::string outputId;
};

inline std::vector<std::string> GetAvailableVirtualCamOutputIds()
{
	std::vector<std::string> ids;

	for (const char *id : VIRTUAL_CAM_OUTPUT_IDS) {
		if ((obs_get_output_flags(id) & OBS_OUTPUT_VIDEO) != 0) {
			ids.emplace_back(id);
		}
	}

	return ids;
}
