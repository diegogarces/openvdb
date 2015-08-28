///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2012-2015 DreamWorks Animation LLC
//
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
//
// Redistributions of source code must retain the above copyright
// and license notice and the following restrictions and disclaimer.
//
// *     Neither the name of DreamWorks Animation nor the names of
// its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// IN NO EVENT SHALL THE COPYRIGHT HOLDERS' AND CONTRIBUTORS' AGGREGATE
// LIABILITY FOR ALL CLAIMS REGARDLESS OF THEIR BASIS EXCEED US$250.00.
//
///////////////////////////////////////////////////////////////////////////
//
/// @file PoissonSolver.h
///
/// @authors D.J. Hill, Peter Cucka
///
/// @brief Solve Poisson's equation &nabla;<sup><small>2</small></sup><i>x</i> = <i>b</i>
/// for <i>x</i>, where @e b is a vector comprising the values of all of the active voxels
/// in a grid.
///
/// @par Example:
/// Solve for the pressure in a cubic tank of liquid, assuming uniform boundary conditions:
/// @code
/// FloatTree source(/*background=*/0.0f);
/// // Activate voxels to indicate that they contain liquid.
/// source.fill(CoordBBox(Coord(0, -10, 0), Coord(10, 0, 10)), /*value=*/0.0f);
///
/// math::pcg::State state = math::pcg::terminationDefaults<float>();
/// FloatTree::Ptr solution = tools::poisson::solve(source, state);
/// @endcode
///
/// @par Example:
/// Solve for the pressure, <i>P</i>, in a cubic tank of liquid that is open at the top.
/// Boundary conditions are <i>P</i>&nbsp;=&nbsp;0 at the top,
/// &part;<i>P</i>/&part;<i>y</i>&nbsp;=&nbsp;&minus;1 at the bottom
/// and &part;<i>P</i>/&part;<i>x</i>&nbsp;=&nbsp;0 at the sides:
/// <pre>
///                P = 0
///             +--------+ (N,0,N)
///            /|       /|
///   (0,0,0) +--------+ |
///           | |      | | dP/dx = 0
/// dP/dx = 0 | +------|-+
///           |/       |/
///  (0,-N,0) +--------+ (N,-N,N)
///           dP/dy = -1
/// </pre>
/// @code
/// const int N = 10;
/// DoubleTree source(/*background=*/0.0);
/// // Activate voxels to indicate that they contain liquid.
/// source.fill(CoordBBox(Coord(0, -N, 0), Coord(N, 0, N)), /*value=*/0.0);
///
/// // C++11
/// auto boundary = [](const openvdb::Coord& ijk, const openvdb::Coord& neighbor,
///     double& source, double& diagonal)
/// {
///     if (neighbor.x() == ijk.x() && neighbor.z() == ijk.z()) {
///         if (neighbor.y() < ijk.y()) source -= 1.0;
///         else diagonal -= 1.0;
///     }
/// };
///
/// math::pcg::State state = math::pcg::terminationDefaults<double>();
/// util::NullInterrupter interrupter;
///
/// DoubleTree::Ptr solution = tools::poisson::solveWithBoundaryConditions(
///     source, boundary, state, interrupter);
/// @endcode

#ifndef OPENVDB_TOOLS_POISSONSOLVER_HAS_BEEN_INCLUDED
#define OPENVDB_TOOLS_POISSONSOLVER_HAS_BEEN_INCLUDED

#include <openvdb/Types.h>
#include <openvdb/math/ConjGradient.h>
#include <openvdb/tree/LeafManager.h>
#include <openvdb/tree/Tree.h>
#include <openvdb/util/NullInterrupter.h>

#include "Morphology.h" // for erodeVoxels

#include <boost/scoped_array.hpp>


namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace tools {
namespace poisson {

// This type should be at least as wide as math::pcg::SizeType.
typedef Int32 VIndex;

/// The type of a matrix used to represent a three-dimensional Laplacian operator
typedef math::pcg::SparseStencilMatrix<double, 7> LaplacianMatrix;


//@{
/// @brief Solve &nabla;<sup><small>2</small></sup><i>x</i> = <i>b</i> for <i>x</i>,
/// where @e b is a vector comprising the values of all of the active voxels
/// in the input tree.
/// @return a new tree, with the same active voxel topology as the input tree,
/// whose voxel values are the elements of the solution vector <i>x</i>.
/// @details On input, the State object should specify convergence criteria
/// (minimum error and maximum number of iterations); on output, it gives
/// the actual termination conditions.
/// @details The solution is computed using the conjugate gradient method
/// with (where possible) incomplete Cholesky preconditioning, falling back
/// to Jacobi preconditioning.
/// @sa solveWithBoundaryConditions
template<typename TreeType>
inline typename TreeType::Ptr
solve(const TreeType&, math::pcg::State&);

template<typename TreeType, typename Interrupter>
inline typename TreeType::Ptr
solve(const TreeType&, math::pcg::State&, Interrupter&);
//@}


//@{
/// @brief Solve &nabla;<sup><small>2</small></sup><i>x</i> = <i>b</i> for <i>x</i>
/// with user-specified boundary conditions, where @e b is a vector comprising
/// the values of all of the active voxels in the input tree or domain mask if provided
/// @return a new tree, with the same active voxel topology as the input tree,
/// whose voxel values are the elements of the solution vector <i>x</i>.
/// @details On input, the State object should specify convergence criteria
/// (minimum error and maximum number of iterations); on output, it gives
/// the actual termination conditions.
/// @details The solution is computed using the conjugate gradient method with
/// the specified type of preconditioner (default: incomplete Cholesky),
/// falling back to Jacobi preconditioning if necessary.
/// @details Each thread gets its own copy of the BoundaryOp, which should be
/// a functor of the form
/// @code
/// struct BoundaryOp {
///     typedef LaplacianMatrix::ValueType ValueType;
///     void operator()(
///         const Coord& ijk,          // coordinates of a boundary voxel
///         const Coord& ijkNeighbor,  // coordinates of an exterior neighbor of ijk
///         ValueType& source,         // element of b corresponding to ijk
///         ValueType& diagonal        // element of Laplacian matrix corresponding to ijk
///     ) const;
/// };
/// @endcode
/// The functor is called for each of the exterior neighbors of each boundary voxel @ijk,
/// and it must specify a boundary condition for @ijk by modifying one or both of two
/// provided values: the entry in the source vector @e b corresponding to @ijk and
/// the weighting coefficient for @ijk in the Laplacian operator matrix.
///
/// @sa solve
template<typename TreeType, typename BoundaryOp, typename Interrupter>
inline typename TreeType::Ptr
solveWithBoundaryConditions(const TreeType&, const BoundaryOp&, math::pcg::State&, Interrupter&);

template<typename PreconditionerType, typename TreeType, typename BoundaryOp, typename Interrupter>
inline typename TreeType::Ptr
solveWithBoundaryConditionsAndPreconditioner(const TreeType&, const BoundaryOp&,
                                             math::pcg::State&, Interrupter&);

template<typename PreconditionerType, typename TreeType, typename DomainTreeType, typename BoundaryOp, typename Interrupter>
inline typename TreeType::Ptr
solveWithBoundaryConditionsAndPreconditioner(const TreeType&, const DomainTreeType&, const BoundaryOp&,
                                             math::pcg::State&, Interrupter&);
//@}


/// @name Low-level functions
//@{
// The following are low-level routines that can be used to assemble custom solvers.

/// @brief Overwrite each active voxel in the given scalar tree
/// with a sequential index, starting from zero.
template<typename VIndexTreeType>
inline void populateIndexTree(VIndexTreeType&);

/// @brief Iterate over the active voxels of the input tree and for each one
/// assign its index in the iteration sequence to the corresponding voxel
/// of an integer-valued output tree.
template<typename TreeType>
inline typename TreeType::template ValueConverter<VIndex>::Type::Ptr
createIndexTree(const TreeType&);


/// @brief Return a vector of the active voxel values of the scalar-valued @a source tree.
/// @details The <i>n</i>th element of the vector corresponds to the voxel whose value
/// in the @a index tree is @e n.
/// @param source  a tree with a scalar value type
/// @param index   a tree of the same configuration as @a source but with
///     value type VIndex that maps voxels to elements of the output vector
template<typename VectorValueType, typename SourceTreeType>
inline typename math::pcg::Vector<VectorValueType>::Ptr
createVectorFromTree(
    const SourceTreeType& source,
    const typename SourceTreeType::template ValueConverter<VIndex>::Type& index);


/// @brief Return a tree with the same active voxel topology as the @a index tree
/// but whose voxel values are taken from the the given vector.
/// @details The voxel whose value in the @a index tree is @e n gets assigned
/// the <i>n</i>th element of the vector.
/// @param index   a tree with value type VIndex that maps voxels to elements of @a values
/// @param values  a vector of values with which to populate the active voxels of the output tree
/// @param background  the value for the inactive voxels of the output tree
template<typename TreeValueType, typename VIndexTreeType, typename VectorValueType>
inline typename VIndexTreeType::template ValueConverter<TreeValueType>::Type::Ptr
createTreeFromVector(
    const math::pcg::Vector<VectorValueType>& values,
    const VIndexTreeType& index,
    const TreeValueType& background);


/// @brief Generate a sparse matrix of the index-space (&Delta;<i>x</i> = 1) Laplacian operator
/// using second-order finite differences.
/// @details This construction assumes homogeneous Dirichlet boundary conditions
/// (exterior grid points are zero).
template<typename BoolTreeType>
inline LaplacianMatrix::Ptr
createISLaplacian(
    const typename BoolTreeType::template ValueConverter<VIndex>::Type& vectorIndexTree,
    const BoolTreeType& interiorMask);


/// @brief Generate a sparse matrix of the index-space (&Delta;<i>x</i> = 1) Laplacian operator
/// with user-specified boundary conditions using second-order finite differences.
/// @details Each thread gets its own copy of @a boundaryOp, which should be
/// a functor of the form
/// @code
/// struct BoundaryOp {
///     typedef LaplacianMatrix::ValueType ValueType;
///     void operator()(
///         const Coord& ijk,          // coordinates of a boundary voxel
///         const Coord& ijkNeighbor,  // coordinates of an exterior neighbor of ijk
///         ValueType& source,         // element of source vector corresponding to ijk
///         ValueType& diagonal        // element of Laplacian matrix corresponding to ijk
///     ) const;
/// };
/// @endcode
/// The functor is called for each of the exterior neighbors of each boundary voxel @ijk,
/// and it must specify a boundary condition for @ijk by modifying one or both of two
/// provided values: an entry in the given @a source vector corresponding to @ijk and
/// the weighting coefficient for @ijk in the Laplacian matrix.
template<typename BoolTreeType, typename BoundaryOp>
inline LaplacianMatrix::Ptr
createISLaplacianWithBoundaryConditions(
    const typename BoolTreeType::template ValueConverter<VIndex>::Type& vectorIndexTree,
    const BoolTreeType& interiorMask,
    const BoundaryOp& boundaryOp,
    typename math::pcg::Vector<LaplacianMatrix::ValueType>& source);

//@}


////////////////////////////////////////


namespace internal {

/// @brief Functor for use with LeafManager::foreach() to populate an array
/// with per-leaf active voxel counts
template<typename LeafType>
struct LeafCountOp
{
    VIndex* count;
    LeafCountOp(VIndex* count_): count(count_) {}
    void operator()(const LeafType& leaf, size_t leafIdx) const {
        count[leafIdx] = static_cast<VIndex>(leaf.onVoxelCount());
    }
};


/// @brief Functor for use with LeafManager::foreach() to populate
/// active leaf voxels with sequential indices
template<typename LeafType>
struct LeafIndexOp
{
    const VIndex* count;
    LeafIndexOp(const VIndex* count_): count(count_) {}
    void operator()(LeafType& leaf, size_t leafIdx) const {
        VIndex idx = (leafIdx == 0) ? 0 : count[leafIdx - 1];
        for (typename LeafType::ValueOnIter it = leaf.beginValueOn(); it; ++it) {
            it.setValue(idx++);
        }
    }
};

} // namespace internal


template<typename VIndexTreeType>
inline void
populateIndexTree(VIndexTreeType& result)
{
    typedef typename VIndexTreeType::LeafNodeType       LeafT;
    typedef typename tree::LeafManager<VIndexTreeType>  LeafMgrT;

    // Linearize the tree.
    LeafMgrT leafManager(result);
    const size_t leafCount = leafManager.leafCount();

    // Count the number of active voxels in each leaf node.
    boost::scoped_array<VIndex> perLeafCount(new VIndex[leafCount]);
    VIndex* perLeafCountPtr = perLeafCount.get();
    leafManager.foreach(internal::LeafCountOp<LeafT>(perLeafCountPtr));

    // The starting index for each leaf node is the total number
    // of active voxels in all preceding leaf nodes.
    for (size_t i = 1; i < leafCount; ++i) {
        perLeafCount[i] += perLeafCount[i - 1];
    }

    // The last accumulated value should be the total of all active voxels.
    assert(Index64(perLeafCount[leafCount-1]) == result.activeVoxelCount());

    // Parallelize over the leaf nodes of the tree, storing a unique index
    // in each active voxel.
    leafManager.foreach(internal::LeafIndexOp<LeafT>(perLeafCountPtr));
}


template<typename TreeType>
inline typename TreeType::template ValueConverter<VIndex>::Type::Ptr
createIndexTree(const TreeType& tree)
{
    typedef typename TreeType::template ValueConverter<VIndex>::Type VIdxTreeT;

    // Construct an output tree with the same active voxel topology as the input tree.
    const VIndex invalidIdx = -1;
    typename VIdxTreeT::Ptr result(
        new VIdxTreeT(tree, /*background=*/invalidIdx, TopologyCopy()));

    // All active voxels are degrees of freedom, including voxels contained in active tiles.
    result->voxelizeActiveTiles();

    populateIndexTree(*result);

    return result;
}


////////////////////////////////////////


namespace internal {

/// @brief Functor for use with LeafManager::foreach() to populate a vector
/// with the values of a tree's active voxels
template<typename VectorValueType, typename SourceTreeType>
struct CopyToVecOp
{
    typedef typename SourceTreeType::template ValueConverter<VIndex>::Type VIdxTreeT;
    typedef typename VIdxTreeT::LeafNodeType             VIdxLeafT;
    typedef typename SourceTreeType::LeafNodeType        LeafT;
    typedef typename SourceTreeType::ValueType           TreeValueT;
    typedef typename math::pcg::Vector<VectorValueType>  VectorT;

    const SourceTreeType* tree;
    VectorT* vector;

    CopyToVecOp(const SourceTreeType& t, VectorT& v): tree(&t), vector(&v) {}

    void operator()(const VIdxLeafT& idxLeaf, size_t /*leafIdx*/) const
    {
        VectorT& vec = *vector;
        if (const LeafT* leaf = tree->probeLeaf(idxLeaf.origin())) {
            // If a corresponding leaf node exists in the source tree,
            // copy voxel values from the source node to the output vector.
            for (typename VIdxLeafT::ValueOnCIter it = idxLeaf.cbeginValueOn(); it; ++it) {
                vec[*it] = leaf->getValue(it.pos());
            }
        } else {
            // If no corresponding leaf exists in the source tree,
            // fill the vector with a uniform value.
            const TreeValueT& value = tree->getValue(idxLeaf.origin());
            for (typename VIdxLeafT::ValueOnCIter it = idxLeaf.cbeginValueOn(); it; ++it) {
                vec[*it] = value;
            }
        }
    }
};

} // namespace internal


template<typename VectorValueType, typename SourceTreeType>
inline typename math::pcg::Vector<VectorValueType>::Ptr
createVectorFromTree(const SourceTreeType& tree,
    const typename SourceTreeType::template ValueConverter<VIndex>::Type& idxTree)
{
    typedef typename SourceTreeType::template ValueConverter<VIndex>::Type VIdxTreeT;
    typedef tree::LeafManager<const VIdxTreeT>           VIdxLeafMgrT;
    typedef typename SourceTreeType::ValueType           ValueT;
    typedef typename math::pcg::Vector<VectorValueType>  VectorT;

    // Allocate the vector.
    const size_t numVoxels = idxTree.activeVoxelCount();
    typename VectorT::Ptr result(new VectorT(static_cast<math::pcg::SizeType>(numVoxels)));

    // Parallelize over the leaf nodes of the index tree, filling the output vector
    // with values from corresponding voxels of the source tree.
    VIdxLeafMgrT leafManager(idxTree);
    leafManager.foreach(internal::CopyToVecOp<VectorValueType, SourceTreeType>(tree, *result));

    return result;
}


////////////////////////////////////////


namespace internal {

/// @brief Functor for use with LeafManager::foreach() to populate a tree
/// with values from a vector
template<typename TreeValueType, typename VIndexTreeType, typename VectorValueType>
struct CopyFromVecOp
{
    typedef typename VIndexTreeType::template ValueConverter<TreeValueType>::Type OutTreeT;
    typedef typename OutTreeT::LeafNodeType              OutLeafT;
    typedef typename VIndexTreeType::LeafNodeType        VIdxLeafT;
    typedef typename math::pcg::Vector<VectorValueType>  VectorT;

    const VectorT* vector;
    OutTreeT* tree;

    CopyFromVecOp(const VectorT& v, OutTreeT& t): vector(&v), tree(&t) {}

    void operator()(const VIdxLeafT& idxLeaf, size_t /*leafIdx*/) const
    {
        const VectorT& vec = *vector;
        OutLeafT* leaf = tree->probeLeaf(idxLeaf.origin());
        assert(leaf != NULL);
        for (typename VIdxLeafT::ValueOnCIter it = idxLeaf.cbeginValueOn(); it; ++it) {
            leaf->setValueOnly(it.pos(), static_cast<TreeValueType>(vec[*it]));
        }
    }
};

} // namespace internal


template<typename TreeValueType, typename VIndexTreeType, typename VectorValueType>
inline typename VIndexTreeType::template ValueConverter<TreeValueType>::Type::Ptr
createTreeFromVector(
    const math::pcg::Vector<VectorValueType>& vector,
    const VIndexTreeType& idxTree,
    const TreeValueType& background)
{
    typedef typename VIndexTreeType::template ValueConverter<TreeValueType>::Type OutTreeT;
    typedef typename tree::LeafManager<const VIndexTreeType> VIdxLeafMgrT;

    // Construct an output tree with the same active voxel topology as the index tree.
    typename OutTreeT::Ptr result(new OutTreeT(idxTree, background, TopologyCopy()));
    OutTreeT& tree = *result;

    // Parallelize over the leaf nodes of the index tree, populating voxels
    // of the output tree with values from the input vector.
    VIdxLeafMgrT leafManager(idxTree);
    leafManager.foreach(
        internal::CopyFromVecOp<TreeValueType, VIndexTreeType, VectorValueType>(vector, tree));

    return result;
}


////////////////////////////////////////


namespace internal {

/// Constant boundary condition functor
template<typename ValueType>
struct DirichletOp {
    inline void operator()(
        const Coord&, const Coord&, ValueType&, ValueType& diag) const { diag -= 1; }
};


/// Functor for use with LeafManager::foreach() to populate a sparse Laplacian matrix
template<typename BoolTreeType, typename BoundaryOp>
struct ISLaplacianOp
{
    typedef typename BoolTreeType::template ValueConverter<VIndex>::Type VIdxTreeT;
    typedef typename VIdxTreeT::LeafNodeType   VIdxLeafT;
    typedef LaplacianMatrix::ValueType         ValueT;
    typedef typename math::pcg::Vector<ValueT> VectorT;

    LaplacianMatrix* laplacian;
    const VIdxTreeT* idxTree;
    const BoolTreeType* interiorMask;
    const BoundaryOp boundaryOp;
    VectorT* source;

    ISLaplacianOp(LaplacianMatrix& m, const VIdxTreeT& idx,
        const BoolTreeType& mask, const BoundaryOp& op, VectorT& src):
        laplacian(&m), idxTree(&idx), interiorMask(&mask), boundaryOp(op), source(&src) {}

    void operator()(const VIdxLeafT& idxLeaf, size_t /*leafIdx*/) const
    {
        // Local accessors
        typename tree::ValueAccessor<const BoolTreeType> interior(*interiorMask);
        typename tree::ValueAccessor<const VIdxTreeT> vectorIdx(*idxTree);

        Coord ijk;
        VIndex column;
        const ValueT diagonal = -6.f, offDiagonal = 1.f;

        // Loop over active voxels in this leaf.
        for (typename VIdxLeafT::ValueOnCIter it = idxLeaf.cbeginValueOn(); it; ++it) {
            assert(it.getValue() > -1);
            const math::pcg::SizeType rowNum = static_cast<math::pcg::SizeType>(it.getValue());

            LaplacianMatrix::RowEditor row = laplacian->getRowEditor(rowNum);

            ijk = it.getCoord();
            if (interior.isValueOn(ijk)) {
                // The current voxel is an interior voxel.
                // All of its neighbors are in the solution domain.

                // -x direction
                row.setValue(vectorIdx.getValue(ijk.offsetBy(-1, 0, 0)), offDiagonal);
                // -y direction
                row.setValue(vectorIdx.getValue(ijk.offsetBy(0, -1, 0)), offDiagonal);
                // -z direction
                row.setValue(vectorIdx.getValue(ijk.offsetBy(0, 0, -1)), offDiagonal);
                // diagonal
                row.setValue(rowNum, diagonal);
                // +z direction
                row.setValue(vectorIdx.getValue(ijk.offsetBy(0, 0, 1)), offDiagonal);
                // +y direction
                row.setValue(vectorIdx.getValue(ijk.offsetBy(0, 1, 0)), offDiagonal);
                // +x direction
                row.setValue(vectorIdx.getValue(ijk.offsetBy(1, 0, 0)), offDiagonal);

            } else {
                // The current voxel is a boundary voxel.
                // At least one of its neighbors is outside the solution domain.

                ValueT modifiedDiagonal = 0.f;

                // -x direction
                if (vectorIdx.probeValue(ijk.offsetBy(-1, 0, 0), column)) {
                    row.setValue(column, offDiagonal);
                    modifiedDiagonal -= 1;
                } else {
                    boundaryOp(ijk, ijk.offsetBy(-1, 0, 0), source->at(rowNum), modifiedDiagonal);
                }
                // -y direction
                if (vectorIdx.probeValue(ijk.offsetBy(0, -1, 0), column)) {
                    row.setValue(column, offDiagonal);
                    modifiedDiagonal -= 1;
                } else {
                    boundaryOp(ijk, ijk.offsetBy(0, -1, 0), source->at(rowNum), modifiedDiagonal);
                }
                // -z direction
                if (vectorIdx.probeValue(ijk.offsetBy(0, 0, -1), column)) {
                    row.setValue(column, offDiagonal);
                    modifiedDiagonal -= 1;
                } else {
                    boundaryOp(ijk, ijk.offsetBy(0, 0, -1), source->at(rowNum), modifiedDiagonal);
                }
                // +z direction
                if (vectorIdx.probeValue(ijk.offsetBy(0, 0, 1), column)) {
                    row.setValue(column, offDiagonal);
                    modifiedDiagonal -= 1;
                } else {
                    boundaryOp(ijk, ijk.offsetBy(0, 0, 1), source->at(rowNum), modifiedDiagonal);
                }
                // +y direction
                if (vectorIdx.probeValue(ijk.offsetBy(0, 1, 0), column)) {
                    row.setValue(column, offDiagonal);
                    modifiedDiagonal -= 1;
                } else {
                    boundaryOp(ijk, ijk.offsetBy(0, 1, 0), source->at(rowNum), modifiedDiagonal);
                }
                // +x direction
                if (vectorIdx.probeValue(ijk.offsetBy(1, 0, 0), column)) {
                    row.setValue(column, offDiagonal);
                    modifiedDiagonal -= 1;
                } else {
                    boundaryOp(ijk, ijk.offsetBy(1, 0, 0), source->at(rowNum), modifiedDiagonal);
                }
                // diagonal
                row.setValue(rowNum, modifiedDiagonal);
            }
        } // end loop over voxels
    }
};

} // namespace internal


template<typename BoolTreeType>
inline LaplacianMatrix::Ptr
createISLaplacian(const typename BoolTreeType::template ValueConverter<VIndex>::Type& idxTree,
    const BoolTreeType& interiorMask)
{
    typedef LaplacianMatrix::ValueType ValueT;
    math::pcg::Vector<ValueT> unused(
        static_cast<math::pcg::SizeType>(idxTree.activeVoxelCount()));
    internal::DirichletOp<ValueT> op;
    return createISLaplacianWithBoundaryConditions(idxTree, interiorMask, op, unused);
}


template<typename BoolTreeType, typename BoundaryOp>
inline LaplacianMatrix::Ptr
createISLaplacianWithBoundaryConditions(
    const typename BoolTreeType::template ValueConverter<VIndex>::Type& idxTree,
    const BoolTreeType& interiorMask,
    const BoundaryOp& boundaryOp,
    typename math::pcg::Vector<LaplacianMatrix::ValueType>& source)
{
    typedef LaplacianMatrix::ValueType                   ValueT;
    typedef typename BoolTreeType::template ValueConverter<VIndex>::Type VIdxTreeT;
    typedef typename tree::LeafManager<const VIdxTreeT>  VIdxLeafMgrT;
    typedef typename VIdxTreeT::LeafNodeType             VIdxLeafT;

    // The number of active voxels is the number of degrees of freedom.
    const Index64 numDoF = idxTree.activeVoxelCount();

    // Construct the matrix.
    LaplacianMatrix::Ptr laplacianPtr(
        new LaplacianMatrix(static_cast<math::pcg::SizeType>(numDoF)));
    LaplacianMatrix& laplacian = *laplacianPtr;

    // Populate the matrix using a second-order, 7-point CD stencil.
    VIdxLeafMgrT idxLeafManager(idxTree);
    idxLeafManager.foreach(internal::ISLaplacianOp<BoolTreeType, BoundaryOp>(
        laplacian, idxTree, interiorMask, boundaryOp, source));

    return laplacianPtr;
}


////////////////////////////////////////


template<typename TreeType>
inline typename TreeType::Ptr
solve(const TreeType& inTree, math::pcg::State& state)
{
    util::NullInterrupter interrupter;
    return solve(inTree, state, interrupter);
}


template<typename TreeType, typename Interrupter>
inline typename TreeType::Ptr
solve(const TreeType& inTree, math::pcg::State& state, Interrupter& interrupter)
{
    internal::DirichletOp<LaplacianMatrix::ValueType> boundaryOp;
    return solveWithBoundaryConditions(inTree, boundaryOp, state, interrupter);
}


template<typename TreeType, typename BoundaryOp, typename Interrupter>
inline typename TreeType::Ptr
solveWithBoundaryConditions(const TreeType& inTree, const BoundaryOp& boundaryOp,
    math::pcg::State& state, Interrupter& interrupter)
{
    typedef math::pcg::IncompleteCholeskyPreconditioner<LaplacianMatrix> DefaultPrecondT;
    return solveWithBoundaryConditionsAndPreconditioner<DefaultPrecondT>(
        inTree, boundaryOp, state, interrupter);
}


template<typename PreconditionerType, typename TreeType, typename BoundaryOp, typename Interrupter>
inline typename TreeType::Ptr
solveWithBoundaryConditionsAndPreconditioner(const TreeType& inTree,
    const BoundaryOp& boundaryOp, math::pcg::State& state, Interrupter& interrupter)
{

    return solveWithBoundaryConditionsAndPreconditioner<PreconditionerType>(inTree /*source*/, inTree /*domain mask*/,
                                                                            boundaryOp, state, interrupter);
}

template<typename PreconditionerType, typename TreeType, typename DomainTreeType, typename BoundaryOp, typename Interrupter>
inline typename TreeType::Ptr
solveWithBoundaryConditionsAndPreconditioner(const TreeType& inTree,
                                             const DomainTreeType& domainMask,
                                             const BoundaryOp& boundaryOp,
                                             math::pcg::State& state, Interrupter& interrupter)
{

    typedef typename TreeType::ValueType           TreeValueT;
    typedef LaplacianMatrix::ValueType             VecValueT;
    typedef typename math::pcg::Vector<VecValueT>  VectorT;
    typedef typename TreeType::template ValueConverter<VIndex>::Type  VIdxTreeT;
    typedef typename TreeType::template ValueConverter<bool>::Type    MaskTreeT;

    // 1. Create a mapping from active voxels of the input tree to elements of a vector.
    typename VIdxTreeT::ConstPtr idxTree = createIndexTree(domainMask);

    // 2. Populate a vector with values from the input tree.
    typename VectorT::Ptr b = createVectorFromTree<VecValueT>(inTree, *idxTree);

    // 3. Create a mask of the interior voxels of the input tree (from the densified index tree).
    typename MaskTreeT::Ptr interiorMask(
        new MaskTreeT(*idxTree, /*background=*/false, TopologyCopy()));
    tools::erodeVoxels(*interiorMask, /*iterations=*/1, tools::NN_FACE);

    // 4. Create the Laplacian matrix.
    LaplacianMatrix::Ptr laplacian = createISLaplacianWithBoundaryConditions(
        *idxTree, *interiorMask, boundaryOp, *b);

    // 5. Solve the Poisson equation.
    laplacian->scale(-1.0); // matrix is negative-definite; solve -M x = -b
    b->scale(-1.0);
    typename VectorT::Ptr x(new VectorT(b->size(), zeroVal<VecValueT>()));
    typename math::pcg::Preconditioner<VecValueT>::Ptr precond(
        new PreconditionerType(*laplacian));
    if (!precond->isValid()) {
        precond.reset(new math::pcg::JacobiPreconditioner<LaplacianMatrix>(*laplacian));
    }

    state = math::pcg::solve(*laplacian, *b, *x, *precond, interrupter, state);

    // 6. Populate the output tree with values from the solution vector.
    /// @todo if (state.success) ... ?
    return createTreeFromVector<TreeValueT>(*x, *idxTree, /*background=*/zeroVal<TreeValueT>());
}

} // namespace poisson
} // namespace tools
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb

#endif // OPENVDB_TOOLS_POISSONSOLVER_HAS_BEEN_INCLUDED

///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2012-2015 DreamWorks Animation LLC
//
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
//
// Redistributions of source code must retain the above copyright
// and license notice and the following restrictions and disclaimer.
//
// *     Neither the name of DreamWorks Animation nor the names of
// its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// IN NO EVENT SHALL THE COPYRIGHT HOLDERS' AND CONTRIBUTORS' AGGREGATE
// LIABILITY FOR ALL CLAIMS REGARDLESS OF THEIR BASIS EXCEED US$250.00.
//
///////////////////////////////////////////////////////////////////////////