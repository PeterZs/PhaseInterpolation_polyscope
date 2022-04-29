#include "polyscope/polyscope.h"
#include "polyscope/pick.h"

#include <igl/invert_diag.h>
#include <igl/per_vertex_normals.h>
#include <igl/readOBJ.h>
#include <igl/writeOBJ.h>
#include <igl/boundary_loop.h>
#include <igl/doublearea.h>
#include <igl/loop.h>
#include <igl/file_dialog_open.h>
#include <igl/file_dialog_save.h>
#include <igl/cotmatrix_entries.h>
#include "polyscope/messages.h"
#include "polyscope/point_cloud.h"
#include "polyscope/surface_mesh.h"
#include "polyscope/view.h"

#include <iostream>
#include <filesystem>
#include <utility>

#include "../../include/CommonTools.h"
#include "../../include/MeshLib/MeshUpsampling.h"
#include "../../include/Visualization/PaintGeometry.h"
#include "../../include/Optimization/NewtonDescent.h"
#include "../../include/IntrinsicFormula/InterpolateZvals.h"
#include "../../include/IntrinsicFormula/KnoppelStripePatternEdgeOmega.h"
#include "../../include/WrinkleFieldsEditor.h"
#include "../../include/SpherigonSmoothing.h"
#include "../../dep/SecStencils/types.h"
#include "../../dep/SecStencils/utils.h"

#include "../../include/json.hpp"
#include "../../include/ComplexLoop.h"
#include "../../include/ComplexLoopNew.h"
#include "../../include/LoadSaveIO.h"
#include "../../include/SecMeshParsing.h"
#include "../../include/MeshLib/RegionEdition.h"

Eigen::MatrixXd triV, loopTriV, NV, newN;
Eigen::MatrixXi triF, loopTriF, NF;

MeshConnectivity triMesh;
Mesh secMesh, subSecMesh;
std::vector<std::pair<int, Eigen::Vector3d>> bary;

Eigen::VectorXd initAmp;
Eigen::VectorXd initOmega;
std::vector<std::complex<double>> initZvals;

Eigen::VectorXd loopedAmp, loopedOmega, loopedPhase;
std::vector<std::complex<double>> loopedZvals;
std::vector<int> badfaces;

Eigen::MatrixXd faceOmega;
Eigen::MatrixXd loopedFaceOmega;

Eigen::MatrixXd dataV;
Eigen::MatrixXi dataF;
Eigen::MatrixXd dataVec;
Eigen::MatrixXd curColor;

int loopLevel = 1;

bool isShowVectorFields = true;
bool isShowWrinkels = true;

PaintGeometry mPaint;

float vecratio = 0.001;
float wrinkleAmpScalingRatio = 1;

std::string workingFolder;
double ampTol = 0.1, curlTol = 1e-4;

// smoothing
int smoothingTimes = 0;
double smoothingRatio = 0.95;

bool isUseTangentCorrection = false;

void updateMagnitudePhase(const Eigen::VectorXd& omega, const std::vector<std::complex<double>>& zvals, Eigen::VectorXd& omegaNew, std::vector<std::complex<double>>& upZvals, Eigen::VectorXd& upsampledAmp, Eigen::VectorXd& upsampledPhase)
{
	NV = triV;
	NF = triF;
	Eigen::MatrixXd VN;
	igl::per_vertex_normals(triV, triF, VN);

	meshUpSampling(triV, triF, NV, NF, loopLevel, NULL, NULL, &bary);
	curvedPNTriangleUpsampling(triV, triF, VN, bary, NV, newN);

	std::vector<Eigen::Vector3d> pos;
	std::vector<std::vector<int>> faces;

	pos.resize(triV.rows());
	for (int i = 0; i < triV.rows(); i++)
	{
		pos[i] = triV.row(i);
	}

	faces.resize(triF.rows());
	for (int i = 0; i < triF.rows(); i++)
	{
		faces[i] = { triF(i, 0), triF(i, 1), triF(i, 2) };
	}

	secMesh.Populate(pos, faces);
	subSecMesh = secMesh;

	Eigen::VectorXd edgeVec = swapEdgeVec(triF, omega, 0);

	Subdivide(secMesh, edgeVec, zvals, omegaNew, upZvals, loopLevel, subSecMesh, ampTol, curlTol, &badfaces);
	

	subSecMesh.GetPos(loopTriV);
	loopTriF.resize(subSecMesh.GetFaceCount(), 3);
	for (int i = 0; i < loopTriF.rows(); i++)
	{
		for (int j = 0; j < 3; j++)
			loopTriF(i, j) = subSecMesh.GetFaceVerts(i)[j];
	}

	upsampledAmp.resize(loopTriV.rows());
	upsampledPhase.resize(loopTriV.rows());
	for(int i = 0; i < upsampledAmp.rows(); i++)
	{
		upsampledAmp(i) = std::abs(upZvals[i]);
		upsampledPhase(i) = std::arg(upZvals[i]);
	}



}


