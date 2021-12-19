#include "../../include/IntrinsicFormula/InterpolateZvalsFromEdgeOmega.h"
#include <iostream>


std::complex<double> IntrinsicFormula::getZvalsFromEdgeOmega(const Eigen::Vector3d& bary, const std::vector<std::complex<double>> vertZvals, const Eigen::Vector3d& edgeOmega, Eigen::Matrix<std::complex<double>, 9, 1>* deriv, Eigen::Matrix<std::complex<double>, 9, 9>* hess)
{
	Eigen::Vector3d hatWeight = computeHatWeight(bary(1), bary(2));		// u = alpha1, v = alpha2
	std::complex<double> z = 0;

	if (deriv)
		deriv->setZero();
	if (hess)
		hess->setZero();
	std::complex<double> I = std::complex<double>(0, 1);

	for (int i = 0; i < 3; i++)
	{
		double deltaTheta = bary((i + 1) % 3) * edgeOmega((i + 2) % 3) - bary((i + 2) % 3) * edgeOmega((i + 1) % 3);
		std::complex<double> expip = std::complex<double>(std::cos(deltaTheta), std::sin(deltaTheta));

		z += hatWeight(i) * vertZvals[i] * expip;
		
		if (deriv || hess)
		{
			Eigen::Vector2cd gradZval;
			gradZval << 1, I;

			Eigen::Vector3cd gradexpip;
			gradexpip(i) = 0;
			gradexpip((i + 1) % 3) = -expip * bary((i + 2) % 3) * I;
			gradexpip((i + 2) % 3) = expip * bary((i + 1) % 3) * I;

			if (deriv)
			{
				
				deriv->segment<2>(2 * i) += hatWeight(i) * expip * gradZval;
				deriv->segment<3>(6) += hatWeight(i) * vertZvals[i] * gradexpip;
				
			}

			if (hess)
			{
				Eigen::Matrix3cd hessexpip;
				hessexpip.setZero();
				hessexpip((i + 1) % 3, (i + 1) % 3) = -expip * bary((i + 2) % 3) * bary((i + 2) % 3);
				hessexpip((i + 1) % 3, (i + 2) % 3) = expip * bary((i + 2) % 3) * bary((i + 1) % 3);
				hessexpip((i + 2) % 3, (i + 1) % 3) = expip * bary((i + 2) % 3) * bary((i + 1) % 3);
				hessexpip((i + 2) % 3, (i + 2) % 3) = -expip * bary((i + 1) % 3) * bary((i + 1) % 3);

				hess->block<2, 3>(2 * i, 6) += hatWeight(i) * gradZval * gradexpip.transpose();
				hess->block<3, 2>(6, 2 * i) += hatWeight(i) * gradexpip * gradZval.transpose();
				hess->block<3, 3>(6, 6) += hatWeight(i) * vertZvals[i] * hessexpip;
				
			}
		}
	}

	return z;

}

std::vector<std::complex<double>> IntrinsicFormula::upsamplingZvals(const MeshConnectivity& mesh, const std::vector<std::complex<double>>& zvals, const Eigen::VectorXd& w, const std::vector<std::pair<int, Eigen::Vector3d>>& bary)
{
	int size = bary.size();
	std::vector<std::complex<double>> upzvals(size);
	auto computeZvals = [&](const tbb::blocked_range<uint32_t>& range) {
		for (uint32_t i = range.begin(); i < range.end(); ++i)
		{
			int fid = bary[i].first;
			std::vector<std::complex<double>> vertzvals(3);
			Eigen::Vector3d edgews;

			for (int j = 0; j < 3; j++)
			{
				int vid = mesh.faceVertex(fid, j);
				int eid = mesh.faceEdge(fid, j);

				vertzvals[j] = zvals[vid];
				edgews(j) = w(eid); // defined as mesh.edgeVertex(eid, 1) - mesh.edgeVertex(eid, 0)

				if (mesh.edgeVertex(eid, 1) == mesh.faceVertex(fid, (j + 1) % 3))
					edgews(j) *= -1;
			}

			upzvals[i] = getZvalsFromEdgeOmega(bary[i].second, vertzvals, edgews);
		}
	};

	tbb::blocked_range<uint32_t> rangex(0u, (uint32_t)size, GRAIN_SIZE);
	tbb::parallel_for(rangex, computeZvals);

	return upzvals;
}

void IntrinsicFormula::testZvalsFromEdgeOmega(const Eigen::Vector3d& bary, const std::vector<std::complex<double>> vertZvals, const Eigen::Vector3d& edgeOmega)
{
	std::cout << "edge w: " << edgeOmega.transpose() << std::endl;
	std::complex<double> z;
	Eigen::Matrix<std::complex<double>, 9, 1> deriv;
	Eigen::Matrix<std::complex<double>, 9, 9> hess;

	z = getZvalsFromEdgeOmega(bary, vertZvals, edgeOmega, &deriv, &hess);

	std::cout << "z: \n" << z << std::endl;
	std::cout << "deriv: \n" << deriv << std::endl;
	std::cout << "hess: \n" << hess << std::endl;

	Eigen::Matrix<double, 9, 1> dir;
	dir.setRandom();

	auto backupZvals = vertZvals;
	auto backupw = edgeOmega;

	for (int i = 3; i < 10; i++)
	{
		double eps = std::pow(0.1, i);
		backupw = edgeOmega + eps * dir.segment<3>(6);
		for (int j = 0; j < 3; j++)
		{
			backupZvals[j] = std::complex<double>(vertZvals[j].real() + eps * dir(2 * j), vertZvals[j].imag() + eps * dir(2 * j + 1));
		}

		Eigen::Matrix<std::complex<double>, 9, 1> deriv1;
		std::complex<double> z1 = getZvalsFromEdgeOmega(bary, backupZvals, backupw, &deriv1, NULL);

		std::cout << "eps: " << eps << std::endl;
		std::cout << "f-g: " << (z1 - z) / eps - dir.dot(deriv) << std::endl;
		std::cout << "g-h: " << ((deriv1 - deriv) / eps - hess * dir).norm() << std::endl;

	}
}