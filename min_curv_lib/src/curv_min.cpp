#include <chrono>
#include <iostream>

#include "min_curv_lib/curv_min.hpp"

namespace spline {
namespace optimization {

MinCurvatureOptimizer::MinCurvatureOptimizer(){
    params_ = std::make_unique<MinCurvatureParams>();
    initSolver();
    // Set up the system matrix inverse if it is constant
    if (params_->constant_system_matrix) {
        setSystemMatrixInverse(params_->num_control_points);
    }
}

MinCurvatureOptimizer::MinCurvatureOptimizer(std::unique_ptr<MinCurvatureParams> params) : params_(std::move(params)) {
    initSolver();
    // Set up the system matrix inverse if it is constant
    if (params_->constant_system_matrix) {
        setSystemMatrixInverse(params_->num_control_points);
    }
}

void MinCurvatureOptimizer::initSolver() {
    // Initialize OSQP solver
    solver_ = std::make_unique<OsqpEigen::Solver>();
    solver_->settings()->setVerbosity(params_->verbose);
    solver_->settings()->setMaxIteration(params_->max_num_iterations); 
    solver_->settings()->setWarmStart(params_->warm_start);
}

void MinCurvatureOptimizer::setSplines(const std::shared_ptr<BaseCubicSpline>& ref_spline,
                                       const std::shared_ptr<BaseCubicSpline>& left_spline,
                                       const std::shared_ptr<BaseCubicSpline>& right_spline) {
    ref_spline_ = ref_spline;
    left_spline_ = left_spline;
    right_spline_ = right_spline;
}

void MinCurvatureOptimizer::setUp(const double last_point_shrink) {
    auto start = std::chrono::high_resolution_clock::now();
    setupQP(last_point_shrink);
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    if (params_->verbose) {
        std::cout << "Setup time: " << duration.count() << "ms\n";
    }
}

void MinCurvatureOptimizer::setSystemMatrixInverse(const std::size_t size) {
    const std::size_t size_system = 4 * size;
    Eigen::SparseMatrix<double> system_matrix_sparse(size_system, size_system);
    system_matrix_sparse.insert(0, 0) = 1.;
    system_matrix_sparse.insert(1, 2) = 2.;
    system_matrix_sparse.insert(2, 0) = 1.;
    system_matrix_sparse.insert(2, 1) = 1.;
    system_matrix_sparse.insert(2, 2) = 1.;
    system_matrix_sparse.insert(2, 3) = 1.;
    system_matrix_sparse.insert(3, 1) = 1.;
    system_matrix_sparse.insert(3, 2) = 2.;
    system_matrix_sparse.insert(3, 3) = 3.;
    system_matrix_sparse.insert(3, 5) = -1.;
    system_matrix_sparse.insert(4, 2) = 1.;
    system_matrix_sparse.insert(4, 3) = 3.;
    system_matrix_sparse.insert(4, 6) = -1.;
    system_matrix_sparse.insert(size_system - 3, size_system - 4) = 1;
    system_matrix_sparse.insert(size_system - 2, size_system - 2) = 2;
    system_matrix_sparse.insert(size_system - 1, size_system - 1) = 1;
    for (std::size_t i = 1; i < size - 1; ++i) {
        system_matrix_sparse.insert(4*i+1, 4*i) = 1.;
        system_matrix_sparse.insert(4*i+2, 4*i) = 1.;
        system_matrix_sparse.insert(4*i+2, 4*i+1) = 1.;
        system_matrix_sparse.insert(4*i+2, 4*i+2) = 1.;
        system_matrix_sparse.insert(4*i+2, 4*i+3) = 1.;
        system_matrix_sparse.insert(4*i+3, 4*i+1) = 1.;
        system_matrix_sparse.insert(4*i+3, 4*i+2) = 2.;
        system_matrix_sparse.insert(4*i+3, 4*i+3) = 3.;
        system_matrix_sparse.insert(4*i+3, 4*i+5) = -1.;
        system_matrix_sparse.insert(4*i+4, 4*i+2) = 1.;
        system_matrix_sparse.insert(4*i+4, 4*i+3) = 3.;
        system_matrix_sparse.insert(4*i+4, 4*i+6) = -1.;
    }

    Eigen::SparseLU<Eigen::SparseMatrix<double>> solver;
    solver.analyzePattern(system_matrix_sparse);  // Analyze the sparsity pattern
    solver.factorize(system_matrix_sparse);       // Factorize the matrix
    // Now solve for the inverse
    Eigen::SparseMatrix<double> identity(size_system, size_system);
    identity.setIdentity();  // Create an identity matrix of size NxN
    // Solve for the inverse by treating it as a linear system
    Eigen::SparseMatrix<double> A_inv_sparse = solver.solve(identity);
    system_inverse_ = fromSparseMatrix(A_inv_sparse);
}

void MinCurvatureOptimizer::computeHessianAndLinear() {
    // Get normal vectors from coefficients 
    // Normal vector is the derivative of the spline, wich are coefficients b
    const std::size_t num_control_points = ref_spline_->size();
    const auto coefficients = ref_spline_->getCoefficients();
    normal_vectors_.resize(num_control_points, 2);
    normal_vectors_.col(0) = -coefficients.second.row(1);
    normal_vectors_.col(1) = coefficients.first.row(1);

    // Normalization of normal vectors
    normal_vectors_.rowwise().normalize();

    // Calculate A matrix (later updated in for loop)
    const std::size_t size_A = 4 * num_control_points;

    // Compute P_xx, P_xy, P_yy
    Eigen::VectorXd square_normals = (normal_vectors_.col(0).array().square() + normal_vectors_.col(1).array().square());
    Eigen::MatrixXd P_xx = (normal_vectors_.col(0).array().square().array() / square_normals.array()).matrix().asDiagonal();
    Eigen::MatrixXd P_yy = (normal_vectors_.col(1).array().square().array() / square_normals.array()).matrix().asDiagonal();
    Eigen::MatrixXd P_xy = ((2 * normal_vectors_.col(1).array() * normal_vectors_.col(0).array()).array() / square_normals.array()).matrix().asDiagonal();

    // Compute q_x, q_y, M_x, M_y and extraction matrix A_ex
    Eigen::VectorXd q_x = Eigen::VectorXd::Zero(size_A);
    Eigen::VectorXd q_y = Eigen::VectorXd::Zero(size_A);
    Eigen::MatrixXd M_x = Eigen::MatrixXd::Zero(size_A, num_control_points);
    Eigen::MatrixXd M_y = Eigen::MatrixXd::Zero(size_A, num_control_points);
    Eigen::MatrixXd A_ex = Eigen::MatrixXd::Zero(num_control_points, size_A);

    const auto& control_points = ref_spline_->getControlPoints();
    q_x(0) = control_points[0].x();
    q_x(2) = control_points[1].x();
    q_y(0) = control_points[0].y();
    q_y(2) = control_points[1].y();
    M_x(0, 0) = normal_vectors_(0, 0);
    M_x(2, 1) = normal_vectors_(1, 0);
    M_y(0, 0) = normal_vectors_(0, 1);
    M_y(2, 1) = normal_vectors_(1, 1);
    A_ex(0, 2) = 1;

    for (std::size_t i = 1; i < num_control_points - 1; ++i) {
        q_x(4 * i + 1) = control_points[i].x();
        q_x(4 * i + 2) = control_points[i + 1].x();
        q_y(4 * i + 1) = control_points[i].y();
        q_y(4 * i + 2) = control_points[i + 1].y();
        M_x(4 * i + 1, i) = normal_vectors_(i, 0);
        M_x(4 * i + 2, i + 1) = normal_vectors_(i + 1, 0);
        M_y(4 * i + 1, i) = normal_vectors_(i, 1);
        M_y(4 * i + 2, i + 1) = normal_vectors_(i + 1, 1);
        A_ex(i, 4 * i + 2) = 1;
    }
    q_x(size_A - 3) = control_points[num_control_points - 1].x();
    q_y(size_A - 3) = control_points[num_control_points - 1].y();
    M_x(size_A - 3, num_control_points - 1) = normal_vectors_(num_control_points - 1, 0);
    M_y(size_A - 3, num_control_points - 1) = normal_vectors_(num_control_points - 1, 1);
    A_ex(num_control_points - 1, size_A - 2) = 1;

    if (!params_->constant_system_matrix) {
        setSystemMatrixInverse(num_control_points);
    }
    Eigen::MatrixXd T_c = 2 * A_ex * system_inverse_;
    Eigen::MatrixXd T_nx = T_c * M_x;
    Eigen::MatrixXd T_ny = T_c * M_y;
    Eigen::MatrixXd tmp = T_nx.adjoint() * P_xx * T_nx + T_ny.adjoint() * P_xy * T_nx + T_ny.adjoint() * P_yy * T_ny;
    c_ = 2 * T_nx.adjoint() * P_xx.adjoint() * T_c * q_x + T_ny.adjoint() * P_xy.adjoint() * T_c * q_x + 
         2 * T_ny.adjoint() * P_yy.adjoint() * T_c * q_y + T_nx.adjoint() * P_xy.adjoint() * T_c * q_y;
    H_ = (tmp.adjoint() + tmp) / 2;
}

const Eigen::MatrixXd MinCurvatureOptimizer::getBoundaryDistance() const {
    const std::size_t num_control_points = ref_spline_->size();
    const std::size_t num_points_evaluate = params_->num_points_evaluate;

    Eigen::MatrixXd distance(num_control_points, 2);

    // Precompute left and right spline points
    std::vector<Eigen::Vector2d> left_points(num_points_evaluate);
    std::vector<Eigen::Vector2d> right_points(num_points_evaluate);
    
    for (std::size_t i = 0; i < num_points_evaluate; ++i) {
        const double u = static_cast<double>(i) / (num_points_evaluate - 1);
        left_points[i] = left_spline_->evaluateSpline(u, 0);
        right_points[i] = right_spline_->evaluateSpline(u, 0);
    }

    // Build k-d trees for left and right points
    KDTreeAdapter left_cloud{left_points};
    KDTreeAdapter right_cloud{right_points};

    using KDTree = nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<double, KDTreeAdapter>, KDTreeAdapter, 2>;
    KDTree left_tree(2, left_cloud, nanoflann::KDTreeSingleIndexAdaptorParams(params_->kdtree_leafs));
    KDTree right_tree(2, right_cloud, nanoflann::KDTreeSingleIndexAdaptorParams(params_->kdtree_leafs));

    left_tree.buildIndex();
    right_tree.buildIndex();

    // Query the nearest neighbors from each control point
    std::vector<unsigned int> nearest_indices(params_->num_nearest);
    std::vector<double> nearest_distances_sq(params_->num_nearest);

    for (std::size_t i = 0; i < num_control_points; ++i) {
        const auto& control_point = ref_spline_->getControlPoints()[i];
        const auto& normal_vector = normal_vectors_.row(i);
        
        // Precompute line coefficients and normalize them
        const double a_line = -normal_vector(1);
        const double b_line = normal_vector(0);
        const double norm_factor = std::sqrt(a_line * a_line + b_line * b_line);
        const double c_line = -a_line * control_point.x() - b_line * control_point.y();

        // Query the 3 nearest left points
        const double query_point[2] = { control_point.x(), control_point.y() };
        left_tree.knnSearch(&query_point[0], params_->num_nearest, nearest_indices.data(), nearest_distances_sq.data());

        // Compute distances to the 3 nearest left points
        double min_plane2point_dist_left = std::numeric_limits<double>::max();
        double min_distance_left = std::numeric_limits<double>::max();
        for (std::size_t j = 0; j < params_->num_nearest; ++j) {
            const auto& nearest_left_point = left_points[nearest_indices[j]];
            double plane2point_distance_left = std::abs(a_line * nearest_left_point.x() + b_line * nearest_left_point.y() + c_line) / norm_factor;
            if (plane2point_distance_left < min_plane2point_dist_left) {
                min_plane2point_dist_left = plane2point_distance_left;
                min_distance_left = (nearest_left_point - control_point).norm();
            }
        }

        // Query the 3 nearest right points
        right_tree.knnSearch(&query_point[0], params_->num_nearest, nearest_indices.data(), nearest_distances_sq.data());

        // Compute distances to the 3 nearest right points
        double min_plane2point_dist_right = std::numeric_limits<double>::max();
        double min_distance_right = std::numeric_limits<double>::max();
        for (std::size_t j = 0; j < params_->num_nearest; ++j) {
            const auto& nearest_right_point = right_points[nearest_indices[j]];
            double plane2point_distance_right = std::abs(a_line * nearest_right_point.x() + b_line * nearest_right_point.y() + c_line) / norm_factor;
            if (plane2point_distance_right < min_plane2point_dist_right) {
                min_plane2point_dist_right = plane2point_distance_right;
                min_distance_right = (nearest_right_point - control_point).norm();
            }
        }

        // Set the minimum distances for the current control point
        distance(i, 0) = std::max(0.0, min_distance_left - params_->shrink);
        distance(i, 1) = std::max(0.0, min_distance_right - params_->shrink);
    }
    return distance;
}

void MinCurvatureOptimizer::computeConstraints(const double last_point_shrink) {
    std::size_t num_control_points = ref_spline_->size();
    const auto distance = getBoundaryDistance();
    lower_bound_ = -distance.col(1);
    upper_bound_ = distance.col(0);
    A_ = Eigen::MatrixXd::Identity(ref_spline_->size(), ref_spline_->size());
    // Set the first control point to be fixed (i.e. no moving along the normal vector)
    lower_bound_(0) = 0.0;
    upper_bound_(0) = 0.0;
    // Set the last control point to have a smaller range
    lower_bound_(num_control_points - 1) = last_point_shrink * lower_bound_(num_control_points - 1);
    upper_bound_(num_control_points - 1) = last_point_shrink * upper_bound_(num_control_points - 1);
}

void MinCurvatureOptimizer::setupQP(const double last_point_shrink) {
    // Assert that last_point_shrink is in the range [0, 1]
    assert(last_point_shrink >= 0.0 && last_point_shrink <= 1.0);
    solver_->clearSolver();
    solver_->data()->clearHessianMatrix();
    solver_->data()->clearLinearConstraintsMatrix();
    computeHessianAndLinear();
    computeConstraints(last_point_shrink);
    
    // Configure OSQP solver
    std::size_t num_control_points = ref_spline_->size();
    solver_->data()->setNumberOfVariables(num_control_points);
    solver_->data()->setNumberOfConstraints(num_control_points);
    solver_->data()->setHessianMatrix(toSparseMatrix(H_));
    solver_->data()->setGradient(c_);
    solver_->data()->setLinearConstraintsMatrix(toSparseMatrix(A_));
    solver_->data()->setLowerBound(lower_bound_);
    solver_->data()->setUpperBound(upper_bound_);
}

const Eigen::SparseMatrix<double> MinCurvatureOptimizer::toSparseMatrix(const Eigen::MatrixXd& matrix) const {
    Eigen::SparseMatrix<double> sparse_matrix(matrix.rows(), matrix.cols());
    for (int i = 0; i < matrix.outerSize(); ++i) {
        for (Eigen::MatrixXd::InnerIterator it(matrix, i); it; ++it) {
            sparse_matrix.insert(it.row(), it.col()) = it.value();
        }
    }
    sparse_matrix.makeCompressed();
    return sparse_matrix;
}

const Eigen::MatrixXd MinCurvatureOptimizer::fromSparseMatrix(const Eigen::SparseMatrix<double>& sparse_matrix) const {
    return Eigen::MatrixXd(sparse_matrix);
}

void MinCurvatureOptimizer::solve(std::shared_ptr<BaseCubicSpline>& opt_traj, const double normal_weight) {
    // Solve the QP problem
    auto start = std::chrono::high_resolution_clock::now();
    solver_->initSolver();
    solver_->solveProblem();
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    if (params_->verbose) {
        std::cout << "Solving time: " << duration.count() << "us\n";
    }
    
    // Retrieve the solution (optimized control points)
    Eigen::VectorXd solution = normal_weight * solver_->getSolution();
    
    // Extract optimized control points (2D points for x and y)
    std::vector<Eigen::Vector2d> optimized_control_points(ref_spline_->size());
    const auto& control_points = ref_spline_->getControlPoints();
    for (std::size_t i = 0; i < ref_spline_->size(); ++i) {
        optimized_control_points[i].x() = control_points[i].x() + solution(i) * normal_vectors_(i, 0);
        optimized_control_points[i].y() = control_points[i].y() + solution(i) * normal_vectors_(i, 1);
    }
    opt_traj->setControlPoints(optimized_control_points);
}
 
} // namespace optimization
} // namespace spline
