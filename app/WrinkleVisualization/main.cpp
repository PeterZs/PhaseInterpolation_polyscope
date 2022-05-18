#include "polyscope/polyscope.h"
#include "polyscope/pick.h"

#include <igl/invert_diag.h>
#include <igl/per_vertex_normals.h>
#include <igl/readOBJ.h>
#include <igl/writeOBJ.h>
#include <igl/boundary_loop.h>
#include <igl/doublearea.h>
#include <igl/loop.h>
#include <igl/decimate.h>
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
#include "../../include/ComplexLoop/ComplexLoop.h"
#include "../../include/ComplexLoop/ComplexLoopAmpPhase.h"
#include "../../include/ComplexLoop/ComplexLoopAmpPhaseEdgeJump.h"
#include "../../include/ComplexLoop/ComplexLoopReIm.h"
#include "../../include/ComplexLoop/ComplexLoopZuenko.h"

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

Eigen::MatrixXd faceOmega;
Eigen::MatrixXd loopedFaceOmega;

Eigen::MatrixXd dataV;
Eigen::MatrixXi dataF;
Eigen::MatrixXd dataVec;
Eigen::MatrixXd curColor;

int loopLevel = 1;

PaintGeometry mPaint;

float vecratio = 0.001;
float wrinkleAmpScalingRatio = 1;

std::string workingFolder;
double ampTol = 0.1, curlTol = 1e-4;

// smoothing
int smoothingTimes = 3;
double smoothingRatio = 0.95;

bool isUseTangentCorrection = false;
bool isFixBnd = true;

