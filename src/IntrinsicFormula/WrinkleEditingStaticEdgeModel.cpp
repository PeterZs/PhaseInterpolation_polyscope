#include "../../include/IntrinsicFormula/WrinkleEditingStaticEdgeModel.h"
#include "../../include/Optimization/NewtonDescent.h"
#include "../../include/IntrinsicFormula/KnoppelStripePatternEdgeOmega.h"
#include "../../include/WrinkleFieldsEditor.h"
#include <igl/cotmatrix_entries.h>
#include <igl/cotmatrix.h>
#include <igl/doublearea.h>
#include <igl/boundary_loop.h>
#include <Eigen/SPQRSupport>

using namespace IntrinsicFormula;

WrinkleEditingStaticEdgeModel::WrinkleEditingStaticEdgeModel(const Eigen::MatrixXd& pos, const MeshConnectivity& mesh, const std::vector<VertexOpInfo>& vertexOpts, const Eigen::VectorXi& faceFlag, int quadOrd, double spatialAmpRatio, double spatialEdgeRatio, double spatialKnoppelRatio)
{
	_pos = pos;
	_mesh = mesh;

	_quadOrd = quadOrd;
	_spatialAmpRatio = spatialAmpRatio;
	_spatialEdgeRatio = spatialEdgeRatio;
	_spatialKnoppelRatio = spatialKnoppelRatio;

	_vertexOpts = vertexOpts;

	int nverts = pos.rows();
	int nfaces = mesh.nFaces();
	int nedges = mesh.nEdges();

	_edgeCotCoeffs.setZero(nedges);

	_interfaceVertFlags.resize(nverts);
	_interfaceVertFlags.setConstant(-1);

	_interfaceEdgeFlags.resize(nedges);
	_interfaceEdgeFlags.setConstant(-1);
	

	buildVertexNeighboringInfo(_mesh, _pos.rows(), _vertNeiEdges, _vertNeiFaces);
	_vertArea = getVertArea(_pos, _mesh);
	_edgeArea = getEdgeArea(_pos, _mesh);
	_faceArea = getFaceArea(_pos, _mesh);
	

	std::vector<int> bnds;
	igl::boundary_loop(_mesh.faces(), bnds);

	_nInterfaces = 0;

	Eigen::MatrixXd cotMatrixEntries;

	igl::cotmatrix_entries(pos, mesh.faces(), cotMatrixEntries);

	for (int i = 0; i < nfaces; i++)
	{
		for (int j = 0; j < 3; j++)
		{
			int eid = _mesh.faceEdge(i, j);
			int vid = _mesh.faceVertex(i, j);
			_edgeCotCoeffs(eid) += cotMatrixEntries(i, j);

			if (faceFlag(i) != -1)
			{
				_interfaceVertFlags(vid) = faceFlag(i);
				_interfaceEdgeFlags(eid) = faceFlag(i);
			}
		}
	}

	_faceVertMetrics.resize(nfaces);
	for (int i = 0; i < nfaces; i++)
	{
		_faceVertMetrics[i].resize(3);
		for (int j = 0; j < 3; j++)
		{
			int vid = _mesh.faceVertex(i, j);
			int vidj = _mesh.faceVertex(i, (j + 1) % 3);
			int vidk = _mesh.faceVertex(i, (j + 2) % 3);

			Eigen::Vector3d e0 = _pos.row(vidj) - _pos.row(vid);
			Eigen::Vector3d e1 = _pos.row(vidk) - _pos.row(vid);

			Eigen::Matrix2d I;
			I << e0.dot(e0), e0.dot(e1), e1.dot(e0), e1.dot(e1);
			_faceVertMetrics[i][j] = I.inverse();
		}
	}
	buildDOFs(faceFlag);
	_nInterfaces = _interfaceFids.size();

	std::cout << "number of interfaces: " << _nInterfaces << std::endl;
	std::cout << "min edge area: " << _edgeArea.minCoeff() << ", min vertex area: " << _vertArea.minCoeff() << std::endl;
	std::cout << "max edge area: " << _edgeArea.maxCoeff() << ", max vertex area: " << _vertArea.maxCoeff() << std::endl;

}

void WrinkleEditingStaticEdgeModel::buildDOFs(const Eigen::VectorXi& faceFlags)
{
	int nfaces = _mesh.nFaces();
	for (int i = 0; i < nfaces; i++)
	{
		if (faceFlags(i) == 1)
		{
			_selectedFids.push_back(i);
			_freeFids.push_back(i);
		}
		else if (faceFlags(i) == -1)
		{
			_interfaceFids.push_back(i);
			_freeFids.push_back(i);
		}
		else
			_unselectedFids.push_back(i);
	}
	// selected edges and verts
	Eigen::VectorXi selectedEdgeFlags, selectedVertFlags;
	getVertIdinGivenDomain(_selectedFids, selectedVertFlags);
	getEdgeIdinGivenDomain(_selectedFids, selectedEdgeFlags);

	// unselected edges and verts
	Eigen::VectorXi unselectedEdgeFlags, unselectedVertFlags;
	getVertIdinGivenDomain(_unselectedFids, unselectedVertFlags);
	getEdgeIdinGivenDomain(_unselectedFids, unselectedEdgeFlags);

	// interface edges and verts
	Eigen::VectorXi interfaceEdgeFlags, interfaceVertFlags;
	getVertIdinGivenDomain(_interfaceFids, interfaceVertFlags);
	getEdgeIdinGivenDomain(_interfaceFids, interfaceEdgeFlags);

	// free dofs
	Eigen::VectorXi freeVertFlags, freeEdgeFlags;
	getVertIdinGivenDomain(_freeFids, freeVertFlags);
	getEdgeIdinGivenDomain(_freeFids, freeEdgeFlags);

	// build the list
	int nverts = _pos.rows();
	int nedges = _mesh.nEdges();
	
	for (int i = 0; i < nverts; i++)
	{
		if (selectedVertFlags(i))
			_selectedVids.push_back(i);
		else if(unselectedVertFlags(i))
			_unselectedVids.push_back(i);
		else
			_interfaceVids.push_back(i);

		if (freeVertFlags(i))
			_freeVids.push_back(i);
	}

	for (int i = 0; i < nedges; i++)
	{
		if (selectedEdgeFlags(i))
			_selectedEids.push_back(i);
		else if (unselectedEdgeFlags(i))
			_unselectedEids.push_back(i);
		else
			_interfaceEids.push_back(i);

		if (freeEdgeFlags(i))
			_freeEids.push_back(i);
	}

	

	// building the map
	_actualVid2Free.resize(nverts);
	_actualVid2Free.setConstant(-1);

	for (int i = 0; i < _freeVids.size(); i++)
		_actualVid2Free[_freeVids[i]] = i;

	_actualEid2Free.resize(nedges);
	_actualEid2Free.setConstant(-1);
	
	for (int j = 0; j < _freeEids.size(); j++)
		_actualEid2Free[_freeEids[j]] = j;

}


void WrinkleEditingStaticEdgeModel::getVertIdinGivenDomain(const std::vector<int> faceList, Eigen::VectorXi& vertFlags)
{
	int nverts = _pos.rows();
	vertFlags.setZero(nverts);

	for (auto& fid : faceList)
	{
		for (int i = 0; i < 3; i++)
		{
			int vid = _mesh.faceVertex(fid, i);
			vertFlags(vid) = 1;
		}
	}
}

void WrinkleEditingStaticEdgeModel::getEdgeIdinGivenDomain(const std::vector<int> faceList, Eigen::VectorXi& edgeFlags)
{
	int nedges = _mesh.nEdges();
	edgeFlags.setZero(nedges);

	for (auto& fid : faceList)
	{
		for (int i = 0; i < 3; i++)
		{
			int eid = _mesh.faceEdge(fid, i);
			edgeFlags(eid) = 1;
		}
	}
}

