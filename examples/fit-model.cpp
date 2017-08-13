/*
 * eos - A 3D Morphable Model fitting library written in modern C++11/14.
 *
 * File: examples/fit-model.cpp
 *
 * Copyright 2016 Patrik Huber
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "eos/core/Landmark.hpp"
#include "eos/core/LandmarkMapper.hpp"
#include "eos/core/Image.hpp"
#include "eos/core/Image_opencv_interop.hpp"
#include "eos/morphablemodel/MorphableModel.hpp"
#include "eos/morphablemodel/Blendshape.hpp"
#include "eos/fitting/fitting.hpp"
#include "eos/render/texture_extraction.hpp"

#include "Eigen/Core"

#include "cxxopts.hpp"

#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"

#include <vector>
#include <iostream>
#include <fstream>
#include <string>
#include <optional>
#include <filesystem>

using namespace eos;
namespace fs = std::experimental::filesystem;
using eos::core::Landmark;
using eos::core::LandmarkCollection;
using cv::Mat;
using std::cout;
using std::endl;
using std::vector;
using std::string;

/**
 * Reads an ibug .pts landmark file and returns an ordered vector with
 * the 68 2D landmark coordinates.
 *
 * @param[in] filename Path to a .pts file.
 * @return An ordered vector with the 68 ibug landmarks.
 */
LandmarkCollection<Eigen::Vector2f> read_pts_landmarks(std::string filename)
{
	using std::getline;
	using Eigen::Vector2f;
	using std::string;
	LandmarkCollection<Vector2f> landmarks;
	landmarks.reserve(68);

	std::ifstream file(filename);
	if (!file.is_open()) {
		throw std::runtime_error(string("Could not open landmark file: " + filename));
	}

	string line;
	// Skip the first 3 lines, they're header lines:
	getline(file, line); // 'version: 1'
	getline(file, line); // 'n_points : 68'
	getline(file, line); // '{'

	int ibugId = 1;
	while (getline(file, line))
	{
		if (line == "}") { // end of the file
			break;
		}
		std::stringstream lineStream(line);

		Landmark<Vector2f> landmark;
		landmark.name = std::to_string(ibugId);
		if (!(lineStream >> landmark.coordinates[0] >> landmark.coordinates[1])) {
			throw std::runtime_error(string("Landmark format error while parsing the line: " + line));
		}
		// From the iBug website:
		// "Please note that the re-annotated data for this challenge are saved in the Matlab convention of 1 being
		// the first index, i.e. the coordinates of the top left pixel in an image are x=1, y=1."
		// ==> So we shift every point by 1:
		landmark.coordinates[0] -= 1.0f;
		landmark.coordinates[1] -= 1.0f;
		landmarks.emplace_back(landmark);
		++ibugId;
	}
	return landmarks;
};

/**
 * Draws the given mesh as wireframe into the image.
 *
 * It does backface culling, i.e. draws only vertices in CCW order.
 *
 * @param[in] image An image to draw into.
 * @param[in] mesh The mesh to draw.
 * @param[in] modelview Model-view matrix to draw the mesh.
 * @param[in] projection Projection matrix to draw the mesh.
 * @param[in] viewport Viewport to draw the mesh.
 * @param[in] colour Colour of the mesh to be drawn.
 */
void draw_wireframe(cv::Mat image, const core::Mesh& mesh, glm::mat4x4 modelview, glm::mat4x4 projection, glm::vec4 viewport, cv::Scalar colour = cv::Scalar(0, 255, 0, 255))
{
	for (const auto& triangle : mesh.tvi)
	{
		const auto p1 = glm::project({ mesh.vertices[triangle[0]][0], mesh.vertices[triangle[0]][1], mesh.vertices[triangle[0]][2] }, modelview, projection, viewport);
		const auto p2 = glm::project({ mesh.vertices[triangle[1]][0], mesh.vertices[triangle[1]][1], mesh.vertices[triangle[1]][2] }, modelview, projection, viewport);
		const auto p3 = glm::project({ mesh.vertices[triangle[2]][0], mesh.vertices[triangle[2]][1], mesh.vertices[triangle[2]][2] }, modelview, projection, viewport);
		if (render::detail::are_vertices_ccw_in_screen_space(glm::vec2(p1), glm::vec2(p2), glm::vec2(p3)))
		{
			cv::line(image, cv::Point(p1.x, p1.y), cv::Point(p2.x, p2.y), colour);
			cv::line(image, cv::Point(p2.x, p2.y), cv::Point(p3.x, p3.y), colour);
			cv::line(image, cv::Point(p3.x, p3.y), cv::Point(p1.x, p1.y), colour);
		}
	}
};

/**
 * This app demonstrates estimation of the camera and fitting of the shape
 * model of a 3D Morphable Model from an ibug LFPW image with its landmarks.
 * In addition to fit-model-simple, this example uses blendshapes, contour-
 * fitting, and can iterate the fitting.
 *
 * 68 ibug landmarks are loaded from the .pts file and converted
 * to vertex indices using the LandmarkMapper.
 */