void updateMagnitudePhase(const Eigen::VectorXd& omega, const std::vector<std::complex<double>>& zvals, Eigen::VectorXd& omegaNew, std::vector<std::complex<double>>& upZvals, Eigen::VectorXd& upsampledAmp, Eigen::VectorXd& upsampledPhase)
{
	NV = triV;
	NF = triF;
	Eigen::MatrixXd VN;
	igl::per_vertex_normals(triV, triF, VN);

	meshUpSampling(triV, triF, NV, NF, loopLevel, NULL, NULL, &bary);
	curvedPNTriangleUpsampling(triV, triF, VN, bary, NV, newN);

	secMesh = convert2SecMesh(triV, triF);
	subSecMesh = secMesh;

	Eigen::VectorXd edgeVec = swapEdgeVec(triF, omega, 0);

	std::shared_ptr<ComplexLoop> loopOpt1 = std::make_shared<ComplexLoopAmpPhaseEdgeJump>();
	loopOpt1->SetMesh(secMesh);
	loopOpt1->setBndFixFlag(isFixBnd);
	loopOpt1->Subdivide(edgeVec, zvals, omegaNew, upZvals, loopLevel);
	subSecMesh = (loopOpt1->GetMesh());
	parseSecMesh(subSecMesh, loopTriV, loopTriF);

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

	polyscope::getSurfaceMesh("input mesh")->addFaceVectorQuantity("vector field", faceOmega);

	Eigen::VectorXd edgeVec = swapEdgeVec(triF, initOmega, 0);
	std::shared_ptr<ComplexLoop> loopOpt1 = std::make_shared<ComplexLoopZuenko>();
	loopOpt1->SetMesh(secMesh);
	loopOpt1->setBndFixFlag(isFixBnd);
	loopOpt1->Subdivide(edgeVec, initZvals, loopedOmega, loopedZvals, loopLevel);
	subSecMesh = loopOpt1->GetMesh();
	parseSecMesh(subSecMesh, loopTriV, loopTriF);

	loopedAmp.resize(loopTriV.rows());
	loopedPhase.resize(loopTriV.rows());

	Eigen::VectorXd realPart(loopTriV.rows());
	Eigen::VectorXd imagPart(loopTriV.rows());

    for (int i = 0; i < loopedAmp.rows(); i++)
    {
        loopedAmp(i) = std::abs(loopedZvals[i]);
        loopedPhase(i) = std::arg(loopedZvals[i]);
		realPart(i) = loopedZvals[i].real();
		imagPart(i) = loopedZvals[i].imag();
    }

	polyscope::registerSurfaceMesh("looped base mesh", loopTriV, loopTriF);
	double shiftx = 1.5 * (triV.col(0).maxCoeff() - triV.col(0).minCoeff());

	polyscope::getSurfaceMesh("looped base mesh")->translate(glm::vec3(shiftx, 0, 0));
	//polyscope::getSurfaceMesh("looped amp mesh")->setEnabled(false);

	polyscope::getSurfaceMesh("looped base mesh")->addVertexScalarQuantity("amp", loopedAmp);
	polyscope::getSurfaceMesh("looped base mesh")->addVertexScalarQuantity("phi", loopedPhase);
	polyscope::getSurfaceMesh("looped base mesh")->addVertexScalarQuantity("real", realPart);
	polyscope::getSurfaceMesh("looped base mesh")->addVertexScalarQuantity("imag", imagPart);
	//polyscope::getSurfaceMesh("looped amp mesh")->getQuantity("amp color")->setEnabled(true);

	mPaint.setNormalization(false);
	Eigen::MatrixXd phiColor = mPaint.paintPhi(loopedPhase);
	polyscope::getSurfaceMesh("looped base mesh")->addVertexColorQuantity("phase color", phiColor);

	loopedFaceOmega = edgeVec2FaceVec(subSecMesh, loopedOmega);
	polyscope::getSurfaceMesh("looped base mesh")->addFaceVectorQuantity("upsampled vector field", loopedFaceOmega);


	Eigen::VectorXd edgeArea = getEdgeArea(loopTriV, loopTriF);
	Eigen::VectorXd vertArea = getVertArea(loopTriV, loopTriF);

    Eigen::MatrixXd wrinkledTriV;

    std::vector<std::vector<int>> vertNeiEdges, vertNeiFaces;
    buildVertexNeighboringInfo(MeshConnectivity(loopTriF), loopTriV.rows(), vertNeiEdges, vertNeiFaces);
    getWrinkledMesh(loopTriV, loopTriF, loopedZvals, &vertNeiFaces, wrinkledTriV, wrinkleAmpScalingRatio, isUseTangentCorrection);
    Eigen::MatrixXd lapWrinkledTriV = wrinkledTriV;
    laplacianSmoothing(wrinkledTriV, loopTriF, lapWrinkledTriV, smoothingRatio, smoothingTimes, isFixBnd);

    Eigen::MatrixXd faceColors(loopTriF.rows(), 3);
    for (int i = 0; i < faceColors.rows(); i++)
    {
        faceColors.row(i) << 80 / 255.0, 122 / 255.0, 91 / 255.0;
    }

    igl::writeOBJ(workingFolder + "wrinkledMesh_loop.obj", wrinkledTriV, loopTriF);

    polyscope::registerSurfaceMesh("loop wrinkled mesh", lapWrinkledTriV, loopTriF);
    polyscope::getSurfaceMesh("loop wrinkled mesh")->setSurfaceColor({ 80 / 255.0, 122 / 255.0, 91 / 255.0 });
    polyscope::getSurfaceMesh("loop wrinkled mesh")->addFaceColorQuantity("wrinkled color", faceColors);
    polyscope::getSurfaceMesh("loop wrinkled mesh")->translate(glm::vec3(2 * shiftx, 0, 0));
    polyscope::getSurfaceMesh("loop wrinkled mesh")->setEnabled(true);
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

	std::string initAmpPath = "amp.txt";
	std::string initOmegaPath = jval["init_omega"];
	std::string initZValsPath = "zvals.txt";
	if (jval.contains(std::string_view{ "init_zvals" }))
	{
		initZValsPath = jval["init_zvals"];
	}
    if (jval.contains(std::string_view{ "init_amp" }))
    {
        initAmpPath = jval["init_amp"];
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
        updateFieldsInView();
	}

	if (ImGui::CollapsingHeader("Visualization Options", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::Checkbox("Fix bnd", &isFixBnd))
			updateFieldsInView();
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
            updateFieldsInView();
		}
		if(ImGui::InputDouble("smoothing ratio", &smoothingRatio))
		{
			smoothingRatio = smoothingRatio > 0 ? smoothingRatio : 0;
            updateFieldsInView();
		}
	}

	if (ImGui::Button("comb fields & updates", ImVec2(-1, 0)))
	{
		// solve for the path from source to target
		updatePaintingItems();
		updateFieldsInView();
	}

    if (ImGui::Button("test loop", ImVec2(-1, 0)))
    {
        std::shared_ptr<ComplexLoop> opt1, opt2;
        opt1 = std::make_shared<ComplexLoopAmpPhase>();
//        opt2 = std::make_shared<ComplexLoopZuenko>();
        opt2 = std::make_shared<ComplexLoopAmpPhaseEdgeJump>();

        opt1->SetMesh(secMesh);
        opt2->SetMesh(secMesh);
        opt1->setBndFixFlag(isFixBnd);
        opt2->setBndFixFlag(isFixBnd);

        Eigen::SparseMatrix<double> S0, S1;
        opt2->BuildS0(S0);
        opt2->BuildS1(S1);

        int nverts = secMesh.GetVertCount();
        int nedges = secMesh.GetEdgeCount();
        Eigen::VectorXd testTheta = 4 * M_PI * Eigen::VectorXd::Random(nverts);
        Eigen::VectorXd testAmp = Eigen::VectorXd::Random(nverts);
        testAmp = testAmp.cwiseAbs();

        std::vector<std::complex<double>> testZvals;
        for(int i = 0; i < nverts; i++)
        {
            testZvals.push_back(testAmp(i) * std::complex<double>(std::cos(testTheta(i)), std::sin(testTheta(i))));
        }

        Eigen::VectorXd testOmega(nedges), testUpOmega;

        for(int i = 0; i < nedges; i++)
        {
            int v0 = secMesh.GetEdgeVerts(i)[0];
            int v1 = secMesh.GetEdgeVerts(i)[1];

            testOmega(i) = testTheta(v1) - testTheta(v0);
        }

        std::vector<std::complex<double>> testUpZvals, testUpStandZvals;
        opt2->Subdivide(testOmega, testZvals, testUpOmega, testUpZvals, 1);
        opt1->Subdivide(testOmega, testZvals, testUpOmega, testUpStandZvals, 1);


        Eigen::VectorXd testStandLoopAmp = S0 * testAmp;
        Eigen::VectorXd testStandLoopTheta = S0 * testTheta;

        Mesh testUpMesh = opt2->GetMesh();
        int nupedges = testUpMesh.GetEdgeCount();
        Eigen::VectorXd standLoopDTheta(nupedges), zuenkoloopDTheta(nupedges);

        Eigen::VectorXd testZuenkoLoopAmp(testUpZvals.size()), testZuenkoLoopTheta(testUpZvals.size());

        double sinDiff = 0;
        for(int i = 0; i < testUpZvals.size(); i++)
        {
            testZuenkoLoopAmp(i)= std::abs(testUpZvals[i]);
            testZuenkoLoopTheta(i) = std::arg(testUpZvals[i]);
            sinDiff += std::pow(std::sin(testZuenkoLoopTheta(i)) - std::sin(testStandLoopTheta(i)), 2);
        }

        for(int i = 0; i < nupedges; i++)
        {
            int v0 = testUpMesh.GetEdgeVerts(i)[0];
            int v1 = testUpMesh.GetEdgeVerts(i)[1];
            standLoopDTheta(i) = testStandLoopTheta(v1) - testStandLoopTheta(v0);
            zuenkoloopDTheta(i) = testZuenkoLoopTheta(v1) - testStandLoopTheta(v0);
        }

        std::cout << "amp error: " << (testZuenkoLoopAmp - testStandLoopAmp).norm() << std::endl;
        std::cout << "theta error: " << (testZuenkoLoopTheta - testStandLoopTheta).norm() << ", sin theta error: " << sinDiff << std::endl;
        std::cout << "rule check: " << (S1 * testOmega - testUpOmega).norm() << " " << (standLoopDTheta - testUpOmega).norm() << " " << (zuenkoloopDTheta - testUpOmega).norm() << std::endl;

//        std::cout << "stand result, our result: " << std::endl;
//        for(int i = 0; i < testUpZvals.size(); i++)
//        {
//            std::cout << "amp: " << i << " " << testStandLoopAmp(i) << " " << testZuenkoLoopAmp(i) << std::endl;
//            std::cout << "theta: " << i << " " << testStandLoopTheta(i) << " " << testZuenkoLoopTheta(i) << std::endl;
//            std::cout << "zvals: " << i << " " << testUpStandZvals[i] << " " << testUpZvals[i] << std::endl << std::endl;
//        }

    }

	if (ImGui::Button("display informations", ImVec2(-1, 0)))
	{
		Eigen::VectorXd loopedOmegaNew = loopedOmega, loopedAmpNew = loopedAmp, loopedPhaseNew = loopedPhase;

		Eigen::MatrixXd loopTriVNew;
		Eigen::MatrixXi loopTriFNew;

		Eigen::VectorXd edgeVec = swapEdgeVec(triF, initOmega, 0);
		std::vector<std::complex<double>> tmpZvals;

		std::shared_ptr<ComplexLoop> loopOpt2 = std::make_shared<ComplexLoopZuenko>();
		loopOpt2->SetMesh(secMesh);
		loopOpt2->setBndFixFlag(isFixBnd);
		loopOpt2->Subdivide(edgeVec, initZvals, loopedOmegaNew, tmpZvals, 2);
		subSecMesh = loopOpt2->GetMesh();
		parseSecMesh(subSecMesh, loopTriVNew, loopTriFNew);

		if (subSecMesh.GetFaceCount() > 4492)
		{
			int f0 = 4492;
			int f1 = 4486;

			std::cout << "fid: " << f0 << std::endl;
			for (int i = 0; i < 3; i++)
			{
				int vid = subSecMesh.GetFaceVerts(f0)[i];
				int eid = subSecMesh.GetFaceEdges(f0)[i];
				std::cout << "vid: " << vid << ", pos: " << subSecMesh.GetVertPos(vid).transpose() << ", z: " << tmpZvals[vid] << std::endl;
				std::cout << "eid: " << eid << ", v0: " << subSecMesh.GetEdgeVerts(eid)[0] << ", v1: " << subSecMesh.GetEdgeVerts(eid)[1] << ", omega: " << loopedOmegaNew(eid) << std::endl;
			}
			std::cout << "\nfid: " << f1 << std::endl;
			for (int i = 0; i < 3; i++)
			{
				int vid = subSecMesh.GetFaceVerts(f1)[i];
				int eid = subSecMesh.GetFaceEdges(f1)[i];
				std::cout << "vid: " << vid << ", pos: " << subSecMesh.GetVertPos(vid).transpose() << ", z: " << tmpZvals[vid] << std::endl;
				std::cout << "eid: " << eid << ", v0: " << subSecMesh.GetEdgeVerts(eid)[0] << ", v1: " << subSecMesh.GetEdgeVerts(eid)[1] << ", omega: " << loopedOmegaNew(eid) << std::endl;
			}
		}

       /* int printVid = 114;
        if(subSecMesh.GetVertCount() > printVid)
        {
            std::cout << "\nloop level: " << loopLevel << std::endl;
            std::cout << "zvals: " << tmpZvals[printVid] << std::endl;
            auto neiEdges = subSecMesh.GetVertEdges(printVid);

            std::cout << "\nneighboring edge info: " << std::endl;
            for(auto& e: neiEdges)
            {
                std::cout << "\neid: " << e << ", omega: " << loopedOmegaNew(e) << ", end verts: " <<  subSecMesh.GetEdgeVerts(e)[0] << " " << subSecMesh.GetEdgeVerts(e)[1] << std::endl;
            }

            auto neiFaces = subSecMesh.GetVertFaces(printVid);

            std::cout << "\nneighboring face info: " << std::endl;
            for(auto& f: neiFaces)
            {
                std::cout << "\nfid: " << f << std::endl;
                std::cout << "face vertices: \n";
                for(int i = 0; i < 3; i++)
                {
                    std::cout << "v_" << i << ": " << subSecMesh.GetFaceVerts(f)[i] << ", z: " << loopedZvals[subSecMesh.GetFaceVerts(f)[i]] << ", theta: " << std::arg(loopedZvals[subSecMesh.GetFaceVerts(f)[i]]) << ", amp: " << std::abs(loopedZvals[subSecMesh.GetFaceVerts(f)[i]]) << ", pos: " << subSecMesh.GetVertPos(subSecMesh.GetFaceVerts(f)[i]).transpose() << std::endl;
                }
                std::cout << "face edges: \n";
                for(int i = 0; i < 3; i++)
                {
                    int e =  subSecMesh.GetFaceEdges(f)[i];
                    std::cout << "e_" << i << ": " << e << ", w: " << loopedOmegaNew[e] << ", end verts: " <<  subSecMesh.GetEdgeVerts(e)[0] << " " << subSecMesh.GetEdgeVerts(e)[1] << std::endl;
                }
            }
        }*/
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