void WrinkleEditingStaticEdgeModel::initialization(const std::vector<std::complex<double>>& initZvals, const Eigen::VectorXd& initOmega,
	double numFrames)
{
	Eigen::VectorXd initAmp;
	initAmp.setZero(_pos.rows());

	for (int i = 0; i < initAmp.rows(); i++)
	{
		initAmp(i) = std::abs(initZvals[i]);
	}

	std::vector<Eigen::VectorXd> refOmegaList(numFrames + 2);
	std::vector<Eigen::VectorXd> refAmpList(numFrames + 2);

	refAmpList[0] = initAmp;
	refOmegaList[0] = initOmega;

	double dt = 1.0 / (numFrames + 1);
	for (int i = 1; i <= numFrames + 1; i++)
	{
		std::vector<VertexOpInfo> curVertOpts = _vertexOpts;
		for (int j = 0; j < _vertexOpts.size(); j++)
		{
			if (_vertexOpts[j].vecOptType == None)
				continue;
			double offset = _vertexOpts[j].vecOptType != Enlarge ? 0 : 1;
			double A = _vertexOpts[j].vecOptType != Enlarge ? _vertexOpts[j].vecOptValue : _vertexOpts[j].vecOptValue - 1;

			curVertOpts[j].vecOptValue = offset + A * dt * i;
			curVertOpts[j].vecMagValue = 1 + (_vertexOpts[j].vecMagValue - 1) * dt * i;

		}

		WrinkleFieldsEditor::edgeBasedWrinkleEdition(_pos, _mesh, initAmp, initOmega, curVertOpts, refAmpList[i], refOmegaList[i]);
	}

	std::vector<std::complex<double>> tarZvals;

	Eigen::VectorXi fixedVertsFlag, fixedVertsFlagsStep2;
	fixedVertsFlag.setZero(_pos.rows());

	for (auto& vid : _unselectedVids)
		fixedVertsFlag(vid) = 1;

	fixedVertsFlagsStep2 = fixedVertsFlag;
	for (auto& vid : _selectedVids)
		fixedVertsFlagsStep2(vid) = 1;

	std::cout << "initialize bnd zvals." << std::endl;

	if (!_nInterfaces)
	{
		_combinedRefOmegaList = refOmegaList;
		_combinedRefAmpList = refAmpList;

		roundZvalsFromEdgeOmegaVertexMag(_mesh, refOmegaList[numFrames + 1], refAmpList[numFrames + 1], _edgeArea, _vertArea, _pos.rows(), tarZvals);
	}
	else
	{
		std::cout << "compute reference omega." << std::endl;
		computeCombinedRefOmegaList(refOmegaList);
		std::cout << "compute reference amplitude." << std::endl;
		computeCombinedRefAmpList(refAmpList, &_combinedRefOmegaList);

		tarZvals = initZvals;
		roundZvalsForSpecificDomainFromEdgeOmegaBndValues(_mesh, _combinedRefOmegaList[numFrames + 1], fixedVertsFlag, _edgeArea, _vertArea, _pos.rows(), tarZvals);

		for (int i = 0; i < tarZvals.size(); i++)
		{
			if (fixedVertsFlag[i] == 0)
			{
				double arg = std::arg(tarZvals[i]);
				tarZvals[i] = refAmpList[numFrames + 1][i] * std::complex<double>(std::cos(arg), std::sin(arg));
			}

		}

		roundZvalsForSpecificDomainFromEdgeOmegaBndValues(_mesh, _combinedRefOmegaList[numFrames + 1], fixedVertsFlagsStep2, _edgeArea, _vertArea, _pos.rows(), tarZvals);

		for (int i = 0; i < tarZvals.size(); i++)
		{
			if (fixedVertsFlagsStep2[i] == 0)
			{
				double arg = std::arg(tarZvals[i]);
				tarZvals[i] = refAmpList[numFrames + 1][i] * std::complex<double>(std::cos(arg), std::sin(arg));
			}

		}
	}



	_edgeOmegaList = _combinedRefOmegaList;
	_zvalsList.resize(numFrames + 2);

	_zvalsList[0] = initZvals;
	_zvalsList[numFrames + 1] = tarZvals;

	std::cout << "initialize the intermediate frames." << std::endl;
	for (int i = 1; i <= numFrames; i++)
	{
		double t = i * dt;

		_zvalsList[i] = tarZvals;

		for (int j = 0; j < tarZvals.size(); j++)
		{
			_zvalsList[i][j] = (1 - t) * initZvals[j] + t * tarZvals[j];
		}
	}

	_zdotModel = ComputeZdotFromEdgeOmega(_mesh, _faceArea, _quadOrd, dt);
	_refAmpAveList.resize(numFrames + 2);

	for (int i = 0; i < _refAmpAveList.size(); i++)
	{
		double ave = 0;
		for (int j = 0; j < _pos.rows(); j++)
		{
			ave += _combinedRefAmpList[i][j];
		}
		ave /= _pos.rows();
		_refAmpAveList[i] = ave;
	}
}

void WrinkleEditingStaticEdgeModel::warmstart()
{
	int nFreeVerts = _freeVids.size();
	int nFreeEdges = _freeEids.size();

	int numFrames = _zvalsList.size() - 2;

	int DOFsPerframe = 2 * nFreeVerts;

	int DOFs = numFrames * DOFsPerframe;

	auto convertVec2ZList = [&](const Eigen::VectorXd& x)
	{
		for (int i = 0; i < numFrames; i++)
		{
			for (int j = 0; j < nFreeVerts; j++)
			{
				_zvalsList[i + 1][_freeVids[j]] = std::complex<double>(x(i * DOFsPerframe + 2 * j), x(i * DOFsPerframe + 2 * j + 1));
			}
		}
	};

	auto convertZList2Vec = [&](Eigen::VectorXd& x)
	{
		x.setZero(DOFs);

		for (int i = 0; i < numFrames; i++)
		{
			for (int j = 0; j < nFreeVerts; j++)
			{
				x(i * DOFsPerframe + 2 * j) = _zvalsList[i + 1][_freeVids[j]].real();
				x(i * DOFsPerframe + 2 * j + 1) = _zvalsList[i + 1][_freeVids[j]].imag();
			}
		}
	};

	double dt = 1.0 / (numFrames + 1);

	auto funVal = [&](const Eigen::VectorXd& x, Eigen::VectorXd* grad, Eigen::SparseMatrix<double>* hess, bool isProj)
	{
		convertVec2ZList(x);
		double energy = 0;
		if (grad)
		{
			grad->setZero(DOFs);
		}

		std::vector<Eigen::Triplet<double>> T, curT;
		Eigen::VectorXd curDeriv;
		
		for (int i = 0; i < _zvalsList.size() - 1; i++)
		{
			energy += naiveKineticEnergy(i, grad ? &curDeriv : NULL, hess ? &curT : NULL, false);
			if (grad)
			{
				if (i == 0)
					grad->segment(0, DOFsPerframe) += curDeriv.segment(DOFsPerframe, DOFsPerframe);
				else if (i == _zvalsList.size() - 2)
					grad->segment((i - 1) * DOFsPerframe, DOFsPerframe) += curDeriv.segment(0, DOFsPerframe);
				else
				{
					grad->segment((i - 1) * DOFsPerframe, 2 * DOFsPerframe) += curDeriv;
				}
			}

			if (hess)
			{
				for (auto& it : curT)
				{

					if (i == 0)
					{
						if (it.row() >= DOFsPerframe && it.col() >= DOFsPerframe)
							T.push_back({ it.row() - DOFsPerframe, it.col() - DOFsPerframe, it.value() });
					}
					else if (i == _zvalsList.size() - 2)
					{
						if (it.row() < DOFsPerframe && it.col() < DOFsPerframe)
							T.push_back({ it.row() + (i - 1) * DOFsPerframe, it.col() + (i - 1) * DOFsPerframe, it.value() });
					}
					else
					{
						T.push_back({ it.row() + (i - 1) * DOFsPerframe, it.col() + (i - 1) * DOFsPerframe, it.value() });
					}


				}
				curT.clear();
			}
		}

		
		for (int i = 0; i < numFrames; i++)
		{
			int id = i + 1;
			Eigen::VectorXd ampDeriv, knoppelDeriv;
			std::vector<Eigen::Triplet<double>> ampT, knoppelT;
			energy += temporalAmpDifference(i + 1, grad ? &ampDeriv : NULL, hess ? &ampT : NULL, isProj);
			energy += spatialKnoppelEnergy(i + 1, grad ? &knoppelDeriv : NULL, hess ? &knoppelT : NULL, isProj);

			if (grad)
			{
				grad->segment(i * DOFsPerframe, 2 * nFreeVerts) += ampDeriv + knoppelDeriv;
			}

			if (hess)
			{
				for (auto& it : ampT)
				{
					T.push_back({ i * DOFsPerframe + it.row(), i * DOFsPerframe + it.col(), it.value() });
				}
				for (auto& it : knoppelT)
				{
					T.push_back({ i * DOFsPerframe + it.row(), i * DOFsPerframe + it.col(), it.value() });
				}
			}
		}

		if (hess)
		{
			//std::cout << "num of triplets: " << T.size() << std::endl;
			hess->resize(DOFs, DOFs);
			hess->setFromTriplets(T.begin(), T.end());
		}

		return energy;

	};
	auto maxStep = [&](const Eigen::VectorXd& x, const Eigen::VectorXd& dir) {
		return 1.0;
	};

	auto postProcess = [&](Eigen::VectorXd& x)
	{
		//            interpModel.postProcess(x);
	};

	Eigen::VectorXd x, x0;
	convertZList2Vec(x);
	x0 = x;
	OptSolver::testFuncGradHessian(funVal, x0);
	OptSolver::newtonSolver(funVal, maxStep, x, 1000, 1e-6, 0, 0, true);
	std::cout << "before optimization: ||x|| = " << x0.norm() << std::endl;
	std::cout << "after optimization: ||x|| = " << x.norm() << std::endl;
	convertVec2ZList(x);

}