int main(int argc, char *argv[])
{
	string modelfile, isomapfile, imagefile, landmarksfile, mappingsfile, contourfile, edgetopologyfile, blendshapesfile, outputbasename;
	try {
		cxxopts::Options options("fit-model", "Fits a 3D Morphable Model to an image with landmarks.");
		options.add_options()
			("h,help", "display the help message")
			("m,model", "a Morphable Model stored as cereal BinaryArchive",
				cxxopts::value<string>(modelfile)->default_value("../share/sfm_shape_3448.bin"))
			("i,image", "an input image",
				cxxopts::value<string>(imagefile)->default_value("data/image_0010.png"))
			("l,landmarks", "2D landmarks for the image, in ibug .pts format",
				cxxopts::value<string>(landmarksfile)->default_value("data/image_0010.pts"))
			("p,mapping", "landmark identifier to model vertex number mapping",
				cxxopts::value<string>(mappingsfile)->default_value("../share/ibug_to_sfm.txt"))
			("c,model-contour", "file with model contour indices",
				cxxopts::value<string>(contourfile)->default_value("../share/model_contours.json"))
			("e,edge-topology", "file with model's precomputed edge topology",
				cxxopts::value<string>(edgetopologyfile)->default_value("../share/sfm_3448_edge_topology.json"))
			("b,blendshapes", "file with blendshapes",
				cxxopts::value<string>(blendshapesfile)->default_value("../share/expression_blendshapes_3448.bin"))
			("o,output", "basename for the output rendering and obj files",
				cxxopts::value<string>(outputbasename)->default_value("out"))
			;
		options.parse(argc, argv);
		if (options.count("help"))
		{
			cout << options.help() << endl;
			return EXIT_SUCCESS;
		}
	}
	catch (const cxxopts::OptionException& e) {
		cout << "Error while parsing command-line arguments: " << e.what() << endl;
		cout << "Use --help to display a list of options." << endl;
		return EXIT_FAILURE;
	}

	// Load the image, landmarks, LandmarkMapper and the Morphable Model:
	Mat image = cv::imread(imagefile);
	LandmarkCollection<Eigen::Vector2f> landmarks;
	try {
		landmarks = read_pts_landmarks(landmarksfile);
	}
	catch (const std::runtime_error& e) {
		cout << "Error reading the landmarks: " << e.what() << endl;
		return EXIT_FAILURE;
	}
	morphablemodel::MorphableModel morphable_model;
	try {
		morphable_model = morphablemodel::load_model(modelfile);
	}
	catch (const std::runtime_error& e) {
		cout << "Error loading the Morphable Model: " << e.what() << endl;
		return EXIT_FAILURE;
	}
	// The landmark mapper is used to map ibug landmark identifiers to vertex ids:
	core::LandmarkMapper landmark_mapper = mappingsfile.empty() ? core::LandmarkMapper() : core::LandmarkMapper(mappingsfile);

	// The expression blendshapes:
	vector<morphablemodel::Blendshape> blendshapes = morphablemodel::load_blendshapes(blendshapesfile);

	// These two are used to fit the front-facing contour to the ibug contour landmarks:
	fitting::ModelContour model_contour = contourfile.empty() ? fitting::ModelContour() : fitting::ModelContour::load(contourfile);
	fitting::ContourLandmarks ibug_contour = fitting::ContourLandmarks::load(mappingsfile);

	// The edge topology is used to speed up computation of the occluding face contour fitting:
	morphablemodel::EdgeTopology edge_topology = morphablemodel::load_edge_topology(edgetopologyfile);

	// Draw the loaded landmarks:
	Mat outimg = image.clone();
	for (auto&& lm : landmarks) {
		cv::rectangle(outimg, cv::Point2f(lm.coordinates[0] - 2.0f, lm.coordinates[1] - 2.0f), cv::Point2f(lm.coordinates[0] + 2.0f, lm.coordinates[1] + 2.0f), { 255, 0, 0 });
	}

	// Fit the model, get back a mesh and the pose:
	core::Mesh mesh;
	fitting::RenderingParameters rendering_params;
	std::tie(mesh, rendering_params) = fitting::fit_shape_and_pose(morphable_model, blendshapes, landmarks, landmark_mapper, image.cols, image.rows, edge_topology, ibug_contour, model_contour, 5, std::nullopt, 30.0f);

	// The 3D head pose can be recovered as follows:
	float yaw_angle = glm::degrees(glm::yaw(rendering_params.get_rotation()));
	// and similarly for pitch and roll.

	// Extract the texture from the image using given mesh and camera parameters:
	Eigen::Matrix<float, 3, 4> affine_from_ortho = fitting::get_3x4_affine_camera_matrix(rendering_params, image.cols, image.rows);
	core::Image4u isomap = render::extract_texture(mesh, affine_from_ortho, core::from_mat(image), true);
	Mat isomap_from_Image = core::to_mat(isomap);

	// Draw the fitted mesh as wireframe, and save the image:
	draw_wireframe(outimg, mesh, rendering_params.get_modelview(), rendering_params.get_projection(), fitting::get_opencv_viewport(image.cols, image.rows));
	fs::path outputfile = outputbasename + ".png";
	cv::imwrite(outputfile.string(), outimg);

	// Save the mesh as textured obj:
	outputfile.replace_extension(".obj");
	core::write_textured_obj(mesh, outputfile.string());

	// And save the isomap:
	outputfile.replace_extension(".isomap.png");
	cv::imwrite(outputfile.string(), core::to_mat(isomap));

	cout << "Finished fitting and wrote result mesh and isomap to files with basename " << outputfile.stem().stem() << "." << endl;

	return EXIT_SUCCESS;
}