void initialization(const Eigen::MatrixXd& triV, const Eigen::MatrixXi& triF, Eigen::MatrixXd& upsampledTriV, Eigen::MatrixXi& upsampledTriF)
{
	Eigen::SparseMatrix<double> S;
	std::vector<int> facemap;

	triMesh = MeshConnectivity(triF);

	loopTriV = triV;
	loopTriF = triF;

	std::vector<Eigen::Vector3d> pos;
	std::vector<std::vector<int>> faces;

	pos.resize(triV.rows());
	for (int i = 0; i < triV.rows(); i++)
	{
		pos[i] = triV.row(i);
	}

	faces.resize(triF.rows());
	for (int i = 0; i < triF.rows(); i++)
	{
		faces[i] = { triF(i, 0), triF(i, 1), triF(i, 2) };
	}

	secMesh.Populate(pos, faces);
	subSecMesh = secMesh;
}

void updatePaintingItems()
{
	// get interploated amp and phase frames
	std::cout << "compute upsampled phase: " << std::endl;
	updateMagnitudePhase(initOmega, initZvals, loopedOmega, loopedZvals, loopedAmp, loopedPhase);

	std::cout << "compute face vector fields:" << std::endl;
	faceOmega = intrinsicEdgeVec2FaceVec(initOmega, triV, triMesh);

	loopedFaceOmega = edgeVec2FaceVec(subSecMesh, loopedOmega);

	std::cout << "start to update viewer." << std::endl;
}