void WrinkleEditingStaticEdgeModel::convertList2Variable(Eigen::VectorXd& x)
{
	int nFreeVerts = _freeVids.size();
	int nFreeEdges = _freeEids.size();

	int numFrames = _zvalsList.size() - 2;

	int DOFsPerframe = (2 * nFreeVerts + nFreeEdges);

	int DOFs = numFrames * DOFsPerframe;

	x.setZero(DOFs);

	for (int i = 0; i < numFrames; i++)
	{
		for (int j = 0; j < nFreeVerts; j++)
		{
			x(i * DOFsPerframe + 2 * j) = _zvalsList[i + 1][_freeVids[j]].real();
			x(i * DOFsPerframe + 2 * j + 1) = _zvalsList[i + 1][_freeVids[j]].imag();
		}

		for (int j = 0; j < nFreeEdges; j++)
		{
			x(i * DOFsPerframe + 2 * nFreeVerts + j) = _edgeOmegaList[i + 1](_freeEids[j]);
		}
	}
}

void WrinkleEditingStaticEdgeModel::convertVariable2List(const Eigen::VectorXd& x)
{
	int nFreeVerts = _freeVids.size();
	int nFreeEdges = _freeEids.size();

	int numFrames = _zvalsList.size() - 2;

	int DOFsPerframe = (2 * nFreeVerts + nFreeEdges);

	for (int i = 0; i < numFrames; i++)
	{
		for (int j = 0; j < nFreeVerts; j++)
		{
			_zvalsList[i + 1][_freeVids[j]] = std::complex<double>(x(i * DOFsPerframe + 2 * j), x(i * DOFsPerframe + 2 * j + 1));
		}

		for (int j = 0; j < nFreeEdges; j++)
		{
			_edgeOmegaList[i + 1](_freeEids[j]) = x(i * DOFsPerframe + 2 * nFreeVerts + j);
		}
	}
}

double WrinkleEditingStaticEdgeModel::amplitudeEnergyWithGivenOmegaPerface(const Eigen::VectorXd& amp, const Eigen::VectorXd& w,
	int fid, Eigen::Vector3d* deriv,
	Eigen::Matrix3d* hess)
{
	double energy = 0;

	double curlSq = curlFreeEnergyPerface(w, fid, NULL, NULL);
	Eigen::Vector3d wSq;
	wSq.setZero();


	if (deriv)
		deriv->setZero();
	if (hess)
		hess->setZero();

	for (int i = 0; i < 3; i++)
	{
		int vid = _mesh.faceVertex(fid, i);
		energy += 0.5 * amp(vid) * amp(vid) / 3 * (wSq(i) * _faceArea(fid) + curlSq);

		if (deriv)
			(*deriv)(i) += amp(vid) * (wSq(i) * _faceArea(fid) + curlSq) / 3;
		if (hess)
			(*hess)(i, i) += (wSq(i) * _faceArea(fid) + curlSq) / 3;
	}

	return energy;
}

double WrinkleEditingStaticEdgeModel::amplitudeEnergyWithGivenOmega(const Eigen::VectorXd& amp, const Eigen::VectorXd& w,
	Eigen::VectorXd* deriv,
	std::vector<Eigen::Triplet<double>>* hessT)
{
	double energy = 0;

	int nverts = _pos.rows();
	int nEffectiveFaces = _interfaceFids.size();

	std::vector<double> energyList(nEffectiveFaces);
	std::vector<Eigen::Vector3d> derivList(nEffectiveFaces);
	std::vector<Eigen::Matrix3d> hessList(nEffectiveFaces);

	auto computeEnergy = [&](const tbb::blocked_range<uint32_t>& range) {
		for (uint32_t i = range.begin(); i < range.end(); ++i)
		{
			int fid = _interfaceFids[i];
			energyList[i] = amplitudeEnergyWithGivenOmegaPerface(amp, w, fid, deriv ? &derivList[i] : NULL, hessT ? &hessList[i] : NULL);
		}
	};

	tbb::blocked_range<uint32_t> rangex(0u, (uint32_t)nEffectiveFaces, GRAIN_SIZE);
	tbb::parallel_for(rangex, computeEnergy);

	if (deriv)
		deriv->setZero(nverts);
	if (hessT)
		hessT->clear();

	for (int efid = 0; efid < nEffectiveFaces; efid++)
	{
		energy += energyList[efid];
		int fid = _interfaceFids[efid];

		if (deriv)
		{
			for (int j = 0; j < 3; j++)
			{
				int vid = _mesh.faceVertex(fid, j);
				(*deriv)(vid) += derivList[efid](j);
			}
		}

		if (hessT)
		{
			for (int j = 0; j < 3; j++)
			{
				int vid = _mesh.faceVertex(fid, j);
				for (int k = 0; k < 3; k++)
				{
					int vid1 = _mesh.faceVertex(fid, k);
					hessT->push_back({ vid, vid1, hessList[efid](j, k) });
				}
			}
		}
	}
	return energy;
}

void WrinkleEditingStaticEdgeModel::computeCombinedRefAmpList(const std::vector<Eigen::VectorXd>& refAmpList, std::vector<Eigen::VectorXd>* combinedOmegaList)
{
	int nverts = _pos.rows();
	int nfaces = _mesh.nFaces();
	int nFrames = refAmpList.size();

	_combinedRefAmpList.resize(nFrames);

	double c = std::min(1.0 / (nFrames * nFrames), 1e-3);

	std::vector<Eigen::Triplet<double>> T;
	// projection matrix
	std::vector<int> freeVid = _interfaceVids;
	Eigen::VectorXi fixedFlags = Eigen::VectorXi::Ones(nverts);

	for (int i = 0; i < freeVid.size(); i++)
	{
		T.push_back(Eigen::Triplet<double>(i, freeVid[i], 1.0));
		fixedFlags(freeVid[i]) = 0;
	}

	Eigen::SparseMatrix<double> projM(freeVid.size(), nverts);
	projM.setFromTriplets(T.begin(), T.end());

	Eigen::SparseMatrix<double> unProjM = projM.transpose();

	auto projVar = [&](const int frameId)
	{
		Eigen::MatrixXd fullX = refAmpList[frameId];
		Eigen::VectorXd x0 = projM * fullX;
		return x0;
	};

	auto unProjVar = [&](const Eigen::VectorXd& x, const int frameId)
	{
		Eigen::VectorXd fullX = unProjM * x;

		for (int i = 0; i < nverts; i++)
		{
			if (fixedFlags(i))
			{
				fullX(i) = refAmpList[frameId](i);
			}
		}
		return fullX;
	};

	Eigen::SparseMatrix<double> L;
	igl::cotmatrix(_pos, _mesh.faces(), L);

	_combinedRefAmpList[0] = refAmpList[0];

	Eigen::VectorXd prevX = refAmpList[0];

	Eigen::SparseMatrix<double> idmat(prevX.rows(), prevX.rows());
	idmat.setIdentity();


	for (int i = 1; i < nFrames; i++)
	{
		std::cout << "Frame " << std::to_string(i) << ": free vertices: " << freeVid.size() << std::endl;;
		auto funVal = [&](const Eigen::VectorXd& x, Eigen::VectorXd* grad, Eigen::SparseMatrix<double>* hess, bool isProj) {
			Eigen::VectorXd deriv, deriv1;
			std::vector<Eigen::Triplet<double>> T;
			Eigen::SparseMatrix<double> H;

			Eigen::VectorXd fullx = unProjVar(x, i);
			double E = -0.5 * fullx.dot(L * fullx) + 0.5 * c * (fullx - prevX).squaredNorm();

			if (combinedOmegaList)
			{
				E += amplitudeEnergyWithGivenOmega(fullx, (*combinedOmegaList)[i], grad ? &deriv1 : NULL, hess ? &T : NULL);
			}

			if (grad)
			{
				deriv = -L * fullx + c * (fullx - prevX);
				if (combinedOmegaList)
					deriv += deriv1;
				(*grad) = projM * deriv;
			}

			if (hess)
			{
				if (combinedOmegaList)
				{
					H.resize(fullx.rows(), fullx.rows());
					H.setFromTriplets(T.begin(), T.end());
					(*hess) = projM * (H - L + c * idmat) * unProjM;
				}

				else
					(*hess) = projM * (-L + c * idmat) * unProjM;

			}

			return E;
		};
		auto maxStep = [&](const Eigen::VectorXd& x, const Eigen::VectorXd& dir) {
			return 1.0;
		};

		Eigen::VectorXd x0 = projVar(i);
		if (_nInterfaces && freeVid.size())
		{
			OptSolver::newtonSolver(funVal, maxStep, x0, 1000, 1e-6, 1e-10, 1e-15, false);

			Eigen::VectorXd deriv;
			double E = funVal(x0, &deriv, NULL, false);
			std::cout << "terminated with energy : " << E << ", gradient norm : " << deriv.norm() << std::endl << std::endl;
		}

		_combinedRefAmpList[i] = unProjVar(x0, i);
		prevX = _combinedRefAmpList[i];
	}
}

