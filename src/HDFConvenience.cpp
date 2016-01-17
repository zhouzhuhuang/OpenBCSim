/*
Copyright (c) 2015, Sigurd Storve
All rights reserved.

Licensed under the BSD license.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <memory>
#include <string>
#include <stdexcept>
#include <boost/multi_array.hpp>
#include <algorithm>
#include "bcsim_defines.h"
#include "HDFConvenience.hpp"
#include "ScanSequence.hpp"
#include "BeamProfile.hpp"
#include "BCSimConfig.hpp"
#include "vector3.hpp"
#include "SimpleHDF.hpp"
#include "LibBCSim.hpp"

namespace bcsim {

std::string AutodetectScatteresType(const std::string& h5_file) {
    SimpleHDF::SimpleHDF5Reader loader(h5_file);
    bool loaded_fixed = true;
    bool loaded_spline = true;
    try {
        auto data  = loader.readMultiArray<float, 2>("data");
    } catch (...) {
        loaded_fixed = false;
    }
    try {
        auto nodes        = loader.readMultiArray<float, 3>("nodes");
        int spline_degree = loader.readScalar<int>("spline_degree");
        auto knots        = loader.readMultiArray<float, 1>("knot_vector");
    } catch (...) {
        loaded_spline = false;
    }

    // Sanity check
    if (!loaded_fixed && !loaded_spline) {
        throw std::runtime_error("Unable to load fixed or spline data");
    }
    if (loaded_fixed && loaded_spline) {
        throw std::runtime_error("Dataset contained both fixed and spline data");
    }
    std::string type;
    if (loaded_fixed) type = "fixed";
    if (loaded_spline) type = "spline";
    return type;
}

IAlgorithm::s_ptr CreateSimulator(const std::string& config_file,
                                  std::string sim_type) {
    return CreateSimulator(config_file, config_file, config_file, sim_type);
}

IAlgorithm::s_ptr CreateSimulator(const std::string& scatterer_file,
                                  const std::string& scanseq_file,
                                  const std::string& excitation_file,
                                  std::string sim_type) {
    if (sim_type == "") {
        sim_type = AutodetectScatteresType(scatterer_file);
    }
    auto res = Create(sim_type);
    // TODO: read "sound_speed" from HDF5 file instead of
    // using hard-coded value for speed of sound.
    res->set_parameter("sound_speed", "1540.0");
    if (sim_type == "fixed") {
        setFixedScatterersFromHdf(res, scatterer_file);
    } else if (sim_type == "spline") {
        setSplineScatterersFromHdf(res, scatterer_file);
    }
    setScanSequenceFromHdf(res,    scanseq_file);
    setExcitationFromHdf(res,      excitation_file);
    
    return res;
}

void setFixedScatterersFromHdf(IAlgorithm::s_ptr sim, const std::string& h5_file) {
    auto fixed_scatterers = loadFixedScatterersFromHdf(h5_file);
    sim->set_scatterers(fixed_scatterers);
}

void setSplineScatterersFromHdf(IAlgorithm::s_ptr sim, const std::string& h5_file) {
    auto spline_scatterers = loadSplineScatterersFromHdf(h5_file);
    sim->set_scatterers(spline_scatterers);
}

void setScanSequenceFromHdf(IAlgorithm::s_ptr sim, const std::string& h5_file) {
    auto scan_seq = ScanSequence::s_ptr(loadScanSequenceFromHdf(h5_file).release());
    sim->set_scan_sequence(scan_seq);
}

void setExcitationFromHdf(IAlgorithm::s_ptr sim, const std::string& h5_file) {
    auto excitation = loadExcitationFromHdf(h5_file);
    sim->set_excitation(excitation);
}

void setBeamProfileFromHdf(IAlgorithm::s_ptr sim, const std::string& h5_file) {
    auto lut_beamprofile = loadBeamProfileFromHdf(h5_file);
    sim->set_beam_profile(lut_beamprofile);
}

Scatterers::s_ptr loadFixedScatterersFromHdf(const std::string& h5_file) {
    SimpleHDF::SimpleHDF5Reader loader(h5_file);
    auto res = new FixedScatterers;
    try {
        auto data = loader.readMultiArray<float, 2>("data");
        auto data_shape = data.shape();
        size_t num_scatterers = data_shape[0];
        size_t num_components = data_shape[1];
        if (num_components != 4) {
            throw std::runtime_error("Second dimension must have length four");
        }
        res->scatterers.resize(num_scatterers);
        for (size_t i = 0; i < num_scatterers; i++) {
            PointScatterer scatterer;
            scatterer.pos       = vector3(data[i][0], data[i][1], data[i][2]);
            scatterer.amplitude = data[i][3];
            res->scatterers[i]  = scatterer;
        }
    } catch (...) {
        throw std::runtime_error("Failed to set fixed scatterers dataset");
    }
    return Scatterers::s_ptr(res);
}

Scatterers::s_ptr loadSplineScatterersFromHdf(const std::string& h5_file) {
    SimpleHDF::SimpleHDF5Reader loader(h5_file);
    auto res = new SplineScatterers;
    try {
        auto nodes         = loader.readMultiArray<float, 3>("nodes");
        int spline_degree  = loader.readScalar<int>("spline_degree");
        auto knot_vector   = loader.readStdVector<float>("knot_vector");

        res->spline_degree = spline_degree;
        res->knot_vector = knot_vector;

        // Copy nodes
        auto shape = nodes.shape();
        size_t num_scatterers = shape[0];
        size_t num_cs = shape[1];
        size_t num_comp = shape[2];
        if (num_comp != 4) {
            throw std::runtime_error("SplineScatterer illegal number of components (should be 4)");
        }
        res->nodes.resize(num_scatterers);
        for (size_t scatterer_no = 0; scatterer_no < num_scatterers; scatterer_no++) {
            std::vector<PointScatterer> control_points;
            for (size_t cs_no = 0; cs_no < num_cs; cs_no++) {
                PointScatterer scatterer;
                scatterer.pos = vector3(nodes[scatterer_no][cs_no][0], nodes[scatterer_no][cs_no][1], nodes[scatterer_no][cs_no][2]);
                scatterer.amplitude = nodes[scatterer_no][cs_no][3];
                control_points.push_back(scatterer);
            }
            res->nodes[scatterer_no] = control_points;
        }
    } catch (...) {
        throw std::runtime_error("Failed to configure spline scatterer dataset");
    }
    return Scatterers::s_ptr(res);
}

ScanSequence::u_ptr loadScanSequenceFromHdf(const std::string& h5_file) {
    SimpleHDF::SimpleHDF5Reader reader(h5_file);
    auto directions   = reader.readMultiArray<float, 2>("directions");
    auto origins      = reader.readMultiArray<float, 2>("origins");
    auto line_length  = reader.readScalar<float       >("lengths");
    auto lateral_dirs = reader.readMultiArray<float, 2>("lateral_dirs");
    auto timestamps   = reader.readMultiArray<float, 1>("timestamps");
    int num_directions = directions.shape()[0];
    if (num_directions != origins.shape()[0] || num_directions != timestamps.shape()[0]) {
        throw std::runtime_error("ScanSequence::size error");
    }
    auto scan_seq = new ScanSequence(line_length);
    // Create all scan lines
    for (int row = 0; row < num_directions; row++) {
        const vector3 direction(directions[row][0], directions[row][1], directions[row][2]);
        const vector3 origin(origins[row][0], origins[row][1], origins[row][2]);
        const vector3 lateral_dir(lateral_dirs[row][0], lateral_dirs[row][1], lateral_dirs[row][2]);
        const float   timestamp = timestamps[row];
        const Scanline s(origin, direction, lateral_dir, timestamp);
        scan_seq->add_scanline(s);
    }
    return ScanSequence::u_ptr(scan_seq);
}

ExcitationSignal loadExcitationFromHdf(const std::string& h5_file) {
    SimpleHDF::SimpleHDF5Reader reader(h5_file);
    ExcitationSignal excitation;
    excitation.center_index       = reader.readScalar<int>     ("center_index");
    excitation.sampling_frequency = reader.readScalar<float>   ("sampling_frequency");
    excitation.samples            = reader.readStdVector<float>("samples");
    throw std::runtime_error("TODO: Must read and set demodulation frequency");
    return excitation;
}

IBeamProfile::s_ptr loadBeamProfileFromHdf(const std::string& h5_file) {
    SimpleHDF::SimpleHDF5Reader reader(h5_file);
    auto lut_samples = reader.readMultiArray<float, 3>("beam_profile");
    auto rad_extent  = reader.readStdVector<float>("rad_extent");
    auto lat_extent  = reader.readStdVector<float>("lat_extent");
    auto ele_extent  = reader.readStdVector<float>("ele_extent");
    auto lut_samples_shape = lut_samples.shape();
    
    // TODO: Consider allowing 2D samples array with the interpretation of being axial symmetric
    if (lut_samples.num_dimensions() != 3) {
        throw std::runtime_error("beam_profile data must be a 3D array");
    }
    
    // parse rad, lat, and elev. extents
    if (rad_extent.size() != 2) {
        throw std::runtime_error("rad_extent must contain two elements: [min, max]");
    }
    const auto r_min = rad_extent[0];
    const auto r_max = rad_extent[1];
    if (lat_extent.size() != 2) {
        throw std::runtime_error("lat_extent must contain two elements: [min, max]");
    }
    const auto l_min = lat_extent[0];
    const auto l_max = lat_extent[1];
    if (ele_extent.size() != 2) {
        throw std::runtime_error("ele_extent must contain two elements: [min, max]");
    }
    const auto e_min = ele_extent[0];
    const auto e_max = ele_extent[1];

    // corresponding number of samples
    const auto num_rad_samples = lut_samples_shape[0];
    const auto num_lat_samples = lut_samples_shape[1];
    const auto num_ele_samples = lut_samples_shape[2];
    
    auto lut_profile = new LUTBeamProfile(num_rad_samples, num_lat_samples, num_ele_samples,
                                          Interval(r_min, r_max), Interval(l_min, l_max), Interval(e_min, e_max));


    // Normalize so that maximum is one
    const auto max_val = *std::max_element(lut_samples.origin(), lut_samples.origin()+lut_samples.num_elements());

    // Copy data. TODO: require matching hdf5 layout so that copying can be eliminated
    // dim0: radial index
    // dim1: lateral index
    // dim2: elevational index
    for (size_t rad_idx = 0; rad_idx < num_rad_samples; rad_idx++) {
        for (size_t lat_idx = 0; lat_idx < num_lat_samples; lat_idx++) {
            for (int ele_idx = 0; ele_idx < num_ele_samples; ele_idx++) {
                lut_profile->setDiscreteSample(rad_idx, lat_idx, ele_idx, lut_samples[rad_idx][lat_idx][ele_idx]/max_val);       
            }
        }
    }
    return IBeamProfile::s_ptr(lut_profile);
}

}   // namespace