void updateFieldsInView()
{
	std::cout << "update viewer. " << std::endl;

	polyscope::registerSurfaceMesh("input mesh", triV, triF);
	polyscope::getSurfaceMesh("input mesh")->addVertexScalarQuantity("amp color", initAmp);
	//polyscope::getSurfaceMesh("input mesh")->getQuantity("amp color")->setEnabled(false);

	if (isShowVectorFields)
	{
		polyscope::getSurfaceMesh("input mesh")->addFaceVectorQuantity("vector field", faceOmega);
	}


	polyscope::registerSurfaceMesh("looped amp mesh", loopTriV, loopTriF);
	double shiftx = 1.5 * (triV.col(0).maxCoeff() - triV.col(0).minCoeff());
	double shiftz = 1.5 * (loopTriV.col(2).maxCoeff() - loopTriV.col(2).minCoeff());

	polyscope::getSurfaceMesh("looped amp mesh")->translate(glm::vec3(shiftx, 0, 2 * shiftz));
	//polyscope::getSurfaceMesh("looped amp mesh")->setEnabled(false);

	polyscope::getSurfaceMesh("looped amp mesh")->addVertexScalarQuantity("amp color", loopedAmp);
	//polyscope::getSurfaceMesh("looped amp mesh")->getQuantity("amp color")->setEnabled(true);

	if (isShowVectorFields)
	{
		polyscope::getSurfaceMesh("looped amp mesh")->addFaceVectorQuantity("upsampled vector field", loopedFaceOmega);
	}

	polyscope::registerSurfaceMesh("looped phase mesh", loopTriV, loopTriF);
	polyscope::getSurfaceMesh("looped phase mesh")->translate(glm::vec3(shiftx, 0, shiftz));
	//polyscope::getSurfaceMesh("looped phase mesh")->setEnabled(true);

	mPaint.setNormalization(false);
	Eigen::MatrixXd phiColor = mPaint.paintPhi(loopedPhase);
	polyscope::getSurfaceMesh("looped phase mesh")->addVertexColorQuantity("phase color", phiColor);
	//polyscope::getSurfaceMesh("looped phase mesh")->getQuantity("phase color")->setEnabled(true);

	Eigen::MatrixXd wrinkledTriV;

	std::vector<std::vector<int>> vertNeiEdges, vertNeiFaces;
	buildVertexNeighboringInfo(MeshConnectivity(loopTriF), loopTriV.rows(), vertNeiEdges, vertNeiFaces);

	getWrinkledMesh(loopTriV, loopTriF, loopedZvals, &vertNeiFaces, wrinkledTriV, wrinkleAmpScalingRatio, isUseTangentCorrection);

	Eigen::MatrixXd faceColors(loopTriF.rows(), 3);
	for (int i = 0; i < faceColors.rows(); i++)
	{
		if (badfaces[i])
			faceColors.row(i) << 1, 1, 1;
		else
			faceColors.row(i) << 80 / 255.0, 122 / 255.0, 91 / 255.0;
	}

	igl::writeOBJ(workingFolder + "wrinkledMesh_loop.obj", wrinkledTriV, loopTriF);

	polyscope::registerSurfaceMesh("loop wrinkled mesh", wrinkledTriV, loopTriF);
	polyscope::getSurfaceMesh("loop wrinkled mesh")->setSurfaceColor({ 80 / 255.0, 122 / 255.0, 91 / 255.0 });
	polyscope::getSurfaceMesh("loop wrinkled mesh")->addFaceColorQuantity("wrinkled color", faceColors);
	polyscope::getSurfaceMesh("loop wrinkled mesh")->translate(glm::vec3(shiftx, 0, 0));
	polyscope::getSurfaceMesh("loop wrinkled mesh")->setEnabled(true);

	Eigen::VectorXd edgeVecSwapped;
	std::vector<std::vector<int>> edge2verts;
	for(int i = 0; i < subSecMesh.GetEdgeCount(); i++)
	{
		edge2verts.push_back(subSecMesh.GetEdgeVerts(i));
	}
	edgeVecSwapped = swapEdgeVec(edge2verts, loopedOmega, 1);
	Eigen::VectorXd edgeArea = getEdgeArea(loopTriV, loopTriF);
	Eigen::VectorXd vertArea = getVertArea(loopTriV, loopTriF);

	std::vector<std::complex<double>> tmpZvals;
	Eigen::VectorXd edgeVec = swapEdgeVec(triF, initOmega, 0);

	Eigen::VectorXd loopedOmegaNew = loopedOmega, loopedAmpNew = loopedAmp, loopedPhaseNew = loopedPhase;

	Eigen::MatrixXd loopTriVNew;
	Eigen::MatrixXi loopTriFNew;

	complexLoopSubdivision(secMesh, edgeVec, initZvals, loopedOmegaNew, tmpZvals, loopLevel, subSecMesh);
	parseSecMesh(subSecMesh, loopTriVNew, loopTriFNew);

	Eigen::MatrixXd wrinkledTrivNew = loopTriVNew;

	Eigen::VectorXd loopedReal = loopedAmp;

	for (int i = 0; i < loopTriVNew.rows(); i++)
	{
		loopedPhaseNew(i) = std::arg(tmpZvals[i]);
		loopedAmpNew(i) = std::abs(tmpZvals[i]);
		loopedReal(i) = tmpZvals[i].real();
	}

	getWrinkledMesh(loopTriVNew, loopTriFNew, tmpZvals, &vertNeiFaces, wrinkledTrivNew, wrinkleAmpScalingRatio, isUseTangentCorrection);

	igl::writeOBJ(workingFolder + "wrinkledMesh_loop_Zuenko.obj", wrinkledTrivNew, loopTriFNew);

	polyscope::registerSurfaceMesh("Loop-Zuenko amp mesh", loopTriVNew, loopTriFNew);
	polyscope::getSurfaceMesh("Loop-Zuenko amp mesh")->translate(glm::vec3(2 * shiftx, 0, 2 * shiftz));
	//polyscope::getSurfaceMesh("Loop-Zuenko amp mesh")->setEnabled(false);

	polyscope::getSurfaceMesh("Loop-Zuenko amp mesh")->addVertexScalarQuantity("amp color", loopedAmpNew);
	//polyscope::getSurfaceMesh("Loop-Zuenko amp mesh")->getQuantity("amp color")->setEnabled(true);

	if (isShowVectorFields)
	{
		Eigen::MatrixXd loopedFaceOmegaNew = edgeVec2FaceVec(subSecMesh, loopedOmegaNew);
		polyscope::getSurfaceMesh("Loop-Zuenko amp mesh")->addFaceVectorQuantity("upsampled vector field", loopedFaceOmegaNew);
	}

	polyscope::registerSurfaceMesh("Loop-Zuenko phase mesh", loopTriVNew, loopTriFNew);
	polyscope::getSurfaceMesh("Loop-Zuenko phase mesh")->translate(glm::vec3(2 * shiftx, 0, shiftz));
	//polyscope::getSurfaceMesh("Loop-Zuenko phase mesh")->setEnabled(true);

	mPaint.setNormalization(false);
	phiColor = mPaint.paintPhi(loopedPhaseNew);
	polyscope::getSurfaceMesh("Loop-Zuenko phase mesh")->addVertexColorQuantity("phase color", phiColor);
	//polyscope::getSurfaceMesh("Loop-Zuenko phase mesh")->getQuantity("phase color")->setEnabled(true);
	polyscope::getSurfaceMesh("Loop-Zuenko phase mesh")->addVertexScalarQuantity("real-z color", loopedReal);

	polyscope::registerSurfaceMesh("Loop-Zuenko mesh", wrinkledTrivNew, loopTriFNew);
	polyscope::getSurfaceMesh("Loop-Zuenko mesh")->setSurfaceColor({ 80 / 255.0, 122 / 255.0, 91 / 255.0 });
	polyscope::getSurfaceMesh("Loop-Zuenko mesh")->translate(glm::vec3(2 * shiftx, 0, 0));
	polyscope::getSurfaceMesh("Loop-Zuenko mesh")->setEnabled(true);

	meshUpSampling(triV, triF, NV, NF, loopLevel, NULL, NULL, &bary);
	std::vector<std::complex<double>> zuenkoZvals = IntrinsicFormula::upsamplingZvals(triMesh, initZvals, initOmega, bary);

	Eigen::MatrixXd zuenkoNV;
	buildVertexNeighboringInfo(MeshConnectivity(NF), NV.rows(), vertNeiEdges, vertNeiFaces);

	getWrinkledMesh(NV, NF, zuenkoZvals, &vertNeiFaces, zuenkoNV, wrinkleAmpScalingRatio, isUseTangentCorrection);

	Eigen::VectorXd zuenkoAmp = loopedAmp, zuenkoPhase = loopedPhase;
	Eigen::MatrixXd zuenkoNormal;
	//igl::per_vertex_normals(NV, NF, zuenkoNormal);

	for(int i = 0; i < loopTriV.rows(); i++)
	{
		zuenkoPhase(i) = std::arg(zuenkoZvals[i]);
		zuenkoAmp(i) = std::abs(zuenkoZvals[i]);
		//zuenkoNV.row(i) = NV.row(i) + zuenkoZvals[i].real() * zuenkoNormal.row(i);
	}

	polyscope::registerSurfaceMesh("Zuenko amp mesh", NV, NF);
	polyscope::getSurfaceMesh("Zuenko amp mesh")->translate(glm::vec3(3 * shiftx, 0, 2 * shiftz));
	//polyscope::getSurfaceMesh("Zuenko amp mesh")->setEnabled(false);


	polyscope::getSurfaceMesh("Zuenko amp mesh")->addVertexScalarQuantity("amp color", zuenkoAmp);
	//polyscope::getSurfaceMesh("Zuenko amp mesh")->getQuantity("amp color")->setEnabled(true);

	polyscope::registerSurfaceMesh("Zuenko phase mesh", NV, NF);
	polyscope::getSurfaceMesh("Zuenko phase mesh")->translate(glm::vec3(3 * shiftx, 0, shiftz));
	//polyscope::getSurfaceMesh("Zuenko phase mesh")->setEnabled(true);

	mPaint.setNormalization(false);
	phiColor = mPaint.paintPhi(zuenkoPhase);
	polyscope::getSurfaceMesh("Zuenko phase mesh")->addVertexColorQuantity("phase color", phiColor);
	//polyscope::getSurfaceMesh("Zuenko phase mesh")->getQuantity("phase color")->setEnabled(true);


	laplacianSmoothing(zuenkoNV, NF, zuenkoNV, smoothingRatio, smoothingTimes);

	igl::writeOBJ(workingFolder + "wrinkledMesh_Zuenko.obj", zuenkoNV, NF);

	polyscope::registerSurfaceMesh("Zuenko mesh", zuenkoNV, NF);
	polyscope::getSurfaceMesh("Zuenko mesh")->setSurfaceColor({ 80 / 255.0, 122 / 255.0, 91 / 255.0 });
	polyscope::getSurfaceMesh("Zuenko mesh")->translate(glm::vec3(3 * shiftx, 0, 0));
	polyscope::getSurfaceMesh("Zuenko mesh")->setEnabled(true);
}