double WrinkleEditingStaticEdgeModel::curlFreeEnergyPerface(const Eigen::MatrixXd& w, int faceId, Eigen::Matrix<double, 3, 1>* deriv, Eigen::Matrix<double, 3, 3>* hess)
{
	double E = 0;

	double diff0;
	Eigen::Matrix<double, 3, 1> select0;
	select0.setZero();

	Eigen::Matrix<double, 3, 1> edgews;

	for (int i = 0; i < 3; i++)
	{
		int eid = _mesh.faceEdge(faceId, i);
		edgews(i) = w(eid);

		if (_mesh.faceVertex(faceId, (i + 1) % 3) == _mesh.edgeVertex(eid, 0))
		{
			select0(i) = 1;
		}

		else
		{
			select0(i) = -1;
		}
	}
	diff0 = select0.dot(edgews);

	E = 0.5 * (diff0 * diff0);
	if (deriv)
	{
		*deriv = select0 * diff0;
	}
	if (hess)
	{
		*hess = select0 * select0.transpose();
	}

	return E;
}


double WrinkleEditingStaticEdgeModel::curlFreeEnergy(const Eigen::MatrixXd& w, Eigen::VectorXd* deriv, std::vector<Eigen::Triplet<double>>* hessT)
{
	double E = 0;
	int nedges = _mesh.nEdges();
	int nEffectiveFaces = _interfaceFids.size();

	std::vector<double> energyList(nEffectiveFaces);
	std::vector<Eigen::Matrix<double, 3, 1>> derivList(nEffectiveFaces);
	std::vector<Eigen::Matrix<double, 3, 3>> hessList(nEffectiveFaces);

	auto computeEnergy = [&](const tbb::blocked_range<uint32_t>& range) {
		for (uint32_t i = range.begin(); i < range.end(); ++i)
		{
			int fid = _interfaceFids[i];
			energyList[i] = curlFreeEnergyPerface(w, fid, deriv ? &derivList[i] : NULL, hessT ? &hessList[i] : NULL);
		}
	};

	tbb::blocked_range<uint32_t> rangex(0u, (uint32_t)nEffectiveFaces, GRAIN_SIZE);
	tbb::parallel_for(rangex, computeEnergy);


	if (deriv)
		deriv->setZero(nedges);
	if (hessT)
		hessT->clear();

	for (int efid = 0; efid < nEffectiveFaces; efid++)
	{
		E += energyList[efid];
		int fid = _interfaceFids[efid];

		if (deriv)
		{
			for (int j = 0; j < 3; j++)
			{
				int eid = _mesh.faceEdge(fid, j);
				(*deriv)(eid) += derivList[efid](j);
			}
		}

		if (hessT)
		{
			for (int j = 0; j < 3; j++)
			{
				int eid = _mesh.faceEdge(fid, j);
				for (int k = 0; k < 3; k++)
				{
					int eid1 = _mesh.faceEdge(fid, k);
					hessT->push_back({ eid, eid1, hessList[efid](j, k) });
				}
			}
		}
	}

	return E;
}


double WrinkleEditingStaticEdgeModel::divFreeEnergyPervertex(const Eigen::MatrixXd& w, int vertId, Eigen::VectorXd* deriv, Eigen::MatrixXd* hess)
{
	double energy = 0;
	int neiEdges = _vertNeiEdges[vertId].size();


	Eigen::VectorXd selectedVec0;
	selectedVec0.setZero(neiEdges);

	Eigen::VectorXd edgew;
	edgew.setZero(neiEdges);

	for (int i = 0; i < neiEdges; i++)
	{
		int eid = _vertNeiEdges[vertId][i];
		if (_mesh.edgeVertex(eid, 0) == vertId)
		{
			selectedVec0(i) = _edgeCotCoeffs(eid);
		}
		else
		{
			selectedVec0(i) = -_edgeCotCoeffs(eid);
		}

		edgew(i) = w(eid);
	}
	double diff0 = selectedVec0.dot(edgew);

	energy = 0.5 * (diff0 * diff0);
	if (deriv)
	{
		(*deriv) = (diff0 * selectedVec0);
	}
	if (hess)
	{
		(*hess) = (selectedVec0 * selectedVec0.transpose());
	}

	return energy;
}

double WrinkleEditingStaticEdgeModel::divFreeEnergy(const Eigen::MatrixXd& w, Eigen::VectorXd* deriv,
	std::vector<Eigen::Triplet<double>>* hessT)
{
	double energy = 0;
	int nedges = _mesh.nEdges();

	Eigen::VectorXi interfaceVertFlags;
	getVertIdinGivenDomain(_interfaceFids, interfaceVertFlags);
	
	std::vector<int> effectiveVids;
	for (int i = 0; i < interfaceVertFlags.rows(); i++)
	{
		if (interfaceVertFlags(i))
			effectiveVids.push_back(i);
	}

	int nEffectiveVerts = effectiveVids.size();

	std::vector<double> energyList(nEffectiveVerts);
	std::vector<Eigen::VectorXd> derivList(nEffectiveVerts);
	std::vector<Eigen::MatrixXd> hessList(nEffectiveVerts);

	auto computeEnergy = [&](const tbb::blocked_range<uint32_t>& range) {
		for (uint32_t i = range.begin(); i < range.end(); ++i)
		{
			int vid = effectiveVids[i];
			energyList[i] = divFreeEnergyPervertex(w, vid, deriv ? &derivList[i] : NULL, hessT ? &hessList[i] : NULL);
		}
	};

	tbb::blocked_range<uint32_t> rangex(0u, (uint32_t)nEffectiveVerts, GRAIN_SIZE);
	tbb::parallel_for(rangex, computeEnergy);

	if (deriv)
		deriv->setZero(nedges);
	if (hessT)
		hessT->clear();

	for (int efid = 0; efid < nEffectiveVerts; efid++)
	{
		int vid = effectiveVids[efid];
		energy += energyList[efid];

		if (deriv)
		{
			for (int j = 0; j < _vertNeiEdges[vid].size(); j++)
			{
				int eid = _vertNeiEdges[vid][j];
				(*deriv)(eid) += derivList[efid](j);
			}
		}

		if (hessT)
		{
			for (int j = 0; j < _vertNeiEdges[vid].size(); j++)
			{
				int eid = _vertNeiEdges[vid][j];
				for (int k = 0; k < _vertNeiEdges[vid].size(); k++)
				{
					int eid1 = _vertNeiEdges[vid][k];
					hessT->push_back({ eid, eid1, hessList[efid](j, k) });
				}
			}
		}
	}

	return energy;
}

void WrinkleEditingStaticEdgeModel::computeCombinedRefOmegaList(const std::vector<Eigen::VectorXd>& refOmegaList)
{
	int nedges = _mesh.nEdges();
	int nFrames = refOmegaList.size();

	_combinedRefOmegaList.resize(nFrames);

	std::vector<Eigen::Triplet<double>> T;
	// projection matrix
	std::vector<int> freeEid = _interfaceEids;
	Eigen::VectorXi fixedFlags = Eigen::VectorXi::Ones(nedges);

	for (int i = 0; i < freeEid.size(); i++)
	{
		T.push_back(Eigen::Triplet<double>(i, freeEid[i], 1.0));
		fixedFlags(freeEid[i]) = 0;
	}

	Eigen::SparseMatrix<double> projM(freeEid.size(), nedges);
	projM.setFromTriplets(T.begin(), T.end());

	Eigen::SparseMatrix<double> unProjM = projM.transpose();

	auto projVar = [&](const int frameId)
	{
		Eigen::MatrixXd fullX = Eigen::VectorXd::Zero(nedges);
		for (int i = 0; i < nedges; i++)
		{
			if (fixedFlags(i))
			{
				fullX(i) = refOmegaList[frameId](i);
			}
		}
		Eigen::VectorXd x0 = projM * fullX;
		return x0;
	};

	auto unProjVar = [&](const Eigen::VectorXd& x, const int frameId)
	{
		Eigen::VectorXd fullX = unProjM * x;

		for (int i = 0; i < nedges; i++)
		{
			if (fixedFlags(i))
			{
				fullX(i) = refOmegaList[frameId](i);
			}

		}
		return fullX;
	};

	Eigen::VectorXd prevw = refOmegaList[0];
	_combinedRefOmegaList[0] = refOmegaList[0];

	for (int k = 1; k < nFrames; k++)
	{
		std::cout << "Frame " << std::to_string(k) << ": free edges: " << freeEid.size() << std::endl;
		auto funVal = [&](const Eigen::VectorXd& x, Eigen::VectorXd* grad, Eigen::SparseMatrix<double>* hess, bool isProj) {
			Eigen::VectorXd deriv, deriv1;
			std::vector<Eigen::Triplet<double>> T, T1;
			Eigen::SparseMatrix<double> H;
			Eigen::VectorXd w = unProjVar(x, k);

			double E = curlFreeEnergy(w, grad ? &deriv : NULL, hess ? &T : NULL);
			E += divFreeEnergy(w, grad ? &deriv1 : NULL, hess ? &T1 : NULL);

			if (grad)
				deriv += deriv1;
			if (hess)
			{
				std::copy(T1.begin(), T1.end(), std::back_inserter(T));
				H.resize(w.rows(), w.rows());
				H.setFromTriplets(T.begin(), T.end());
			}


			// we need some reg to remove the singularity, where we choose some kinetic energy (||w - prevw||^2), which coeff = 1e-3
			double c = std::min(1.0 / (nFrames * nFrames), 1e-3);
			E += c / 2.0 * (w - prevw).squaredNorm();

			if (grad)
			{
				(*grad) = projM * (deriv + c * (w - prevw));
			}

			if (hess)
			{
				Eigen::SparseMatrix<double> idMat(w.rows(), w.rows());
				idMat.setIdentity();
				(*hess) = projM * (H + c * idMat) * unProjM;
			}

			return E;
		};
		auto maxStep = [&](const Eigen::VectorXd& x, const Eigen::VectorXd& dir) {
			return 1.0;
		};

		Eigen::VectorXd x0 = projVar(k);
		Eigen::VectorXd fullx = unProjVar(x0, k);

		if (_nInterfaces)
		{
			OptSolver::newtonSolver(funVal, maxStep, x0, 1000, 1e-6, 1e-10, 1e-15, false);
			Eigen::VectorXd deriv;
			double E = funVal(x0, &deriv, NULL, false);
			std::cout << "terminated with energy : " << E << ", gradient norm : " << deriv.norm() << std::endl << std::endl;
		}

		prevw = unProjVar(x0, k);
		_combinedRefOmegaList[k] = prevw;
	}
}


double WrinkleEditingStaticEdgeModel::temporalAmpDifference(int frameId, Eigen::VectorXd* deriv, std::vector<Eigen::Triplet<double>>* hessT, bool isProj)
{
	int nFreeVerts = _freeVids.size();
	double energy = 0;

	if (deriv)
		deriv->setZero(2 * nFreeVerts);
	if (hessT)
		hessT->clear();

	for (int i = 0; i < nFreeVerts; i++)
	{
		int vid = _freeVids[i];
		double ampSq = _zvalsList[frameId][vid].real() * _zvalsList[frameId][vid].real() +
			_zvalsList[frameId][vid].imag() * _zvalsList[frameId][vid].imag();
		double refAmpSq = _combinedRefAmpList[frameId][vid] * _combinedRefAmpList[frameId][vid];
		double ca = _spatialAmpRatio * _vertArea(vid) / (_refAmpAveList[frameId] * _refAmpAveList[frameId]);

		energy += ca * (ampSq - refAmpSq) * (ampSq - refAmpSq);

		if (deriv)
		{
			(*deriv)(2 * i) += 2.0 * ca * (ampSq - refAmpSq) * (2.0 * _zvalsList[frameId][vid].real());
			(*deriv)(2 * i + 1) += 2.0 * ca * (ampSq - refAmpSq) * (2.0 * _zvalsList[frameId][vid].imag());
		}

		if (hessT)
		{
			Eigen::Matrix2d tmpHess;
			tmpHess << 
				2.0 * _zvalsList[frameId][vid].real() * 2.0 * _zvalsList[frameId][vid].real(),
				2.0 * _zvalsList[frameId][vid].real() * 2.0 * _zvalsList[frameId][vid].imag(),
				2.0 * _zvalsList[frameId][vid].real() * 2.0 * _zvalsList[frameId][vid].imag(),
				2.0 * _zvalsList[frameId][vid].imag() * 2.0 * _zvalsList[frameId][vid].imag();

			tmpHess *= 2.0 * ca;
			tmpHess += 2.0 * ca * (ampSq - refAmpSq) * (2.0 * Eigen::Matrix2d::Identity());


			if (isProj)
				tmpHess = SPDProjection(tmpHess);

			for (int k = 0; k < 2; k++)
				for (int l = 0; l < 2; l++)
					hessT->push_back({ 2 * i + k, 2 * i + l, tmpHess(k, l) });
		}
	}
	return energy;
}

double WrinkleEditingStaticEdgeModel::temporalOmegaDifference(int frameId, Eigen::VectorXd* deriv, std::vector<Eigen::Triplet<double>>* hessT, bool isProj)
{
	int nFreeEdges = _freeEids.size();
	double energy = 0;

	if (deriv)
		deriv->setZero(nFreeEdges);

	for (int i = 0; i < nFreeEdges; i++)
	{
		int eid = _freeEids[i];
		double ce = _spatialEdgeRatio * _edgeArea(eid) * (_refAmpAveList[frameId] * _refAmpAveList[frameId]);

		energy += ce * (_edgeOmegaList[frameId](eid) - _combinedRefOmegaList[frameId](eid)) * (_edgeOmegaList[frameId](eid) - _combinedRefOmegaList[frameId](eid));

		if (deriv) 
		{
			(*deriv)(i) += 2 * ce * (_edgeOmegaList[frameId](eid) - _combinedRefOmegaList[frameId](eid));
		}

		if (hessT) 
		{
			hessT->push_back(Eigen::Triplet<double>(i, i, 2 * ce));
		}
	}

	return energy;
}