bool loadProblem()
{
	std::string loadFileName = igl::file_dialog_open();

	std::cout << "load file in: " << loadFileName << std::endl;
	using json = nlohmann::json;
	std::ifstream inputJson(loadFileName);
	if (!inputJson) {
		std::cerr << "missing json file in " << loadFileName << std::endl;
		return false;
	}

	std::string filePath = loadFileName;
	std::replace(filePath.begin(), filePath.end(), '\\', '/'); // handle the backslash issue for windows
	int id = filePath.rfind("/");
	std::string workingFolder = filePath.substr(0, id + 1);
	std::cout << "working folder: " << workingFolder << std::endl;

	json jval;
	inputJson >> jval;

	std::string meshFile = jval["mesh_name"];
	loopLevel = jval["upsampled_times"];

	meshFile = workingFolder + meshFile;


	igl::readOBJ(meshFile, triV, triF);
	triMesh = MeshConnectivity(triF);
	initialization(triV, triF, loopTriV, loopTriF);

	int nedges = triMesh.nEdges();
	int nverts = triV.rows();

	std::string initAmpPath = jval["init_amp"];
	std::string initOmegaPath = jval["init_omega"];
	std::string initZValsPath = "zvals.txt";
	if (jval.contains(std::string_view{ "init_zvals" }))
	{
		initZValsPath = jval["init_zvals"];
	}

	if (!loadEdgeOmega(workingFolder + initOmegaPath, nedges, initOmega)) {
		std::cout << "missing init edge omega file." << std::endl;
		return false;
	}

	if (!loadVertexZvals(workingFolder + initZValsPath, triV.rows(), initZvals))
	{
		std::cout << "missing init zval file, try to load amp file, and round zvals from amp and omega" << std::endl;
		if (!loadVertexAmp(workingFolder + initAmpPath, triV.rows(), initAmp))
		{
			std::cout << "missing init amp file: " << std::endl;
			return false;
		}

		else
		{
			Eigen::VectorXd edgeArea, vertArea;
			edgeArea = getEdgeArea(triV, triMesh);
			vertArea = getVertArea(triV, triMesh);
			IntrinsicFormula::roundZvalsFromEdgeOmegaVertexMag(triMesh, initOmega, initAmp, edgeArea, vertArea, triV.rows(), initZvals);
		}
	}
	else
	{
		initAmp.setZero(triV.rows());
		for (int i = 0; i < initZvals.size(); i++)
			initAmp(i) = std::abs(initZvals[i]);
	}

	updateMagnitudePhase(initOmega, initZvals, loopedOmega, loopedZvals, loopedAmp, loopedPhase);

	updatePaintingItems();

	return true;
}