double WrinkleEditingStaticEdgeModel::spatialKnoppelEnergy(int frameId, Eigen::VectorXd* deriv, std::vector<Eigen::Triplet<double>>* hessT, bool isProj)
{
	double energy = 0;
	int nFreeEdges = _freeEids.size();
	int nFreeVerts = _freeVids.size();
	double aveAmp = _refAmpAveList[frameId];
	std::vector<Eigen::Triplet<double>> AT;
	AT.clear();

	int maxFreeVid = 0;

	for (int fe = 0; fe < nFreeEdges; fe++)
	{
		int eid = _freeEids[fe];
		int vid0 = _mesh.edgeVertex(eid, 0);
		int vid1 = _mesh.edgeVertex(eid, 1);

		double r0 = _combinedRefAmpList[frameId](vid0) / aveAmp;
		double r1 = _combinedRefAmpList[frameId](vid1) / aveAmp;

		std::complex<double> expw0 = std::complex<double>(std::cos(_combinedRefOmegaList[frameId](eid)), std::sin(_combinedRefOmegaList[frameId](eid)));

		std::complex<double> z0 = _zvalsList[frameId][vid0];
		std::complex<double> z1 = _zvalsList[frameId][vid1];

		double ce = _spatialKnoppelRatio * _edgeArea(eid);

		energy += 0.5 * norm((r1 * z0 * expw0 - r0 * z1)) * ce;

		if (deriv || hessT)
		{
			int freeV0 = _actualVid2Free[vid0];
			int freeV1 = _actualVid2Free[vid1];


			AT.push_back({ 2 * freeV0, 2 * freeV0, r1 * r1 * ce });
			AT.push_back({ 2 * freeV0 + 1, 2 * freeV0 + 1, r1 * r1 * ce });

			AT.push_back({ 2 * freeV1, 2 * freeV1, r0 * r0 * ce });
			AT.push_back({ 2 * freeV1 + 1, 2 * freeV1 + 1, r0 * r0 * ce });


			AT.push_back({ 2 * freeV0, 2 * freeV1, -ce * (expw0.real()) * r0 * r1 });
			AT.push_back({ 2 * freeV0 + 1, 2 * freeV1, -ce * (-expw0.imag()) * r0 * r1 });
			AT.push_back({ 2 * freeV0, 2 * freeV1 + 1, -ce * (expw0.imag()) * r0 * r1 });
			AT.push_back({ 2 * freeV0 + 1, 2 * freeV1 + 1, -ce * (expw0.real()) * r0 * r1 });

			AT.push_back({ 2 * freeV1, 2 * freeV0, -ce * (expw0.real()) * r0 * r1 });
			AT.push_back({ 2 * freeV1, 2 * freeV0 + 1, -ce * (-expw0.imag()) * r0 * r1 });
			AT.push_back({ 2 * freeV1 + 1, 2 * freeV0, -ce * (expw0.imag()) * r0 * r1 });
			AT.push_back({ 2 * freeV1 + 1, 2 * freeV0 + 1, -ce * (expw0.real()) * r0 * r1 });
		}
	}

	if (deriv || hessT)
	{
		Eigen::SparseMatrix<double> A;
		
		A.resize(2 * nFreeVerts, 2 * nFreeVerts);
		A.setFromTriplets(AT.begin(), AT.end());

		// check whether A is PD


		if (deriv)
		{
			Eigen::VectorXd fvals(2 * nFreeVerts);
			for (int i = 0; i < nFreeVerts; i++)
			{
				fvals(2 * i) = _zvalsList[frameId][_freeVids[i]].real();
				fvals(2 * i + 1) = _zvalsList[frameId][_freeVids[i]].imag();
			}
			(*deriv) = A * fvals;
		}

		if (hessT)
			(*hessT) = AT;
	}

	return energy;
}

double WrinkleEditingStaticEdgeModel::kineticEnergy(int frameId, Eigen::VectorXd* deriv, std::vector<Eigen::Triplet<double>>* hessT, bool isProj)
{
	double energy = 0;
	if (frameId >= _zvalsList.size() - 1)
	{
		std::cerr << "frame id overflows" << std::endl;
		exit(EXIT_FAILURE);
	}
	int nFreeEdges = _freeEids.size();
	int nFreeVerts = _freeVids.size();
	int nFreeFaces = _freeFids.size();
	int nDOFs = 2 * nFreeVerts + nFreeEdges;

	if (deriv)
		deriv->setZero(2 * nDOFs);
	if (hessT)
		hessT->clear();

	for (int i = 0; i < nFreeFaces; i++)
	{
		int fid = _freeFids[i];

		Eigen::Matrix<double, 18, 1> faceDeriv;
		Eigen::Matrix<double, 18, 18> faceHess;

		energy += _zdotModel.computeZdotIntegrationPerface(_zvalsList[frameId], _edgeOmegaList[frameId], _zvalsList[frameId + 1], _edgeOmegaList[frameId + 1], fid, deriv ? &faceDeriv : NULL, hessT ? &faceHess : NULL, isProj);

		if (deriv)
		{
			for (int j = 0; j < 3; j++)
			{
				int freeVid = _actualVid2Free[_mesh.faceVertex(fid, j)];
				int freeEid = _actualEid2Free[_mesh.faceEdge(fid, j)];

				(*deriv)(2 * freeVid) += faceDeriv(2 * j);
				(*deriv)(2 * freeVid + 1) += faceDeriv(2 * j + 1);
				(*deriv)(freeEid + 2 * nFreeVerts) += faceDeriv(6 + j);

				(*deriv)(2 * freeVid + nDOFs) += faceDeriv(9 + 2 * j);
				(*deriv)(2 * freeVid + 1 + nDOFs) += faceDeriv(9 + 2 * j + 1);
				(*deriv)(freeEid + 2 * nFreeVerts + nDOFs) += faceDeriv(15 + j);
			}
		}

		if (hessT)
		{
			for (int j = 0; j < 3; j++)
			{
				int vid = _actualVid2Free[_mesh.faceVertex(fid, j)];
				int eid = _actualEid2Free[_mesh.faceEdge(fid, j)];

				for (int k = 0; k < 3; k++)
				{
					int vid1 = _actualVid2Free[_mesh.faceVertex(fid, k)];
					int eid1 = _actualEid2Free[_mesh.faceEdge(fid, k)];

					for (int m1 = 0; m1 < 2; m1++)
					{
						for (int m2 = 0; m2 < 2; m2++)
						{
							hessT->push_back({ 2 * vid + m1, 2 * vid1 + m2,  faceHess(2 * j + m1, 2 * k + m2) });
							hessT->push_back({ 2 * vid + m1, 2 * vid1 + m2 + nDOFs,  faceHess(2 * j + m1, 9 + 2 * k + m2) });

							hessT->push_back({ 2 * vid + m1 + nDOFs, 2 * vid1 + m2,  faceHess(9 + 2 * j + m1, 2 * k + m2) });
							hessT->push_back({ 2 * vid + m1 + nDOFs, 2 * vid1 + m2 + nDOFs,  faceHess(9 + 2 * j + m1, 9 + 2 * k + m2) });
						}

						hessT->push_back({ 2 * vid + m1, eid1 + 2 * nFreeVerts,  faceHess(2 * j + m1, 6 + k) });
						hessT->push_back({ eid + 2 * nFreeVerts, 2 * vid1 + m1, faceHess(6 + j, 2 * k + m1) });

						hessT->push_back({ 2 * vid + m1, eid1 + 2 * nFreeVerts + nDOFs, faceHess(2 * j + m1, 15 + k) });
						hessT->push_back({ eid + 2 * nFreeVerts + nDOFs, 2 * vid1 + m1, faceHess(15 + j, 2 * k + m1) });

						hessT->push_back({ 2 * vid + m1 + nDOFs, eid1 + 2 * nFreeVerts,  faceHess(9 + 2 * j + m1, 6 + k) });
						hessT->push_back({ eid + 2 * nFreeVerts, 2 * vid1 + m1 + nDOFs, faceHess(6 + j, 9 + 2 * k + m1) });

						hessT->push_back({ 2 * vid + m1 + nDOFs, eid1 + 2 * nFreeVerts + nDOFs,  faceHess(9 + 2 * j + m1, 15 + k) });
						hessT->push_back({ eid + 2 * nFreeVerts + nDOFs, 2 * vid1 + m1 + nDOFs,  faceHess(15 + j, 9 + 2 * k + m1) });

					}
					hessT->push_back({ eid + 2 * nFreeVerts, eid1 + 2 * nFreeVerts, faceHess(6 + j, 6 + k) });
					hessT->push_back({ eid + 2 * nFreeVerts, eid1 + 2 * nFreeVerts + nDOFs, faceHess(6 + j, 15 + k) });
					hessT->push_back({ eid + 2 * nFreeVerts + nDOFs, eid1 + 2 * nFreeVerts, faceHess(15 + j, 6 + k) });
					hessT->push_back({ eid + 2 * nFreeVerts + nDOFs, eid1 + 2 * nFreeVerts + nDOFs, faceHess(15 + j, 15 + k) });
				}


			}
		}
	}

	return energy;
}