void callback() {
	ImGui::PushItemWidth(100);
	float w = ImGui::GetContentRegionAvailWidth();
	float p = ImGui::GetStyle().FramePadding.x;
	if (ImGui::Button("Load", ImVec2(-1, 0)))
	{
		loadProblem();
		updateFieldsInView();
	}

	if (ImGui::InputInt("underline loop level", &loopLevel))
	{
		if (loopLevel < 0)
			loopLevel = 0;
	}

	if (ImGui::CollapsingHeader("Visualization Options", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::Checkbox("is show vector fields", &isShowVectorFields))
		{
			updateFieldsInView();
		}
		if (ImGui::Checkbox("is use tangent correction", &isUseTangentCorrection))
		{
			updateFieldsInView();
		}
		if (ImGui::DragFloat("wrinkle amp scaling ratio", &wrinkleAmpScalingRatio, 0.0005, 0, 1))
		{
			if (wrinkleAmpScalingRatio >= 0)
				updateFieldsInView();
		}

	}

	if (ImGui::CollapsingHeader("smoothing Options", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if(ImGui::InputInt("smoothing times", &smoothingTimes))
		{
			smoothingTimes = smoothingTimes > 0 ? smoothingTimes : 0;
		}
		if(ImGui::InputDouble("smoothing ratio", &smoothingRatio))
		{
			smoothingRatio = smoothingRatio > 0 ? smoothingRatio : 0;
		}
	}

	if (ImGui::CollapsingHeader("Bad faces threshold", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::InputDouble("magnitude tol", &ampTol);
		ImGui::InputDouble("curl free tol", &curlTol);
	}

	if (ImGui::Button("comb fields & updates", ImVec2(-1, 0)))
	{
		// solve for the path from source to target
		updatePaintingItems();
		updateFieldsInView();
	}

	if (ImGui::Button("display informations", ImVec2(-1, 0)))
	{
		std::cout << "initial mesh: " << std::endl;
		std::cout << "\nvertex info: " << std::endl;
		for(int i = 0; i < triV.rows(); i++)
		{
			std::cout << "v_" << i << ": " << triV.row(i) << ", zvals: (" << initZvals[i].real() << ", " << initZvals[i].imag() << ")" << std::endl;
		}

		std::cout << "\nedge info: " << std::endl;
		for(int i = 0; i < triMesh.nEdges(); i++)
		{
			std::cout << "e_" << i << ": " << "edge vertex: " << triMesh.edgeVertex(i, 0) << " " << triMesh.edgeVertex(i, 1) << ", w: " << initOmega(i) << std::endl;
		}

		std::cout << "\nface info: " << std::endl;
		for(int i = 0; i < triF.rows(); i++)
		{
			std::cout << "f_" << i << ": " << triF.row(i) << std::endl;
		}

		std::cout << "\nloop level: " << loopLevel << std::endl;
		std::cout << "\nvertex info: " << std::endl;

		Eigen::VectorXd loopedOmegaNew = loopedOmega, loopedAmpNew = loopedAmp, loopedPhaseNew = loopedPhase;

		Eigen::MatrixXd loopTriVNew;
		Eigen::MatrixXi loopTriFNew;

		Eigen::VectorXd edgeVec = swapEdgeVec(triF, initOmega, 0);
		std::vector<std::complex<double>> tmpZvals;

		complexLoopSubdivision(secMesh, edgeVec, initZvals, loopedOmegaNew, tmpZvals, loopLevel, subSecMesh);
		parseSecMesh(subSecMesh, loopTriVNew, loopTriFNew);

		for(int i = 0; i < loopTriVNew.rows(); i++)
		{
			std::cout << "v_" << i << ": " << loopTriVNew.row(i) << ", theta: " << std::arg(tmpZvals[i]) << ", amp: " << std::abs(tmpZvals[i]) << ", zvals: (" << tmpZvals[i].real() << ", " << tmpZvals[i].imag() << ")" << std::endl;
		}

		std::cout << "\nedge info: " << std::endl;
		for(int i = 0; i < subSecMesh.GetEdgeCount(); i++)
		{
			std::cout << "e_" << i << ": " << "edge vertex: " << subSecMesh.GetEdgeVerts(i)[0] << " " << subSecMesh.GetEdgeVerts(i)[1] << ", w: " << loopedOmegaNew(i) << std::endl;
		}

		std::cout << "\nface info: " << std::endl;
		for(int i = 0; i < loopTriFNew.rows(); i++)
		{
			std::cout << "f_" << i << ": " << loopTriFNew.row(i) << std::endl;
		}
	}

	if (ImGui::Button("output images", ImVec2(-1, 0)))
	{
		std::string curFolder = std::filesystem::current_path().string();
		std::string name = curFolder + "/output.jpg";
		polyscope::screenshot(name);
	}


	ImGui::PopItemWidth();
}


int main(int argc, char** argv)
{
	if (!loadProblem())
	{
		std::cout << "failed to load file." << std::endl;
		return 1;
	}


	// Options
	polyscope::options::autocenterStructures = true;
	polyscope::view::windowWidth = 1024;
	polyscope::view::windowHeight = 1024;

	// Initialize polyscope
	polyscope::init();
	polyscope::view::upDir = polyscope::view::UpDir::ZUp;

	// Add the callback
	polyscope::state::userCallback = callback;

	polyscope::options::groundPlaneHeightFactor = 0.25; // adjust the plane height

	updateFieldsInView();
	// Show the gui
	polyscope::show();


	return 0;
}