double WrinkleEditingStaticEdgeModel::naiveKineticEnergy(int frameId, Eigen::VectorXd* deriv, std::vector<Eigen::Triplet<double>>* hessT, bool isProj)
{
	int nFreeVerts = _freeVids.size();
	double dt = 1. / (_zvalsList.size() - 1);
	double energy = 0;

	int DOFsPerframe = 2 * nFreeVerts;

	if (deriv)
		deriv->setZero(4 * nFreeVerts);

	for (int fv = 0; fv < nFreeVerts; fv++)
	{
		Eigen::Vector2d diff;
		int vid = _freeVids[fv];
		double coeff = 1. / (dt * dt) * _vertArea[vid];
		diff << (_zvalsList[frameId + 1][vid] - _zvalsList[frameId][vid]).real(), (_zvalsList[frameId + 1][vid] - _zvalsList[frameId][vid]).imag();
		energy += 0.5 * coeff * diff.squaredNorm();

		if (deriv)
		{
			deriv->segment<2>(2 * fv) += -coeff * diff;
			deriv->segment<2>(2 * fv + DOFsPerframe) += coeff * diff;
		}

		if (hessT)
		{
			hessT->push_back({ 2 * fv, 2 * fv, coeff });
			hessT->push_back({ DOFsPerframe + 2 * fv, DOFsPerframe + 2 * fv, coeff });

			hessT->push_back({ DOFsPerframe + 2 * fv, 2 * fv, -coeff });
			hessT->push_back({ 2 * fv, DOFsPerframe + 2 * fv, -coeff });

			hessT->push_back({ 2 * fv + 1, 2 * fv + 1, coeff });
			hessT->push_back({ DOFsPerframe + 2 * fv + 1, DOFsPerframe + 2 * fv + 1, coeff });

			hessT->push_back({ 2 * fv + 1, DOFsPerframe + 2 * fv + 1, -coeff });
			hessT->push_back({ DOFsPerframe + 2 * fv + 1, 2 * fv + 1, -coeff });

		}
	}
	return energy;
}

double WrinkleEditingStaticEdgeModel::computeEnergy(const Eigen::VectorXd& x, Eigen::VectorXd* deriv, Eigen::SparseMatrix<double>* hess, bool isProj)
{
	int nFreeVerts = _freeVids.size();
	int nFreeEdges = _freeEids.size();

	int numFrames = _zvalsList.size() - 2;

	int DOFsPerframe = (2 * nFreeVerts + nFreeEdges);

	int DOFs = numFrames * DOFsPerframe;

	convertVariable2List(x);

	Eigen::VectorXd curDeriv;
	std::vector<Eigen::Triplet<double>> T, curT;

	double energy = 0;
	if (deriv)
	{
		deriv->setZero(DOFs);
	}

	std::vector<Eigen::VectorXd> curKDerivList(numFrames + 1);
	std::vector<std::vector<Eigen::Triplet<double>>> curKTList(numFrames + 1);
	std::vector<double> keList(numFrames + 1);

	auto kineticEnergyPerframe = [&](const tbb::blocked_range<uint32_t>& range) 
	{
		for (uint32_t i = range.begin(); i < range.end(); ++i)
		{
			keList[i] = kineticEnergy(i, deriv ? &curKDerivList[i] : NULL, hess ? &curKTList[i] : NULL, isProj);
		}
	};

	tbb::blocked_range<uint32_t> rangex(0u, (uint32_t)numFrames + 1, GRAIN_SIZE);
	tbb::parallel_for(rangex, kineticEnergyPerframe);

	for (int i = 0; i < _zvalsList.size() - 1; i++)
	{
		energy += keList[i];

		if (deriv)
		{
			if (i == 0)
				deriv->segment(0, DOFsPerframe) += curKDerivList[i].segment(DOFsPerframe, DOFsPerframe);
			else if (i == _zvalsList.size() - 2)
				deriv->segment((i - 1) * DOFsPerframe, DOFsPerframe) += curKDerivList[i].segment(0, DOFsPerframe);
			else
			{
				deriv->segment((i - 1) * DOFsPerframe, 2 * DOFsPerframe) += curKDerivList[i];
			}
		}

		if (hess)
		{
			for (auto& it : curKTList[i])
			{

				if (i == 0)
				{
					if (it.row() >= DOFsPerframe && it.col() >= DOFsPerframe)
						T.push_back({ it.row() - DOFsPerframe, it.col() - DOFsPerframe, it.value() });
				}
				else if (i == _zvalsList.size() - 2)
				{
					if (it.row() < DOFsPerframe && it.col() < DOFsPerframe)
						T.push_back({ it.row() + (i - 1) * DOFsPerframe, it.col() + (i - 1) * DOFsPerframe, it.value() });
				}
				else
				{
					T.push_back({ it.row() + (i - 1) * DOFsPerframe, it.col() + (i - 1) * DOFsPerframe, it.value() });
				}


			}
		}
	}

	std::vector<Eigen::VectorXd> ampDerivList(numFrames), omegaDerivList(numFrames), knoppelDerivList(numFrames);
	std::vector<std::vector<Eigen::Triplet<double>>> ampTList(numFrames), omegaTList(numFrames), knoppelTList(numFrames);
	std::vector<double> ampEnergyList(numFrames), omegaEnergyList(numFrames), knoppelEnergyList(numFrames);

	auto otherEnergiesPerframe = [&](const tbb::blocked_range<uint32_t>& range)
	{
		for (uint32_t i = range.begin(); i < range.end(); ++i)
		{
			ampEnergyList[i] = temporalAmpDifference(i + 1, deriv ? &ampDerivList[i] : NULL, hess ? &ampTList[i] : NULL, isProj);
			omegaEnergyList[i] = temporalOmegaDifference(i + 1, deriv ? &omegaDerivList[i] : NULL, hess ? &omegaTList[i] : NULL, isProj);
			knoppelEnergyList[i] = spatialKnoppelEnergy(i + 1, deriv ? &knoppelDerivList[i] : NULL, hess ? &knoppelTList[i] : NULL, isProj);
		}
	};

	tbb::blocked_range<uint32_t> rangex1(0u, (uint32_t)numFrames, GRAIN_SIZE);
	tbb::parallel_for(rangex1, otherEnergiesPerframe);
	

	for (int i = 0; i < numFrames; i++)
	{
		energy += ampEnergyList[i];
		energy += omegaEnergyList[i];
		energy += knoppelEnergyList[i];

		if (deriv) 
		{
			deriv->segment(i * DOFsPerframe, 2 * nFreeVerts) += ampDerivList[i] + knoppelDerivList[i];
			deriv->segment(i * DOFsPerframe + 2 * nFreeVerts, nFreeEdges) += omegaDerivList[i];
		}

		if (hess) 
		{
			for (auto& it : ampTList[i])
			{
				T.push_back({ i * DOFsPerframe + it.row(), i * DOFsPerframe + it.col(), it.value() });
			}
			for (auto& it : knoppelTList[i])
			{
				T.push_back({ i * DOFsPerframe + it.row(), i * DOFsPerframe + it.col(), it.value() });
			}
			for (auto& it : omegaTList[i])
			{
				T.push_back({ i * DOFsPerframe + it.row() + 2 * nFreeVerts, i * DOFsPerframe + it.col() + 2 * nFreeVerts, it.value() });
			}
		}
	}

	if (hess)
	{
		//std::cout << "num of triplets: " << T.size() << std::endl;
		hess->resize(DOFs, DOFs);
		hess->setFromTriplets(T.begin(), T.end());
	}
	return energy;
}

////////////////////////////////////////////// test functions ///////////////////////////////////////////////////////////////////////////
void WrinkleEditingStaticEdgeModel::testCurlFreeEnergy(const Eigen::VectorXd& w)
{
	Eigen::VectorXd deriv;
	std::vector<Eigen::Triplet<double>> T;
	Eigen::SparseMatrix<double> hess;
	double E = curlFreeEnergy(w, &deriv, &T);
	hess.resize(w.rows(), w.rows());
	hess.setFromTriplets(T.begin(), T.end());

	std::cout << "tested curl free energy: " << E << ", gradient norm: " << deriv.norm() << std::endl;

	Eigen::VectorXd dir = deriv;
	dir.setRandom();

	for (int i = 3; i < 10; i++)
	{
		double eps = std::pow(0.1, i);
		Eigen::VectorXd deriv1;
		Eigen::VectorXd w1 = w + eps * dir;

		double E1 = curlFreeEnergy(w1, &deriv1, NULL);

		std::cout << "\neps: " << eps << std::endl;
		std::cout << "gradient check: " << std::abs((E1 - E) / eps - dir.dot(deriv)) << std::endl;
		std::cout << "hess check: " << ((deriv1 - deriv) / eps - hess * dir).norm() << std::endl;
	}
}

void WrinkleEditingStaticEdgeModel::testCurlFreeEnergyPerface(const Eigen::VectorXd& w, int faceId)
{
	Eigen::Matrix<double, 3, 1> deriv;
	Eigen::Matrix<double, 3, 3> hess;
	double E = curlFreeEnergyPerface(w, faceId, &deriv, &hess);
	Eigen::Matrix<double, 3, 1> dir = deriv;
	dir.setRandom();

	std::cout << "tested curl free energy for face: " << faceId << ", energy: " << E << ", gradient norm: " << deriv.norm() << std::endl;

	for (int i = 3; i < 10; i++)
	{
		double eps = std::pow(0.1, i);
		Eigen::VectorXd w1 = w;
		for (int j = 0; j < 3; j++)
		{
			int eid = _mesh.faceEdge(faceId, j);
			w1(eid) += eps * dir(j);
		}
		Eigen::Matrix<double, 3, 1> deriv1;
		double E1 = curlFreeEnergyPerface(w1, faceId, &deriv1, NULL);

		std::cout << "\neps: " << eps << std::endl;
		std::cout << "gradient check: " << std::abs((E1 - E) / eps - dir.dot(deriv)) << std::endl;
		std::cout << "hess check: " << ((deriv1 - deriv) / eps - hess * dir).norm() << std::endl;
	}

}

void WrinkleEditingStaticEdgeModel::testDivFreeEnergy(const Eigen::VectorXd& w)
{
	Eigen::VectorXd deriv;
	std::vector<Eigen::Triplet<double>> T;
	Eigen::SparseMatrix<double> hess;
	double E = divFreeEnergy(w, &deriv, &T);
	hess.resize(w.rows(), w.rows());
	hess.setFromTriplets(T.begin(), T.end());

	std::cout << "tested div free energy: " << E << ", gradient norm: " << deriv.norm() << std::endl;

	Eigen::VectorXd dir = deriv;
	dir.setRandom();

	for (int i = 3; i < 10; i++)
	{
		double eps = std::pow(0.1, i);
		Eigen::VectorXd deriv1;
		Eigen::VectorXd w1 = w + eps * dir;

		double E1 = divFreeEnergy(w1, &deriv1, NULL);

		std::cout << "\neps: " << eps << std::endl;
		std::cout << "gradient check: " << std::abs((E1 - E) / eps - dir.dot(deriv)) << std::endl;
		std::cout << "hess check: " << ((deriv1 - deriv) / eps - hess * dir).norm() << std::endl;
	}
}

void WrinkleEditingStaticEdgeModel::testDivFreeEnergyPervertex(const Eigen::VectorXd& w, int vertId)
{
	Eigen::VectorXd deriv;
	Eigen::MatrixXd hess;
	double E = divFreeEnergyPervertex(w, vertId, &deriv, &hess);
	Eigen::VectorXd dir = deriv;
	dir.setRandom();

	std::cout << "tested div free energy for vertex: " << vertId << ", energy: " << E << ", gradient norm: " << deriv.norm() << std::endl;

	for (int i = 3; i < 10; i++)
	{
		double eps = std::pow(0.1, i);
		Eigen::VectorXd w1 = w;
		for (int j = 0; j < _vertNeiEdges[vertId].size(); j++)
		{
			int eid = _vertNeiEdges[vertId][j];
			w1(eid) += eps * dir(j);
		}
		Eigen::VectorXd deriv1;
		double E1 = divFreeEnergyPervertex(w1, vertId, &deriv1, NULL);

		std::cout << "\neps: " << eps << std::endl;
		std::cout << "gradient check: " << std::abs((E1 - E) / eps - dir.dot(deriv)) << std::endl;
		std::cout << "hess check: " << ((deriv1 - deriv) / eps - hess * dir).norm() << std::endl;
	}

}


void WrinkleEditingStaticEdgeModel::testAmpEnergyWithGivenOmega(const Eigen::VectorXd& amp, const Eigen::VectorXd& w)
{
	Eigen::VectorXd deriv;
	std::vector<Eigen::Triplet<double>> T;
	Eigen::SparseMatrix<double> hess;
	double E = amplitudeEnergyWithGivenOmega(amp, w, &deriv, &T);
	hess.resize(amp.rows(), amp.rows());
	hess.setFromTriplets(T.begin(), T.end());

	std::cout << "tested amp energy: " << E << ", gradient norm: " << deriv.norm() << std::endl;

	Eigen::VectorXd dir = deriv;
	dir.setRandom();


	for (int i = 3; i < 10; i++)
	{
		double eps = std::pow(0.1, i);
		Eigen::VectorXd deriv1;
		Eigen::VectorXd amp1 = amp;
		for (int j = 0; j < amp.rows(); j++)
		{
			amp1(j) += eps * dir(j);
		}
		double E1 = amplitudeEnergyWithGivenOmega(amp1, w, &deriv1, NULL);

		std::cout << "\neps: " << eps << std::endl;
		std::cout << "gradient check: " << std::abs((E1 - E) / eps - dir.dot(deriv)) << std::endl;
		std::cout << "hess check: " << ((deriv1 - deriv) / eps - hess * dir).norm() << std::endl;
	}
}

void WrinkleEditingStaticEdgeModel::testAmpEnergyWithGivenOmegaPerface(const Eigen::VectorXd& amp, const Eigen::VectorXd& w, int faceId)
{
	Eigen::Vector3d deriv;
	Eigen::Matrix3d hess;
	double E = amplitudeEnergyWithGivenOmegaPerface(amp, w, faceId, &deriv, &hess);
	Eigen::Vector3d dir = deriv;
	dir.setRandom();

	std::cout << "tested amp energy for face: " << faceId << ", energy: " << E << ", gradient norm: " << deriv.norm() << std::endl;

	for (int i = 3; i < 10; i++)
	{
		double eps = std::pow(0.1, i);
		Eigen::VectorXd amp1 = amp;
		for (int j = 0; j < 3; j++)
		{
			int vid = _mesh.faceVertex(faceId, j);
			amp1(vid) += eps * dir(j);
		}
		Eigen::Vector3d deriv1;
		double E1 = amplitudeEnergyWithGivenOmegaPerface(amp1, w, faceId, &deriv1, NULL);

		std::cout << "\neps: " << eps << std::endl;
		std::cout << "gradient check: " << std::abs((E1 - E) / eps - dir.dot(deriv)) << std::endl;
		std::cout << "hess check: " << ((deriv1 - deriv) / eps - hess * dir).norm() << std::endl;
	}

}

void WrinkleEditingStaticEdgeModel::testEnergy(Eigen::VectorXd x)
{
	Eigen::VectorXd deriv;
	Eigen::SparseMatrix<double> hess;

	double e = computeEnergy(x, &deriv, &hess, false);
	std::cout << "energy: " << e << std::endl;

	Eigen::VectorXd dir = deriv;
	dir.setRandom();

	for (int i = 3; i < 9; i++)
	{
		double eps = std::pow(0.1, i);

		Eigen::VectorXd deriv1;
		double e1 = computeEnergy(x + eps * dir, &deriv1, NULL, false);

		std::cout << "eps: " << eps << std::endl;
		std::cout << "value-gradient check: " << (e1 - e) / eps - dir.dot(deriv) << std::endl;
		std::cout << "gradient-hessian check: " << ((deriv1 - deriv) / eps - hess * dir).norm() << std::endl;
	}
}